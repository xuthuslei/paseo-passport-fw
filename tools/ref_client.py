#!/usr/bin/env python3
"""Paseo 协议子集 PC 参考客户端（0.5 期）。

实现 PROTOCOL_SUBSET.md 的完整子集，作为 ESP32-C3 固件的行为参照：
  offer 解析 -> relay WS (v2) -> e2ee_hello/e2ee_ready -> NaCl box 加密通道
  （hello 不带 capabilities，全程 base64 文本帧，与固件选型一致）
  -> daemon hello -> server_info -> fetch_agents（带目录订阅）
  -> session.events.set_subscription（四事件）-> agent.create / send_agent_message
  -> 权限自动批准（--auto-approve）-> fetch_agent_timeline(tail, projected)
  -> 应用层 ping/pong 心跳；外加一条不发 ping 的探针连接验证服务端容忍度。

用法：
  python3 ref_client.py --pair-file /tmp/pair.json [--create] [--no-probe]
  python3 ref_client.py --offer 'https://app.paseo.sh/#offer=...' [--create]
  python3 ref_client.py --home ~/workspace/paseo-dev-daemon/home [--create]

开发链路注意：本机出网走 CONNECT 代理、且域名被 fake-IP 解析劫持，
daemon 与本客户端都经本地隧道（connect_tunnel.py, 127.0.0.1:8443）连
pr-worker。--connect-host/--connect-port 可覆盖实际 TCP 落点，SNI 与
offer 中的 endpoint 保持一致。样本帧（打码后）存到 --samples-dir。
"""
import argparse
import asyncio
import base64
import json
import os
import socket
import ssl
import subprocess
import sys
import time
import uuid

import websockets
from nacl.public import Box, PrivateKey, PublicKey

REDACT_KEYS = {"key", "token", "password", "secret", "daemonpublickeyb64",
               "localcredential", "authorization"}
MAX_STR = 1500


def b64url_decode(s: str) -> bytes:
    s = s.strip().replace("-", "+").replace("_", "/")
    s += "=" * (-len(s) % 4)
    return base64.b64decode(s)


def parse_offer(text: str) -> dict:
    text = text.strip()
    if "#offer=" in text:
        text = text.split("#offer=", 1)[1]
    offer = json.loads(b64url_decode(text).decode("utf-8"))
    assert offer.get("v") == 2, f"unsupported offer version: {offer.get('v')}"
    return offer


def offer_from_pair_file(path: str) -> dict:
    d = json.load(open(path))
    return parse_offer(d["url"])


def offer_from_home(home: str) -> dict:
    cli = os.path.join(os.path.dirname(os.path.abspath(home)), "node_modules", ".bin", "paseo")
    out = subprocess.run([cli, "pair", "--json", "--home", home],
                         capture_output=True, text=True, timeout=30)
    if out.returncode != 0:
        raise SystemExit(f"paseo pair failed: {out.stderr[:300]}")
    return parse_offer(json.loads(out.stdout)["url"])


def sanitize(obj, depth=0):
    if isinstance(obj, dict):
        return {k: ("<redacted>" if k.lower() in REDACT_KEYS else sanitize(v, depth + 1))
                for k, v in obj.items()}
    if isinstance(obj, list):
        return [sanitize(v, depth + 1) for v in obj[:50]]
    if isinstance(obj, str) and len(obj) > MAX_STR:
        return obj[:MAX_STR] + f"...<truncated {len(obj) - MAX_STR} chars>"
    return obj


class SampleStore:
    def __init__(self, directory):
        self.dir = directory
        self.seen = set()
        if directory:
            os.makedirs(directory, exist_ok=True)

    def save(self, name: str, obj):
        if not self.dir or name in self.seen:
            return
        self.seen.add(name)
        fname = name.replace(".", "_").replace("/", "_") + ".json"
        with open(os.path.join(self.dir, fname), "w") as f:
            json.dump(sanitize(obj), f, ensure_ascii=False, indent=1)


class Channel:
    """一条 relay WS + E2EE 加密通道。"""

    def __init__(self, offer, connect_host, connect_port, label, samples: SampleStore):
        self.offer = offer
        self.connect_host = connect_host
        self.connect_port = connect_port
        self.label = label
        self.samples = samples
        self.ws = None
        self.box = None
        self.pending = {}          # requestId -> Future
        self.msg_counts = {}
        self.on_message = None     # async callback(msg dict)
        self.reader_task = None
        self.server_info_before_subscribe = None

    async def connect(self):
        endpoint = self.offer["relay"]["endpoint"]
        host, _, port = endpoint.rpartition(":")
        port = int(port)
        use_tls = self.offer["relay"].get("useTls", port == 443)
        scheme = "wss" if use_tls else "ws"
        uri = (f"{scheme}://{endpoint}/ws?serverId={self.offer['serverId']}"
               f"&role=client&v=2")
        chost = self.connect_host or host
        cport = self.connect_port or port
        raw = socket.create_connection((chost, cport), timeout=20)
        kwargs = dict(ping_interval=None, max_size=None, sock=raw)
        if use_tls:
            kwargs["ssl"] = ssl.create_default_context()
            kwargs["server_hostname"] = host
        self.ws = await websockets.connect(uri, **kwargs)
        print(f"[{self.label}] WS connected -> {endpoint} (tcp {chost}:{cport})")

        # --- E2EE handshake (plaintext frames) ---
        self._priv = PrivateKey.generate()
        daemon_pub = PublicKey(base64.b64decode(self.offer["daemonPublicKeyB64"]))
        self.box = Box(self._priv, daemon_pub)
        hello = {"type": "e2ee_hello",
                 "key": base64.b64encode(bytes(self._priv.public_key)).decode()}
        self.samples.save("e2ee_hello", hello)
        await self.ws.send(json.dumps(hello))
        while True:
            frame = await asyncio.wait_for(self.ws.recv(), timeout=20)
            if isinstance(frame, bytes):
                continue
            msg = json.loads(frame)
            self._count(msg.get("type", "?"))
            if msg.get("type") == "e2ee_ready":
                self.samples.save("e2ee_ready", msg)
                print(f"[{self.label}] e2ee_ready (capabilities="
                      f"{msg.get('capabilities')})")
                break
        self.reader_task = asyncio.create_task(self._reader())

    def _count(self, t):
        self.msg_counts[t] = self.msg_counts.get(t, 0) + 1

    async def _reader(self):
        try:
            async for frame in self.ws:
                if isinstance(frame, bytes):
                    self._count("<binary-frame>")
                    continue
                plain = self.box.decrypt(base64.b64decode(frame)).decode("utf-8")
                msg = json.loads(plain)
                await self._dispatch(msg)
        except (websockets.ConnectionClosed, Exception) as e:  # noqa: BLE001
            print(f"[{self.label}] reader ended: {type(e).__name__} {e}")
        finally:
            for fut in list(self.pending.values()):
                if not fut.done():
                    fut.set_exception(ConnectionError("channel closed"))
            self.pending.clear()

    async def _dispatch(self, msg):
        t = msg.get("type", "?")
        inner = msg.get("message") if t == "session" else None
        itype = inner.get("type") if isinstance(inner, dict) else None
        self._count(itype or t)
        if itype:
            self.samples.save(itype, msg)
        else:
            self.samples.save(t, msg)
        payload = (inner or {}).get("payload") if isinstance(inner, dict) else None
        rid = payload.get("requestId") if isinstance(payload, dict) else None
        if rid and rid in self.pending:
            self.pending.pop(rid).set_result(msg)
        if self.on_message:
            await self.on_message(msg)

    async def send_json(self, obj):
        data = json.dumps(obj).encode("utf-8")
        token = base64.b64encode(self.box.encrypt(data)).decode()
        await self.ws.send(token)

    async def rpc(self, inner: dict, timeout=60):
        rid = inner.setdefault("requestId", uuid.uuid4().hex)
        fut = asyncio.get_running_loop().create_future()
        self.pending[rid] = fut
        await self.send_json({"type": "session", "message": inner})
        try:
            return await asyncio.wait_for(fut, timeout)
        finally:
            self.pending.pop(rid, None)

    async def hello(self, client_id):
        await self.send_json({
            "type": "hello", "clientId": client_id, "clientType": "cli",
            "protocolVersion": 1,
            "capabilities": {"hello_rejection": True,
                             "explicit_event_subscriptions": True}})

    async def ping_loop(self):
        while True:
            await asyncio.sleep(10)
            await self.send_json({"type": "ping"})

    async def close(self):
        if self.reader_task:
            self.reader_task.cancel()
        if self.ws:
            await self.ws.close()


FOUR_EVENTS = ["agent_attention_required", "agent_permission_request",
               "agent_permission_resolved", "status.server_info"]


def inner_type(msg):
    if msg.get("type") == "session" and isinstance(msg.get("message"), dict):
        return msg["message"].get("type")
    return None


def inner_payload(msg):
    if msg.get("type") == "session" and isinstance(msg.get("message"), dict):
        return msg["message"].get("payload") or {}
    return {}


def describe_item(item, depth=0):
    """打印 timeline item 的结构（键与类型），定位 assistant 文本路径。"""
    if isinstance(item, dict):
        keys = {k: type(v).__name__ for k, v in item.items()}
        return keys
    return type(item).__name__


async def wait_agent_settled(channel: Channel, agent_id: str, timeout=240):
    """等 agent 回到 idle（或 error/closed）。靠 agent_status/agent_update 推送
    + 周期性 fetch_agent_request 兜底。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        resp = await channel.rpc({"type": "fetch_agent_request",
                                  "agentId": agent_id}, timeout=30)
        agent = inner_payload(resp).get("agent") or {}
        status = agent.get("status")
        pending = agent.get("pendingPermissions") or []
        print(f"  [poll] agent status={status} pendingPermissions={len(pending)}")
        if status in ("idle", "error", "closed") and not pending:
            return agent
        await asyncio.sleep(5)
    raise TimeoutError(f"agent {agent_id} did not settle in {timeout}s")


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--offer")
    ap.add_argument("--pair-file")
    ap.add_argument("--home")
    ap.add_argument("--connect-host", default="127.0.0.1",
                    help="实际 TCP 落点（缺省走本地隧道 127.0.0.1）")
    ap.add_argument("--connect-port", type=int, default=8443)
    ap.add_argument("--direct", action="store_true",
                    help="不经隧道：按 offer endpoint 直连")
    ap.add_argument("--create", action="store_true", help="建一个 codex agent 并跑一轮")
    ap.add_argument("--auto-approve", action="store_true", default=True)
    ap.add_argument("--no-probe", action="store_true")
    ap.add_argument("--probe-seconds", type=int, default=90)
    ap.add_argument("--samples-dir",
                    default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                         "..", "research", "samples"))
    args = ap.parse_args()

    if args.offer:
        offer = parse_offer(args.offer)
    elif args.pair_file:
        offer = offer_from_pair_file(args.pair_file)
    elif args.home:
        offer = offer_from_home(args.home)
    else:
        ap.error("one of --offer/--pair-file/--home is required")
    print(f"offer: serverId={offer['serverId']} relay={offer['relay']}")

    cho = None if args.direct else args.connect_host
    cpo = None if args.direct else args.connect_port
    samples = SampleStore(os.path.abspath(args.samples_dir))

    ch = Channel(offer, cho, cpo, "main", samples)
    state = {"agent_id": None, "saw_server_info_pre_subscribe": False,
             "permission_requests": 0, "attention": []}

    async def on_msg(msg):
        t = inner_type(msg)
        p = inner_payload(msg)
        if t == "status" and p.get("status") == "server_info":
            print(f"  [evt] server_info: serverId={p.get('serverId')} "
                  f"version={p.get('version')} protocol={p.get('protocolVersion')}")
        elif t == "agent_permission_request":
            state["permission_requests"] += 1
            req = p.get("request") or {}
            print(f"  [evt] PERMISSION agent={p.get('agentId')} name={req.get('name')} "
                  f"kind={req.get('kind')} title={req.get('title')}")
            if args.auto_approve and p.get("agentId") and req.get("id"):
                await ch.send_json({"type": "session", "message": {
                    "type": "agent_permission_response",
                    "agentId": p["agentId"], "requestId": req["id"],
                    "response": {"behavior": "allow"}}})
                print("  [act] auto-approved")
        elif t == "agent_attention_required":
            state["attention"].append(p)
            print(f"  [evt] attention agent={p.get('agentId')} reason={p.get('reason')}")
        elif t in ("agent_update", "agent_status"):
            a = p.get("agent") or p.get("info") or {}
            print(f"  [evt] {t}: agent={a.get('id', p.get('agentId'))} "
                  f"status={a.get('status', p.get('status'))}")
        elif t and t not in ("status",):
            print(f"  [evt] {t}")

    ch.on_message = on_msg
    await ch.connect()
    await ch.hello("cid_refclient_passport_05")

    # --- 未核实 #1：server_info 是否在订阅前主动到达 ---
    try:
        await asyncio.wait_for(asyncio.sleep(5), timeout=5)
    except asyncio.TimeoutError:
        pass
    pre = any(True for _ in [0]) and ch.msg_counts.get("status", 0) > 0
    state["saw_server_info_pre_subscribe"] = pre
    print(f"[check#1] server_info arrived before any subscription: {pre}")

    ping_task = asyncio.create_task(ch.ping_loop())

    # --- 显式事件订阅（四事件） ---
    resp = await ch.rpc({"type": "session.events.set_subscription.request",
                         "events": FOUR_EVENTS})
    print(f"[rpc] set_subscription -> {inner_type(resp)} "
          f"payload keys={list(inner_payload(resp).keys())}")

    # --- fetch_agents（带目录订阅） ---
    resp = await ch.rpc({"type": "fetch_agents_request",
                         "scope": "active",
                         "page": {"limit": 20},
                         "subscribe": {"subscriptionId": uuid.uuid4().hex}})
    entries = inner_payload(resp).get("entries") or []
    print(f"[rpc] fetch_agents_response: {len(entries)} agent(s)")
    for e in entries:
        a = e.get("agent") or {}
        print(f"  - {a.get('id')} provider={a.get('provider')} "
              f"status={a.get('status')} cwd={a.get('cwd')}")

    agent_id = None
    if args.create:
        resp = await ch.rpc({
            "type": "agent.create.request",
            "config": {"provider": "codex", "cwd": "/tmp"},
            "initialPrompt": ("Run the shell command `pwd && echo PASEO_REF_OK` "
                              "and report its exact output."),
            "idempotencyKey": uuid.uuid4().hex}, timeout=120)
        p = inner_payload(resp)
        agent = p.get("agent") or {}
        agent_id = agent.get("id")
        print(f"[rpc] agent.create.response: id={agent_id} "
              f"status={agent.get('status')} error={p.get('error')}")
        if agent_id:
            state["agent_id"] = agent_id
            await wait_agent_settled(ch, agent_id)

            # --- 未核实 #3：send_agent_message 的 attachments 给 [] ---
            resp = await ch.rpc({
                "type": "send_agent_message_request", "agentId": agent_id,
                "text": ("Run the shell command `echo PASEO_FOLLOWUP_OK` "
                         "and report its exact output."),
                "messageId": uuid.uuid4().hex, "attachments": []}, timeout=60)
            p = inner_payload(resp)
            print(f"[rpc] send_agent_message_response: accepted={p.get('accepted')} "
                  f"deliveryStatus={p.get('deliveryStatus')} error={p.get('error')}")
            print("[check#3] attachments: [] accepted by daemon"
                  if p.get("accepted") else "[check#3] attachments: [] NOT accepted")
            await wait_agent_settled(ch, agent_id)

            # --- timeline 尾部（未核实 #5：item 结构） ---
            resp = await ch.rpc({
                "type": "fetch_agent_timeline_request", "agentId": agent_id,
                "direction": "tail", "limit": 10,
                "projection": "projected"}, timeout=60)
            p = inner_payload(resp)
            tl_entries = p.get("entries") or []
            print(f"[rpc] fetch_agent_timeline_response: {len(tl_entries)} entries, "
                  f"window={p.get('window')}")
            for e in tl_entries:
                item = e.get("item") or {}
                print(f"  entry item.type={item.get('type')} keys={describe_item(item)}")
                if item.get("type") == "assistant_message":
                    print(f"    assistant text: {str(item.get('text'))[:200]!r}")

    # --- 未核实 #6：不发 ping 的探针连接 ---
    if not args.no_probe:
        probe = Channel(offer, cho, cpo, "probe", samples)
        await probe.connect()
        await probe.hello("cid_refclient_probe_noping")
        # 必须等 hello 被 daemon 接受（server_info 到达）后再发 RPC：
        # pending 阶段的非 hello 报文会被 daemon 以 "Invalid hello" 拒绝并关连接
        # （实测踩坑，见 research/UNVERIFIED_RESOLVED.md #6）。
        await asyncio.sleep(3)
        r = await probe.rpc({"type": "fetch_agents_request", "page": {"limit": 1}})
        print(f"[probe] initial fetch ok: {inner_type(r)}; now idling "
              f"{args.probe_seconds}s WITHOUT app ping...")
        await asyncio.sleep(args.probe_seconds)
        try:
            r = await probe.rpc({"type": "fetch_agents_request",
                                 "page": {"limit": 1}}, timeout=20)
            print(f"[check#6] after {args.probe_seconds}s no-ping idle, fetch "
                  f"still works: {inner_type(r)} (no server liveness cutoff "
                  f"within window)")
        except Exception as e:  # noqa: BLE001
            print(f"[check#6] no-ping probe FAILED after idle: "
                  f"{type(e).__name__} {e}")
        await probe.close()

    ping_task.cancel()
    print(f"[summary] main channel message counts: "
          f"{json.dumps(ch.msg_counts, ensure_ascii=False)}")
    print(f"[summary] permission requests seen: {state['permission_requests']}, "
          f"attention events: {len(state['attention'])}")
    await ch.close()


if __name__ == "__main__":
    asyncio.run(main())
