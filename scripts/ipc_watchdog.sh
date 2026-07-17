#!/bin/sh
# Respawn ipc_app after exit/crash (T5.2 watchdog).
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

while true; do
  # Drop stale listeners before (re)start
  if pidof ipc_app >/dev/null 2>&1; then
    echo "$(date '+%Y-%m-%d %H:%M:%S') ipc_app already running, wait" >>"$LOG"
    sleep 10
    continue
  fi
  # App log goes to file via log_init_ex; keep rockit/stderr separate to avoid dup lines.
  "$DAEMON" --web >>"$LOG" 2>>/userdata/log/ipc_stderr.log
  echo "$(date '+%Y-%m-%d %H:%M:%S') ipc_app exited rc=$?, respawn in 10s" >>"$LOG"
  sleep 10
done
