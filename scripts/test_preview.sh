#!/bin/sh
# Smoke: login + WS preview receives codec text + binary init.
set -e
printf '%s' '{"username":"admin","password":"admin"}' > /tmp/login_ok.json
wget -qO- --post-file=/tmp/login_ok.json --header='Content-Type: application/json' \
  http://127.0.0.1:8080/api/v1/auth/login > /tmp/login_resp.json
TOKEN=$(sed -n 's/.*"token":"\([^"]*\)".*/\1/p' /tmp/login_resp.json)
echo "token=${TOKEN:0:16}..."

# If must_change, clear it via password API with same password family
MUST=$(sed -n 's/.*"must_change":\([^,}]*\).*/\1/p' /tmp/login_resp.json)
if [ "$MUST" = "true" ]; then
  printf '%s' '{"old_password":"admin","new_password":"admin1"}' > /tmp/pw.json
  wget -qO- --post-file=/tmp/pw.json --header='Content-Type: application/json' \
    --header="Authorization: Bearer $TOKEN" \
    http://127.0.0.1:8080/api/v1/auth/password || true
  printf '%s' '{"username":"admin","password":"admin1"}' > /tmp/login_ok.json
  wget -qO- --post-file=/tmp/login_ok.json --header='Content-Type: application/json' \
    http://127.0.0.1:8080/api/v1/auth/login > /tmp/login_resp.json
  TOKEN=$(sed -n 's/.*"token":"\([^"]*\)".*/\1/p' /tmp/login_resp.json)
fi

# Use python if available for websocket; else dump first WS frames via busybox-unusable.
# Fallback: check that media starts when WS connects using a tiny C helper is heavy.
# Prefer websocat / python.
if command -v python3 >/dev/null 2>&1; then
  python3 - <<PY
import json, struct, urllib.request, ssl, sys
try:
    import websocket
except ImportError:
    # stdlib-only minimal WS client
    import base64, hashlib, os, socket
    token = open("/tmp/login_resp.json").read()
    import re
    m = re.search(r'"token":"([^"]+)"', token)
    tok = m.group(1)
    key = base64.b64encode(os.urandom(16)).decode()
    req = (
        "GET /api/v1/preview/ws?token=%s HTTP/1.1\r\n"
        "Host: 127.0.0.1:8080\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n"
    ) % (tok, key)
    s = socket.create_connection(("127.0.0.1", 8080), timeout=10)
    s.sendall(req.encode())
    hdr = b""
    while b"\r\n\r\n" not in hdr:
        hdr += s.recv(1)
    print("ws handshake:", hdr.split(b"\r\n",1)[0].decode())
    # read a few frames
    got_text = False
    got_bin = False
    deadline = __import__("time").time() + 20
    while __import__("time").time() < deadline and not (got_text and got_bin):
        h = s.recv(2)
        if len(h) < 2:
            break
        opcode = h[0] & 0x0f
        ln = h[1] & 0x7f
        if ln == 126:
            ext = s.recv(2)
            ln = int.from_bytes(ext, "big")
        elif ln == 127:
            ext = s.recv(8)
            ln = int.from_bytes(ext, "big")
        data = b""
        while len(data) < ln:
            data += s.recv(ln - len(data))
        if opcode == 1:
            print("TEXT:", data[:120])
            got_text = True
        elif opcode == 2:
            print("BIN len=", len(data), "head=", data[:8].hex())
            got_bin = True
    s.close()
    if not got_text or not got_bin:
        print("FAIL: need codec text + init binary")
        sys.exit(2)
    print("OK preview frames")
PY
else
  echo "no python3; skip WS frame dump"
fi
