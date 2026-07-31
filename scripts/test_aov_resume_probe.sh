#!/bin/sh
# Prove whether mem/freeze resumes even if USB/adb dies.
# Result is ONLY on TF: /mnt/sdcard/aov_resume_probe.log
set -e
LOG=/mnt/sdcard/aov_resume_probe.log
MODE=${1:-mem}
MS=${2:-3000}
VAL=$((MS * 32))

echo "=== probe mode=$MODE ms=$MS start $(date) up=$(awk '{print int($1)}' /proc/uptime) ===" >> "$LOG"
sync

if [ "$MODE" = "mem" ]; then
  io -4 0xff300048 "$VAL" || echo "io_fail" >> "$LOG"
  sync
fi

echo "before_$MODE up=$(awk '{print int($1)}' /proc/uptime) $(date)" >> "$LOG"
sync

echo "$MODE" > /sys/power/state
rc=$?

echo "after_$MODE rc=$rc up=$(awk '{print int($1)}' /proc/uptime) $(date)" >> "$LOG"
echo "RESUME_OK mode=$MODE" >> "$LOG"
sync
# blink/act: touch a second marker
echo "woke" > /mnt/sdcard/aov_resume_ok.flag
sync
