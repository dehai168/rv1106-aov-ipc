#!/usr/bin/env python3
"""Smoke-test /api/v1/preview/ws via adb forward (tcp:18080 -> 8080)."""
import json
import threading
import time
import urllib.request
import http.cookiejar

try:
    import websocket
except ImportError:
    import subprocess
    import sys

    subprocess.check_call([sys.executable, "-m", "pip", "install", "websocket-client", "-q"])
    import websocket

BASE = "http://127.0.0.1:18080"
WS_URL = "ws://127.0.0.1:18080/api/v1/preview/ws"


def main() -> int:
    cj = http.cookiejar.CookieJar()
    opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(cj))
    req = urllib.request.Request(
        BASE + "/api/v1/auth/login",
        data=json.dumps({"username": "admin", "password": "admin"}).encode(),
        headers={"Content-Type": "application/json"},
    )
    print("login", opener.open(req, timeout=10).read().decode()[:240])
    token = next((c.value for c in cj if c.name == "ipc_token"), None)
    if not token:
        print("FAIL: no cookie token")
        return 1

    got = {"text": None, "bins": 0, "bytes": 0}

    def on_msg(_ws, msg):
        if isinstance(msg, str):
            got["text"] = msg
            print("text", msg)
        else:
            got["bins"] += 1
            got["bytes"] += len(msg)
            if got["bins"] <= 3:
                print("bin", got["bins"], len(msg), msg[:8].hex())
            if got["bins"] >= 20:
                _ws.close()

    ws = websocket.WebSocketApp(
        WS_URL,
        cookie=f"ipc_token={token}",
        on_message=on_msg,
        on_error=lambda _w, e: print("err", e),
        on_open=lambda _w: print("ws open"),
    )
    threading.Thread(target=ws.run_forever, daemon=True).start()
    for _ in range(60):
        time.sleep(0.5)
        if got["bins"] >= 20:
            break
    print("RESULT", got)
    ws.close()
    ok = got["text"] and "avc1" in got["text"] and got["bins"] >= 5 and got["bytes"] > 1000
    print("PASS" if ok else "FAIL")
    return 0 if ok else 2


if __name__ == "__main__":
    raise SystemExit(main())
