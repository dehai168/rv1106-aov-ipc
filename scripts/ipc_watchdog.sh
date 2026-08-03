#!/bin/sh
# Respawn ipc_app after exit/crash (T5.2 watchdog).
# Also keeps OEM rkipc dead — RkLunch post_chk may start it after S99.
DAEMON=/userdata/ipc_app
LOG=/userdata/log/ipc_app.log
LOCK=/var/run/ipc_app.lock
export LD_LIBRARY_PATH=/oem/usr/lib:/oem/lib:${LD_LIBRARY_PATH:-}

mkdir -p /userdata/log /var/run

# Only one watchdog/instance may run.
exec 9>"$LOCK"
if ! flock -n 9; then
  echo "$(date '+%Y-%m-%d %H:%M:%S') another ipc_app/watchdog holds $LOCK" >>"$LOG"
  exit 0
fi

stop_rkipc() {
  killed=0
  if pidof rkipc >/dev/null 2>&1; then
    echo "$(date '+%Y-%m-%d %H:%M:%S') stopping conflicting rkipc" >>"$LOG"
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
      echo "$(date '+%Y-%m-%d %H:%M:%S') restarting ipc_app after rkipc conflict" >>"$LOG"
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
  "$DAEMON" --web >>"$LOG" 2>>/userdata/log/ipc_stderr.log
  echo "$(date '+%Y-%m-%d %H:%M:%S') ipc_app exited rc=$?, respawn in 5s" >>"$LOG"
  sleep 5
done
