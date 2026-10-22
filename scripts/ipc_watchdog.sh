#!/bin/sh
# Respawn ipc_app after exit/crash (T5.2 watchdog).
# Also keeps OEM rkipc dead — RkLunch post_chk may start it after S99.
#
# Log ownership: ipc_app rotates /userdata/log/ipc_app.log and ipc_stderr.log
# itself (log.max_kb / log.keep). The watchdog must not append to those files,
# or /userdata (2.2MB UBIFS) fills up and web/config writes start failing.
DAEMON=/userdata/ipc_app
LOG=/userdata/log/watchdog.log
LOCK=/var/run/ipc_app.lock
LOG_MAX_BYTES=32768
export LD_LIBRARY_PATH=/oem/usr/lib:/oem/lib:${LD_LIBRARY_PATH:-}

mkdir -p /userdata/log /var/run

wlog() {
  echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >>"$LOG"
  sz=$(wc -c <"$LOG" 2>/dev/null || echo 0)
  if [ "${sz:-0}" -gt "$LOG_MAX_BYTES" ]; then
    mv -f "$LOG" "$LOG.1" 2>/dev/null || true
  fi
}

# Only one watchdog/instance may run.
exec 9>"$LOCK"
if ! flock -n 9; then
  wlog "another ipc_app/watchdog holds $LOCK"
  exit 0
fi

stop_rkipc() {
  killed=0
  if pidof rkipc >/dev/null 2>&1; then
    wlog "stopping conflicting rkipc"
    killall rkipc 2>/dev/null || true
    sleep 1
    killall -9 rkipc 2>/dev/null || true
    killed=1
  fi
  if pidof rk_mpi_ao_test >/dev/null 2>&1; then
    killall -9 rk_mpi_ao_test 2>/dev/null || true
    killed=1
  fi
  if [ "$killed" = "1" ]; then
    rm -f /tmp/aiq0.lock 2>/dev/null || true
    if pidof ipc_app >/dev/null 2>&1; then
      wlog "restarting ipc_app after rkipc conflict"
      killall -9 ipc_app 2>/dev/null || true
      sleep 1
    fi
  fi
}

while true; do
  stop_rkipc
  if pidof ipc_app >/dev/null 2>&1; then
    sleep 5
    continue
  fi
  "$DAEMON" --web >/dev/null 2>&1
  wlog "ipc_app exited rc=$?, respawn in 5s"
  sleep 5
done
