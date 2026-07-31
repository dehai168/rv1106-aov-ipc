#!/bin/sh
# In-process AOV lapse loop (one enter/leave, FE-PHY off between cycles).
# Usage: sh /mnt/sdcard/run_aov_soak.sh [N] [--media]
set -e
N=${1:-5}
MEDIA_ARG=""
if [ "$2" = "--media" ] || [ "$1" = "--media" ]; then
  MEDIA_ARG="--media"
fi
if [ "$1" = "--media" ]; then
  N=${2:-5}
fi

LOG=/tmp/aov_soak.log
TF_LOG=/mnt/sdcard/aov_soak.log
RESULT=/tmp/aov_userspace_soak_result.txt
TF_RESULT=/mnt/sdcard/aov_userspace_soak_result.txt
APP=/userdata/ipc_app

: > "$LOG"
echo "running n=$N ok=0 fail=0" > "$RESULT"
echo "=== aov loop N=$N media=$MEDIA_ARG start $(date) ===" >> "$LOG"
if "$APP" --aov loop "$N" $MEDIA_ARG >>"$LOG" 2>&1; then
  echo "PASS $N cycles $(date)" >> "$LOG"
  echo "done ok=$N fail=0" > "$RESULT"
else
  echo "FAIL loop $(date)" >> "$LOG"
  echo "done ok=0 fail=1" > "$RESULT"
  cat "$LOG" >> "$TF_LOG" 2>/dev/null || true
  cp "$RESULT" "$TF_RESULT" 2>/dev/null || true
  sync
  exit 1
fi
cat "$LOG" >> "$TF_LOG"
cp "$RESULT" "$TF_RESULT"
sync
