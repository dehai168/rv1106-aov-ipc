#!/usr/bin/env python3
"""Reproduce preview WS crash via adb forward :18080."""
import http.cookiejar
import json
import subprocess
import threading
import time
import urllib.request

try:
    import websocket
except ImportError:
    import sys

    subprocess.check_call([sys.executable, "-m", "pip", "install", "websocket-client", "-q"])
    import websocket

BASE = "http://127.0.0.1:18080"
WS = "ws://127.0.0.1:18080/api/v1/preview/ws"


def login(opener):
    for pw in ("admin1", "admin"):
        req = urllib.request.Request(
            BASE + "/api/v1/auth/login",
            data=json.dumps({"username": "admin", "password": pw}).encode(),
            headers={"Content-Type": "application/json"},
        )
        try:
            body = opener.open(req, timeout=10).read().decode()
            print("login", pw, body[:220])
            if '"token"' in body:
                return True
        except Exception as e:
            print("login fail", pw, e)
    return False


def main() -> int:
    cj = http.cookiejar.CookieJar()
    opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(cj))
    if not login(opener):
        return 1
    token = next((c.value for c in cj if c.name == "ipc_token"), None)
    if not token:
        print("FAIL: no token cookie")
        return 1

    got = {"text": None, "bins": 0, "bytes": 0, "err": None}

    def on_msg(ws, msg):
        if isinstance(msg, str):
            got["text"] = msg
            print("text", msg)
        else:
            got["bins"] += 1
            got["bytes"] += len(msg)
            if got["bins"] <= 3:
                print("bin", got["bins"], len(msg), msg[:8].hex())
            if got["bins"] >= 20:
                ws.close()

    ws = websocket.WebSocketApp(
        WS,
        cookie=f"ipc_token={token}",
        on_message=on_msg,
        on_error=lambda _w, e: got.__setitem__("err", str(e)) or print("err", e),
        on_open=lambda _w: print("ws open"),
    )
    threading.Thread(target=ws.run_forever, daemon=True).start()
    for _ in range(90):
        time.sleep(0.5)
        if got["bins"] >= 20:
            break

    print("RESULT", got)
    subprocess.run(
        ["adb", "shell", "ps | grep '[i]pc_app'; free; echo ---; tail -40 /userdata/log/ipc_app.log"],
        check=False,
    )
    ok = bool(got["text"] and "avc1" in got["text"] and got["bins"] >= 5)
    print("PASS" if ok else "FAIL")
    return 0 if ok else 2


if __name__ == "__main__":
    raise SystemExit(main())
