#!/usr/bin/env python3
"""解密二分探针：抓取与固件同一路径的第一帧真实密文。

与 ref_client.py 相同的连接方式（本地隧道 127.0.0.1:8443 落点、
SNI=offer endpoint 主机名），但只做只读动作：relay WS -> e2ee 握手
-> 发 daemon hello -> 捕获 hello 之后的第一帧密文（原文 base64），
用 PyNaCl Box 解密，并把复现夹具所需的四样数据写入 probe_out.json：
本会话私钥 b64、daemon 公钥 b64（offer)、第一帧密文 b64 原文、明文。

probe_out.json 含会话私钥，只留本机 tools/ 目录，勿外传。
用法： python3 ref_probe.py --offer 'https://app.paseo.sh/#offer=...'
"""
import argparse
import asyncio
import base64
import json
import os
import socket
import ssl

import websockets
from nacl.public import Box, PrivateKey, PublicKey


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


async def run(offer, connect_host, connect_port, out_path):
    endpoint = offer["relay"]["endpoint"]
    host, _, port = endpoint.rpartition(":")
    port = int(port)
    use_tls = offer["relay"].get("useTls", port == 443)
    scheme = "wss" if use_tls else "ws"
    uri = f"{scheme}://{endpoint}/ws?serverId={offer['serverId']}&role=client&v=2"
    chost = connect_host or host
    cport = connect_port or port
    raw = socket.create_connection((chost, cport), timeout=20)
    kwargs = dict(ping_interval=None, max_size=None, sock=raw)
    if use_tls:
        kwargs["ssl"] = ssl.create_default_context()
        kwargs["server_hostname"] = host
    ws = await websockets.connect(uri, **kwargs)
    print(f"[probe] WS connected -> {endpoint} (tcp {chost}:{cport})")

    priv = PrivateKey.generate()
    daemon_pub = PublicKey(base64.b64decode(offer["daemonPublicKeyB64"]))
    box = Box(priv, daemon_pub)
    hello = {"type": "e2ee_hello",
             "key": base64.b64encode(bytes(priv.public_key)).decode()}
    await ws.send(json.dumps(hello))
    while True:
        frame = await asyncio.wait_for(ws.recv(), timeout=20)
        if isinstance(frame, bytes):
            continue
        msg = json.loads(frame)
        if msg.get("type") == "e2ee_ready":
            print(f"[probe] e2ee_ready (capabilities={msg.get('capabilities')})")
            break

    # 与固件相同的下一步：发 daemon hello，然后等第一帧密文。
    data = json.dumps({
        "type": "hello", "clientId": "cid_refprobe_bisect", "clientType": "cli",
        "protocolVersion": 1,
        "capabilities": {"hello_rejection": True,
                         "explicit_event_subscriptions": True}}).encode()
    await ws.send(base64.b64encode(box.encrypt(data)).decode())

    frames = []
    first_plain = None
    try:
        while len(frames) < 3:
            frame = await asyncio.wait_for(ws.recv(), timeout=10)
            if isinstance(frame, bytes):
                frames.append({"kind": "binary", "len": len(frame)})
                continue
            raw_bytes = base64.b64decode(frame)
            plain = box.decrypt(raw_bytes)
            entry = {"kind": "text",
                     "b64_len": len(frame),
                     "raw_len": len(raw_bytes),
                     "plain_len": len(plain)}
            try:
                pj = json.loads(plain.decode("utf-8"))
                entry["type"] = pj.get("type")
                if isinstance(pj.get("message"), dict):
                    entry["inner_type"] = pj["message"].get("type")
            except Exception:  # noqa: BLE001
                entry["type"] = "<non-json>"
            frames.append(entry)
            if first_plain is None:
                first_plain = plain
                first_frame_b64 = frame
    except asyncio.TimeoutError:
        pass

    assert first_plain is not None, "no encrypted frame captured"
    out = {
        "dev_priv_b64": base64.b64encode(bytes(priv)).decode(),
        "dev_pub_b64": base64.b64encode(bytes(priv.public_key)).decode(),
        "daemon_pub_b64": offer["daemonPublicKeyB64"],
        "frame_b64": first_frame_b64,
        "plain": first_plain.decode("utf-8"),
        "frames_meta": frames,
    }
    with open(out_path, "w") as f:
        json.dump(out, f)
    await ws.close()
    print(f"[probe] captured {len(frames)} frame(s); first: "
          f"b64_len={frames[0].get('b64_len')} raw_len={frames[0].get('raw_len')} "
          f"plain_len={frames[0].get('plain_len')} type={frames[0].get('type')} "
          f"inner={frames[0].get('inner_type')}")
    print(f"[probe] wrote {out_path}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--offer", required=True)
    ap.add_argument("--connect-host", default="127.0.0.1")
    ap.add_argument("--connect-port", type=int, default=8443)
    ap.add_argument("--out", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "probe_out.json"))
    args = ap.parse_args()
    offer = parse_offer(args.offer)
    print(f"offer: serverId={offer['serverId']} relay={offer['relay']}")
    asyncio.run(run(offer, args.connect_host, args.connect_port, args.out))


if __name__ == "__main__":
    main()
