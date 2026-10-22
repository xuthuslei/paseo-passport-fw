#!/usr/bin/env python3
"""Raw-socket WebSocket framing probe.

Connects to the relay exactly like the firmware (via local tunnel,
SNI=pr-worker.178960.xyz) but parses WS frames by hand to observe the
FRAMING (fin/opcode/payload length per frame) of what the relay actually
delivers -- in particular whether the daemon's first encrypted frame
(the session/status envelope) arrives as ONE frame or fragmented.

Read-only: e2ee handshake + daemon hello, no RPCs.
Also tests whether the status frame is proactive: waits 3 s after
e2ee_ready BEFORE sending the daemon hello and records any frames.
"""
import argparse
import base64
import json
import os
import socket
import ssl
import struct
import sys
import time

from nacl.public import Box, PrivateKey, PublicKey

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ref_probe import parse_offer  # noqa: E402


class RawWS:
    def __init__(self, sock):
        self.sock = sock
        self.buf = b""

    def _readn(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("socket closed")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def read_frame(self):
        h = self._readn(2)
        fin = h[0] >> 7
        op = h[0] & 0x0F
        masked = h[1] >> 7
        ln = h[1] & 0x7F
        if ln == 126:
            ln = struct.unpack(">H", self._readn(2))[0]
        elif ln == 127:
            ln = struct.unpack(">Q", self._readn(8))[0]
        mask = self._readn(4) if masked else None
        payload = self._readn(ln) if ln else b""
        if mask:
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        return fin, op, payload

    def send_frame(self, op, payload):
        mask = os.urandom(4)
        ln = len(payload)
        if ln < 126:
            hdr = bytes([0x80 | op, 0x80 | ln])
        elif ln < 65536:
            hdr = bytes([0x80 | op, 0x80 | 126]) + struct.pack(">H", ln)
        else:
            hdr = bytes([0x80 | op, 0x80 | 127]) + struct.pack(">Q", ln)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(hdr + mask + masked)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--offer", required=True)
    ap.add_argument("--connect-host", default="127.0.0.1")
    ap.add_argument("--connect-port", type=int, default=8443)
    args = ap.parse_args()
    offer = parse_offer(args.offer)
    endpoint = offer["relay"]["endpoint"]
    host = endpoint.rpartition(":")[0]

    raw = socket.create_connection((args.connect_host, args.connect_port), timeout=20)
    ctx = ssl.create_default_context()
    ssock = ctx.wrap_socket(raw, server_hostname=host)
    key = base64.b64encode(os.urandom(16)).decode()
    path = f"/ws?serverId={offer['serverId']}&role=client&v=2"
    req = (f"GET {path} HTTP/1.1\r\nHost: {endpoint}\r\n"
           f"Upgrade: websocket\r\nConnection: Upgrade\r\n"
           f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n")
    ssock.sendall(req.encode())
    resp = b""
    while b"\r\n\r\n" not in resp:
        chunk = ssock.recv(4096)
        if not chunk:
            raise ConnectionError("no handshake response")
        resp += chunk
    status = resp.split(b"\r\n", 1)[0].decode()
    print(f"[wsprobe] handshake: {status}")
    assert "101" in status, status
    ws = RawWS(ssock)
    # any bytes after headers belong to frames (unlikely); ignore edge case
    ws.buf = resp.split(b"\r\n\r\n", 1)[1]

    priv = PrivateKey.generate()
    daemon_pub = PublicKey(base64.b64decode(offer["daemonPublicKeyB64"]))
    box = Box(priv, daemon_pub)
    hello = {"type": "e2ee_hello",
             "key": base64.b64encode(bytes(priv.public_key)).decode()}
    ws.send_frame(0x1, json.dumps(hello).encode())

    ssock.settimeout(10)
    while True:
        fin, op, payload = ws.read_frame()
        print(f"[wsprobe] frame fin={fin} op={op} len={len(payload)}")
        if op == 0x1:
            msg = json.loads(payload)
            if msg.get("type") == "e2ee_ready":
                print("[wsprobe] e2ee_ready")
                break

    # Proactive test: wait 3 s WITHOUT sending daemon hello.
    ssock.settimeout(3)
    proactive = []
    try:
        while True:
            fin, op, payload = ws.read_frame()
            proactive.append((fin, op, len(payload)))
            print(f"[wsprobe] PRE-HELLO frame fin={fin} op={op} len={len(payload)}")
    except (socket.timeout, ssl.SSLError):
        pass
    print(f"[wsprobe] proactive frames before daemon hello: {len(proactive)}")

    data = json.dumps({
        "type": "hello", "clientId": "cid_wsprobe_framing", "clientType": "cli",
        "protocolVersion": 1,
        "capabilities": {"hello_rejection": True,
                         "explicit_event_subscriptions": True}}).encode()
    ws.send_frame(0x1, base64.b64encode(box.encrypt(data)))
    ssock.settimeout(10)
    got = 0
    while got < 2:
        fin, op, payload = ws.read_frame()
        print(f"[wsprobe] POST-HELLO frame fin={fin} op={op} len={len(payload)}")
        if op == 0x1 and not payload.startswith(b"{"):
            plain = box.decrypt(base64.b64decode(payload))
            pj = json.loads(plain.decode())
            inner = pj.get("message", {}).get("type") if isinstance(pj.get("message"), dict) else None
            print(f"[wsprobe]   decrypted ok: plain_len={len(plain)} type={pj.get('type')} inner={inner}")
            got += 1
    ssock.close()


if __name__ == "__main__":
    main()
