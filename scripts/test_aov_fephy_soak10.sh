#!/bin/sh
# On-device N-times FE-PHY mem soak (run detached from adb).
N=${1:-10}
MS=${2:-3000}
LOG=/mnt/sdcard/aov_resume_probe.log
RESULT=/mnt/sdcard/aov_soak10_result.txt
ok=0
fail=0

rm -f "$RESULT"
echo "running n=$N ok=0 fail=0" > "$RESULT"
echo "=== soak10 start n=$N ms=$MS $(date) ===" >> "$LOG"
sync

i=1
while [ "$i" -le "$N" ]; do
  echo "=== soak10 #$i $(date) ===" >> "$LOG"
  sync
  if sh /mnt/sdcard/test_aov_fephy_off_mem.sh "$MS"; then
    ok=$((ok + 1))
    echo "soak10 #$i OK" >> "$LOG"
    echo "running n=$N ok=$ok fail=$fail" > "$RESULT"
    sync
  else
    fail=$((fail + 1))
    echo "soak10 #$i FAIL" >> "$LOG"
    echo "done ok=$ok fail=$fail" > "$RESULT"
    sync
    break
  fi
  sleep 1
  i=$((i + 1))
done

echo "=== soak10 done ok=$ok fail=$fail success=$(cat /sys/power/suspend_stats/success) $(date) ===" >> "$LOG"
echo "done ok=$ok fail=$fail" > "$RESULT"
sync
