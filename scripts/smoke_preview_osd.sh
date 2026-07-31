#!/bin/sh
set -e
export LD_LIBRARY_PATH=/oem/usr/lib
export IPC_PASS=123456
BASE=http://127.0.0.1:8080
. /userdata/lib_login.sh
ipc_login "$BASE" || exit 1

echo "=== osd get/set ==="
printf '%s' '{"enabled":true,"show_time":true,"show_name":true,"name":"客厅相机","pos_x":100,"pos_y":60,"apply":true}' > /tmp/osd.json
wget -qO- --header="$IPC_AUTH" --header='Content-Type: application/json' --post-file=/tmp/osd.json "$BASE/api/v1/video/osd"
echo
wget -qO- --header="$IPC_AUTH" "$BASE/api/v1/video/osd"
echo

# Use websocat if present; else python
echo "=== main preview ws probe ==="
python3 - <<'PY'
import json, ssl, sys, time, urllib.request
try:
    import websocket
except Exception:
    websocket=None

token=open('/tmp/login_resp.json').read()
import re
m=re.search(r'"token":"([^"]+)"', token)
tok=m.group(1) if m else ''
url=f'ws://127.0.0.1:8080/api/v1/preview/ws?stream=main&token={tok}'
print('url', url[:80], '...')
if websocket is None:
    print('no websocket-client; skip ws')
    sys.exit(0)
ws=websocket.create_connection(url, timeout=10)
ws.settimeout(8)
got_meta=False
got_bin=0
t0=time.time()
while time.time()-t0 < 8:
    try:
        msg=ws.recv()
    except Exception as e:
        print('recv end', e)
        break
    if isinstance(msg, str):
        print('meta', msg[:200])
        got_meta=True
    else:
        got_bin += 1
        if got_bin <= 3:
            print('bin', len(msg), msg[:8])
print('bins', got_bin, 'meta', got_meta)
ws.close()
PY

sleep 1
echo "=== log ==="
grep -E 'preview-main got|preview client|freetype|osd apply|stream=main' /userdata/log/ipc_app.log | tail -30
