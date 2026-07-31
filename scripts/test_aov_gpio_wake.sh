#!/bin/sh
# GPIO wake probe: arm a userspace wake source if available, then mem.
# Logs to TF only. Requires power cycle to read if resume fails.
# Usage: sh /mnt/sdcard/test_aov_gpio_wake.sh [gpio_sysfs_path]
set -e
LOG=/mnt/sdcard/aov_resume_probe.log
GPIO_WAKE=${1:-}

echo "=== gpio_wake probe start $(date) up=$(awk '{print int($1)}' /proc/uptime) ===" >> "$LOG"
sync

# Prefer rtc wakealarm as secondary path documentation (usually fails on this platform)
if [ -w /sys/class/rtc/rtc0/wakealarm ]; then
  NOW=$(date +%s)
  echo 0 > /sys/class/rtc/rtc0/wakealarm 2>/dev/null || true
  echo $((NOW + 5)) > /sys/class/rtc/rtc0/wakealarm 2>/dev/null || echo "rtc_wakealarm_fail" >> "$LOG"
  echo "rtc_wakealarm_armed_for_$((NOW+5))" >> "$LOG"
fi

if [ -n "$GPIO_WAKE" ] && [ -f "$GPIO_WAKE" ]; then
  echo enabled > "$GPIO_WAKE" 2>>"$LOG" || echo "gpio_enable_fail" >> "$LOG"
fi

# Still program PMU timeout as fallback (5s)
io -4 0xff300048 $((5000 * 32)) || echo "io_fail" >> "$LOG"
sync

echo "before_mem_gpio_probe $(date)" >> "$LOG"
sync
echo mem > /sys/power/state
rc=$?
echo "after_mem_gpio_probe rc=$rc $(date)" >> "$LOG"
echo "RESUME_OK mode=gpio_probe" >> "$LOG"
sync
echo woke > /mnt/sdcard/aov_resume_ok.flag
sync
