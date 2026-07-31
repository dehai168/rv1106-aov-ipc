#!/bin/sh
# T6.3.4 / T6.2.4: HPMCU timer + suspend(mem) wake loop (on-board helper).
# Host driver: scripts/run_aov_wake_test.sh
set -e

COUNT="${1:-10}"
MS="${2:-1000}"
VAL=$((MS * 32))
LOG="/mnt/sdcard/aov_wake_test.log"
MARK="/mnt/sdcard/aov_wake_mark.txt"

echo "=== aov_wake_test start count=$COUNT ms=$MS val=$VAL $(date) ===" | tee "$LOG"

i=1
while [ "$i" -le "$COUNT" ]; do
  UPT=$(awk '{print int($1)}' /proc/uptime)
  echo "cycle=$i before_uptime=$UPT" | tee -a "$LOG"
  echo "cycle=$i uptime=$UPT $(date)" > "$MARK"
  sync

  io -4 0xff300048 "$VAL" || { echo "cycle=$i io_fail" | tee -a "$LOG"; exit 1; }
  sync
  echo mem > /sys/power/state || { echo "cycle=$i mem_fail" | tee -a "$LOG"; exit 1; }

  UPT2=$(awk '{print int($1)}' /proc/uptime)
  echo "cycle=$i after_uptime=$UPT2" | tee -a "$LOG"
  i=$((i + 1))
done

echo "=== aov_wake_test done ok count=$COUNT ===" | tee -a "$LOG"
