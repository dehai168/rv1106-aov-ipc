#!/bin/sh
# Quick T5.2 watchdog smoke: kill -9 ipc_app and wait for respawn.
set -e
LOG=/userdata/log/ipc_app.log

# Ensure watchdog is running
if ! pgrep -f ipc_watchdog.sh >/dev/null 2>&1; then
  echo "starting watchdog..."
  killall ipc_app 2>/dev/null || true
  sleep 1
  nohup /userdata/ipc_watchdog.sh >>"$LOG" 2>&1 &
  sleep 3
fi

pid=$(pidof ipc_app 2>/dev/null | awk '{print $1}')
if [ -z "$pid" ]; then
  echo "FAIL: ipc_app not running after watchdog start"
  exit 1
fi
echo "ipc_app pid=$pid — killing"
kill -9 "$pid"
sleep 12
npid=$(pidof ipc_app 2>/dev/null | awk '{print $1}')
if [ -z "$npid" ]; then
  echo "FAIL: ipc_app not respawned within 12s"
  exit 1
fi
echo "OK: respawned pid=$npid"
wget -qO- http://127.0.0.1:8080/ >/dev/null
echo "OK: http up"
