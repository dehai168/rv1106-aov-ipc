#!/bin/sh
# Quick preview health: start media path via login if possible, check recent packs / face.
echo "=== face.enabled ==="
sed -n '/"face"/,/^	}/p' /userdata/ipc_config.json 2>/dev/null | head -40
echo "=== last media/preview/face ==="
grep -E 'stream (start|stop)|preview client|face (init|worker|infer)|GetChnFrame|grab' /userdata/log/ipc_app.log | tail -30
echo "=== ivs timeout count (last 2min window approx) ==="
grep -c 'get result timeout' /userdata/log/ipc_stderr.log 2>/dev/null || true
echo "=== free/mem ==="
free
cat /proc/$(pidof ipc_app)/status 2>/dev/null | grep -E 'VmRSS|Threads'
