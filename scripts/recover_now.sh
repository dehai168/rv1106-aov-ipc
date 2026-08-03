#!/bin/sh
# Emergency recover: kill hung S99/RkLunch-stop, kill rkipc, start ipc_app.
killall -9 rkipc rk_mpi_ao_test ipc_app 2>/dev/null || true
for p in /proc/[0-9]*; do
  cmd=$(tr '\0' ' ' <"$p/cmdline" 2>/dev/null) || continue
  case "$cmd" in
    *ipc_watchdog*|*S99ipc_app*|*RkLunch-stop*)
      kill -9 "${p#/proc/}" 2>/dev/null || true
      ;;
  esac
done
rm -f /tmp/aiq0.lock /var/run/ipc_app.lock
chmod 755 /userdata/ipc_watchdog.sh /userdata/S99ipc_app /etc/init.d/S99ipc_app 2>/dev/null || true
cp /userdata/S99ipc_app /etc/init.d/S99ipc_app 2>/dev/null || true
sleep 1
nohup /userdata/ipc_watchdog.sh >/dev/null 2>&1 &
sleep 3
echo ===procs===
ps | grep -E 'rkipc|ipc_app|ipc_watch|S99' | grep -v grep
echo ===http===
wget -qO- -T 3 http://127.0.0.1:8080/api/v1/system/device_name
echo
