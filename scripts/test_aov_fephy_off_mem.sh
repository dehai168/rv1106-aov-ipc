#!/bin/sh
# Prepare FE-PHY off, then mem probe (Rockchip AOV tip).
# Result on TF: /mnt/sdcard/aov_resume_probe.log
set -e
LOG=/mnt/sdcard/aov_resume_probe.log
MS=${1:-3000}
GMAC=ffa80000.ethernet
DRV=/sys/bus/platform/drivers/rk_gmac-dwmac

echo "=== fephy_off mem probe ms=$MS start $(date) ===" >> "$LOG"
sync

# 1) disable gmac wakeup
if [ -f /sys/devices/platform/$GMAC/power/wakeup ]; then
  echo disabled > /sys/devices/platform/$GMAC/power/wakeup || true
  echo "gmac_wakeup=$(cat /sys/devices/platform/$GMAC/power/wakeup)" >> "$LOG"
fi

# 2) link down
ip link set eth0 down 2>>"$LOG" || echo "eth0_down_fail" >> "$LOG"

# 3) unbind → FEPHY SHUTDOWN via rk_gmac_powerdown
if [ -e "$DRV/$GMAC" ]; then
  echo "$GMAC" > "$DRV/unbind" || echo "unbind_fail" >> "$LOG"
  echo "gmac_unbound" >> "$LOG"
else
  echo "gmac_already_unbound" >> "$LOG"
fi
sync

io -4 0xff300048 $((MS * 32)) || echo "io_fail" >> "$LOG"
sync
echo "before_mem_fephy_off $(date)" >> "$LOG"
sync
echo mem > /sys/power/state
rc=$?
echo "after_mem_fephy_off rc=$rc $(date)" >> "$LOG"
echo "RESUME_OK mode=fephy_off" >> "$LOG"
# rebind for next boot if we somehow woke
echo "$GMAC" > "$DRV/bind" 2>/dev/null || true
sync
echo woke > /mnt/sdcard/aov_resume_ok.flag
sync
