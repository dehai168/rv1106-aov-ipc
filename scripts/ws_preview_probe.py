#!/usr/bin/env python3
"""Minimal WS client for preview smoke (no deps)."""
import hashlib
import json
import os
import re
import socket
import struct
import sys
import time
import urllib.request

BASE = os.environ.get("IPC_BASE", "http://127.0.0.1:8080")
USER = os.environ.get("IPC_USER", "admin")
PASS = os.environ.get("IPC_PASS", "123456")
STREAM = os.environ.get("IPC_STREAM", "main")


def login():
    req = urllib.request.Request(
        BASE + "/api/v1/auth/login",
        data=json.dumps({"username": USER, "password": PASS}).encode(),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=5) as r:
        body = r.read().decode()
    m = re.search(r'"token":"([^"]+)"', body)
    if not m:
        raise SystemExit("login failed: " + body)
    return m.group(1)


def ws_recv(sock):
    hdr = sock.recv(2)
    if len(hdr) < 2:
        return None, None
    opcode = hdr[0] & 0x0F
    ln = hdr[1] & 0x7F
    if ln == 126:
        ext = sock.recv(2)
        ln = struct.unpack("!H", ext)[0]
    elif ln == 127:
        ext = sock.recv(8)
        ln = struct.unpack("!Q", ext)[0]
    data = b""
    while len(data) < ln:
        chunk = sock.recv(ln - len(data))
        if not chunk:
            break
        data += chunk
    return opcode, data


def main():
    token = login()
    host = "127.0.0.1"
    port = 8080
    path = f"/api/v1/preview/ws?stream={STREAM}&token={token}"
    key = hashlib.sha1(os.urandom(16)).hexdigest()[:24]
    req = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n"
    )
    s = socket.create_connection((host, port), timeout=8)
    s.sendall(req.encode())
    resp = b""
    while b"\r\n\r\n" not in resp:
        resp += s.recv(4096)
    if b"101" not in resp.split(b"\r\n", 1)[0]:
        print("upgrade fail", resp[:200])
        return 1
    print("ws upgraded stream=", STREAM)
    s.settimeout(10)
    bins = 0
    t0 = time.time()
    while time.time() - t0 < 10:
        try:
            op, data = ws_recv(s)
        except Exception as e:
            print("recv", e)
            break
        if op is None:
            break
        if op == 0x1:
            print("meta", data[:200].decode(errors="replace"))
        elif op == 0x2:
            bins += 1
            if bins <= 3:
                print("bin", len(data), data[:8])
        elif op == 0x8:
            print("close")
            break
    print("bins", bins)
    s.close()
    return 0 if bins > 0 else 2


if __name__ == "__main__":
    sys.exit(main())
