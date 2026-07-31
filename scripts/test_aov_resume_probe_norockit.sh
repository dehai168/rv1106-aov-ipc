#!/bin/sh
# Mem/freeze resume probe with rockit/media stack unloaded (no hpmcu_wrap @ 0xff6fe000).
# Result on TF: /mnt/sdcard/aov_resume_probe.log
set -e
LOG=/mnt/sdcard/aov_resume_probe.log
MODE=${1:-mem}
MS=${2:-3000}
VAL=$((MS * 32))

echo "=== norockit probe mode=$MODE ms=$MS start $(date) up=$(awk '{print int($1)}' /proc/uptime) ===" >> "$LOG"
sync

# Stop userspace camera stack (official Luckfox path)
/etc/init.d/S99ipc_app stop 2>/dev/null || killall ipc_app 2>/dev/null || true
sh /oem/usr/bin/RkLunch-stop.sh 2>/dev/null || killall rkipc 2>/dev/null || true
sleep 2

# Unload rockit and dependents (reverse of insmod_ko.sh)
for m in rockit mpp_vcodec rga3 video_rkisp video_rkcif rk_dvbm mis5001; do
  if lsmod | grep -q "^${m} "; then
    rmmod "$m" 2>>"$LOG" || echo "rmmod_fail $m $(date)" >> "$LOG"
    sync
  fi
done

echo "norockit_modules $(lsmod | grep -cE 'rockit|mpp_vcodec|rga3|video_rkisp|video_rkcif|mis5001' || echo 0) $(date)" >> "$LOG"
sync

if [ "$MODE" = "mem" ]; then
  io -4 0xff300048 "$VAL" || echo "io_fail" >> "$LOG"
  sync
fi

echo "before_${MODE}_norockit up=$(awk '{print int($1)}' /proc/uptime) $(date)" >> "$LOG"
sync

echo "$MODE" > /sys/power/state
rc=$?

echo "after_${MODE}_norockit rc=$rc up=$(awk '{print int($1)}' /proc/uptime) $(date)" >> "$LOG"
echo "RESUME_OK mode=${MODE}_norockit" >> "$LOG"
sync
echo "woke_norockit" > /mnt/sdcard/aov_resume_ok.flag
sync
