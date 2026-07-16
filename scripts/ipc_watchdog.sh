#!/bin/sh
# Respawn ipc_app after exit/crash (T5.2 watchdog).
DAEMON=/userdata/ipc_app
LOG=/userdata/log/ipc_app.log

mkdir -p /userdata/log
while true; do
  "$DAEMON" --web >>"$LOG" 2>&1
  echo "$(date '+%Y-%m-%d %H:%M:%S') ipc_app exited, respawn in 10s" >>"$LOG"
  sleep 10
done
