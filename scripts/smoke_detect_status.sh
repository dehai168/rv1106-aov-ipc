#!/bin/sh
export LD_LIBRARY_PATH=/oem/usr/lib
echo "=== config ==="
python3 - <<'PY'
import json
c=json.load(open('/userdata/ipc_config.json'))
print('record.mode', c.get('record',{}).get('mode'))
d=c.get('detect',{})
for k in ['enabled','width','height','sensitivity','square_pct','link_record','link_snapshot']:
    print('detect.%s'%k, d.get(k))
PY
echo "=== recent detect/policy ==="
grep -E 'record policy|motion automation|detect |IVS|bind VI|deferred|detect_enabled' /userdata/log/ipc_app.log | tail -30
echo "=== alarm api ==="
. /userdata/lib_login.sh
export IPC_PASS=123456
ipc_login http://127.0.0.1:8080 || exit 1
wget -qO- --header="$IPC_AUTH" http://127.0.0.1:8080/api/v1/alarm/motion
echo
