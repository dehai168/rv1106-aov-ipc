#!/bin/sh
# Detach FE-PHY mem probe so adb USB drop does not kill it.
MS=${1:-3000}
setsid sh /mnt/sdcard/test_aov_fephy_off_mem.sh "$MS" \
  >/mnt/sdcard/aov_launch.out 2>&1 &
echo "launched pid=$! ms=$MS"
