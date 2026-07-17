#!/usr/bin/env python3
"""Dump preview WS init+frags to a .mp4 and optionally ffprobe."""
import http.cookiejar
import json
import struct
import subprocess
import sys
import threading
import time
import urllib.request
from pathlib import Path

try:
    import websocket
except ImportError:
    subprocess.check_call([sys.executable, "-m", "pip", "install", "websocket-client", "-q"])
    import websocket

BASE = "http://127.0.0.1:18080"
WS = "ws://127.0.0.1:18080/api/v1/preview/ws"
OUT = Path(__file__).resolve().parents[1] / "build" / "preview_dump.mp4"


def login(opener):
    for pw in ("admin1", "admin"):
        req = urllib.request.Request(
            BASE + "/api/v1/auth/login",
            data=json.dumps({"username": "admin", "password": pw}).encode(),
            headers={"Content-Type": "application/json"},
        )
        try:
            body = opener.open(req, timeout=10).read().decode()
            if '"token"' in body:
                print("login ok", pw)
                return True
        except Exception as e:
            print("login", pw, e)
    return False


def main() -> int:
    cj = http.cookiejar.CookieJar()
    opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(cj))
    if not login(opener):
        return 1
    token = next((c.value for c in cj if c.name == "ipc_token"), None)

    parts: list[bytes] = []
    codec = {"v": None}
    done = threading.Event()

    def on_msg(_ws, msg):
        if isinstance(msg, str):
            print("text", msg)
            codec["v"] = json.loads(msg).get("codec")
            return
        parts.append(msg)
        print(f"bin#{len(parts)} len={len(msg)} head={msg[:8].hex()}")
        if len(parts) >= 30:
            done.set()
            _ws.close()

    ws = websocket.WebSocketApp(
        WS,
        cookie=f"ipc_token={token}",
        on_message=on_msg,
        on_error=lambda _w, e: print("err", e),
        on_open=lambda _w: print("ws open"),
    )
    threading.Thread(target=ws.run_forever, daemon=True).start()
    done.wait(timeout=45)
    ws.close()

    if len(parts) < 2:
        print("FAIL: not enough parts", len(parts))
        return 2

    OUT.parent.mkdir(parents=True, exist_ok=True)
    blob = b"".join(parts)
    OUT.write_bytes(blob)
    print("wrote", OUT, "bytes", len(blob), "codec", codec["v"])

    # Parse top-level boxes
    off = 0
    while off + 8 <= len(blob):
        size = struct.unpack(">I", blob[off : off + 4])[0]
        typ = blob[off + 4 : off + 8].decode("ascii", "replace")
        if size < 8 or off + size > len(blob):
            print("box truncate", typ, size, "at", off)
            break
        print(f"box @{off}: {typ} size={size}")
        off += size

    try:
        r = subprocess.run(
            ["ffprobe", "-v", "error", "-show_streams", "-show_format", str(OUT)],
            capture_output=True,
            text=True,
            timeout=30,
        )
        print("ffprobe rc", r.returncode)
        print(r.stdout or r.stderr)
    except FileNotFoundError:
        print("ffprobe not found; skip")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
