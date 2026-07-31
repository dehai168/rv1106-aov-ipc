#!/bin/bash
# Host-side AOV wake loop: push on-board helper, run cycles, detect adb drop/recover.
set -euo pipefail

COUNT="${1:-10}"
MS="${2:-1000}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOG_DIR="$ROOT/build/aov-wake-test"
mkdir -p "$LOG_DIR"
HOST_LOG="$LOG_DIR/run_$(date +%Y%m%d_%H%M%S).log"

log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$HOST_LOG"; }

adb wait-for-device
log "push test script"
adb push "$ROOT/scripts/test_aov_wake.sh" /mnt/sdcard/test_aov_wake.sh
adb shell 'chmod +x /mnt/sdcard/test_aov_wake.sh'

ok=0
fail=0
i=1

while [ "$i" -le "$COUNT" ]; do
  log "=== cycle $i/$COUNT ==="
  UPT=$(adb shell 'awk "{print int(\$1)}" /proc/uptime' | tr -d '\r')
  log "before uptime=$UPT"

  # Fire suspend in background on device; adb will drop if suspend succeeds.
  adb shell "nohup sh /mnt/sdcard/test_aov_wake.sh 1 $MS > /mnt/sdcard/aov_wake_once.log 2>&1 & echo started" | tr -d '\r' | tee -a "$HOST_LOG"

  sleep 2
  gone=0
  for _ in $(seq 1 15); do
    if ! adb devices | grep -q 'device$'; then
      gone=1
      log "adb disconnected (suspend likely)"
      break
    fi
    sleep 1
  done
  if [ "$gone" -eq 0 ]; then
    log "WARN: adb still connected after 15s — suspend may not have happened"
    adb shell 'tail -5 /mnt/sdcard/aov_wake_once.log' | tee -a "$HOST_LOG" || true
    fail=$((fail + 1))
    i=$((i + 1))
    sleep 2
    continue
  fi

  adb wait-for-device
  sleep 3
  UPT2=$(adb shell 'awk "{print int(\$1)}" /proc/uptime' | tr -d '\r')
  MARK=$(adb shell 'cat /mnt/sdcard/aov_wake_mark.txt 2>/dev/null' | tr -d '\r')
  IPC=$(adb shell 'ps | grep ipc_app | grep -v grep | wc -l' | tr -d '\r')
  VIDEO=$(adb shell 'test -c /dev/video0 && echo 1 || echo 0' | tr -d '\r')
  STATE=$(adb shell 'cat /sys/power/state' | tr -d '\r')

  delta=$((UPT2 - UPT))
  log "after uptime=$UPT2 delta=$delta mark=$MARK ipc=$IPC video=$VIDEO state=$STATE"

  if [ "$UPT2" -gt "$UPT" ] && [ "$delta" -lt 600 ] && [ "$VIDEO" = "1" ]; then
    ok=$((ok + 1))
    log "cycle $i PASS"
  else
    fail=$((fail + 1))
    log "cycle $i FAIL"
  fi

  i=$((i + 1))
  sleep 2
done

log "=== summary ok=$ok fail=$fail total=$COUNT ==="
adb shell 'tail -20 /mnt/sdcard/aov_wake_test.log 2>/dev/null' | tee -a "$HOST_LOG"

if [ "$fail" -eq 0 ]; then
  exit 0
fi
exit 1
