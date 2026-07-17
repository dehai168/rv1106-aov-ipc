#!/bin/sh
# Redeploy binary+www and restart watchdog cleanly.
set -e
export LD_LIBRARY_PATH=/oem/usr/lib:/oem/lib

killall ipc_app 2>/dev/null || true
sleep 1
for p in $(pidof sh 2>/dev/null); do
  if tr '\0' ' ' <"/proc/$p/cmdline" 2>/dev/null | grep -q ipc_watchdog; then
    kill "$p" 2>/dev/null || true
  fi
done
sleep 1
rm -f /var/run/ipc_app.lock
chmod +x /userdata/ipc_app /userdata/ipc_watchdog.sh
nohup /userdata/ipc_watchdog.sh >/dev/null 2>&1 &
sleep 2
ps | grep -E 'ipc_app|watchdog' | grep -v grep
wget -qO- -T 3 http://127.0.0.1:8080/ | head -c 60
echo
