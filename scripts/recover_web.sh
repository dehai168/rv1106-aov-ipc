#!/bin/sh
# Stop old watchdog/ipc and start fixed watchdog.
killall ipc_app 2>/dev/null || true
sleep 1
for p in $(pidof sh 2>/dev/null); do
  if tr '\0' ' ' <"/proc/$p/cmdline" 2>/dev/null | grep -q ipc_watchdog; then
    kill "$p" 2>/dev/null || true
  fi
done
sleep 1
chmod +x /userdata/ipc_watchdog.sh /userdata/S99ipc_app
cp /userdata/S99ipc_app /etc/init.d/S99ipc_app
chmod +x /etc/init.d/S99ipc_app
export LD_LIBRARY_PATH=/oem/usr/lib:/oem/lib
nohup /userdata/ipc_watchdog.sh >/dev/null 2>&1 &
sleep 2
ps | grep -E 'ipc_app|watchdog'
wget -qO- -T 3 http://127.0.0.1:8080/ | head -c 80
echo
