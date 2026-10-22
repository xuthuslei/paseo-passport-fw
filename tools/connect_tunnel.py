#!/usr/bin/env python3
"""本地 CONNECT 隧道：监听 127.0.0.1:8443，把每个 TCP 连接经 $https_proxy 的
CONNECT 转发到 pr-worker.178960.xyz:443，再双向转发原始字节。

用途：本机出网走 CONNECT 代理，Node 的 ws 直连外网 TLS 会被代理干扰
（wrong version number）。daemon 与参考客户端把 pr-worker.178960.xyz
解析到 127.0.0.1（/etc/hosts），连到本隧道后拿到真正的 Cloudflare TLS 流，
SNI/证书仍是 pr-worker.178960.xyz，端到端 TLS 不受影响。

启动：nohup python3 connect_tunnel.py > tunnel.log 2>&1 &
环境变量可覆盖：TUNNEL_LISTEN_PORT（默认 8443）、TUNNEL_TARGET（默认
pr-worker.178960.xyz:443）、https_proxy（必需，含认证）。
"""
import asyncio
import base64
import os
import sys
from urllib.parse import urlparse

LISTEN_HOST = "127.0.0.1"
LISTEN_PORT = int(os.environ.get("TUNNEL_LISTEN_PORT", "8443"))
TARGET = os.environ.get("TUNNEL_TARGET", "pr-worker.178960.xyz:443")


def proxy_parts():
    raw = os.environ.get("https_proxy") or os.environ.get("HTTPS_PROXY")
    if not raw:
        raise SystemExit("https_proxy not set")
    u = urlparse(raw)
    auth = ""
    if u.username:
        cred = f"{u.username}:{u.password or ''}".encode()
        auth = "Proxy-Authorization: Basic " + base64.b64encode(cred).decode() + "\r\n"
    return u.hostname, u.port or 3128, auth


async def pipe(reader, writer):
    try:
        while True:
            data = await reader.read(65536)
            if not data:
                break
            writer.write(data)
            await writer.drain()
    except (ConnectionResetError, BrokenPipeError):
        pass
    finally:
        try:
            writer.close()
        except Exception:
            pass


async def handle(client_r, client_w):
    phost, pport, auth = proxy_parts()
    try:
        proxy_r, proxy_w = await asyncio.open_connection(phost, pport)
    except Exception as e:
        print(f"tunnel: proxy connect failed: {e}", flush=True)
        client_w.close()
        return
    req = (
        f"CONNECT {TARGET} HTTP/1.1\r\nHost: {TARGET}\r\n{auth}"
        "Proxy-Connection: keep-alive\r\n\r\n"
    )
    proxy_w.write(req.encode())
    await proxy_w.drain()
    # 读到 header 结束为止（注意可能多读到隧道字节，需保留）
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = await proxy_r.read(4096)
        if not chunk:
            print("tunnel: proxy closed during CONNECT", flush=True)
            client_w.close(); proxy_w.close()
            return
        buf += chunk
    head, _, rest = buf.partition(b"\r\n\r\n")
    status_line = head.split(b"\r\n")[0].decode("latin1", "replace")
    if " 200 " not in status_line and not status_line.endswith(" 200"):
        print(f"tunnel: CONNECT rejected: {status_line}", flush=True)
        client_w.close(); proxy_w.close()
        return
    if rest:
        client_w.write(rest)
        await client_w.drain()
    await asyncio.gather(pipe(client_r, proxy_w), pipe(proxy_r, client_w))


async def main():
    server = await asyncio.start_server(handle, LISTEN_HOST, LISTEN_PORT)
    print(f"tunnel: listening on {LISTEN_HOST}:{LISTEN_PORT} -> CONNECT -> {TARGET}", flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
