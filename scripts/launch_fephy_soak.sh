#!/bin/sh
# Detach soak from adb (BusyBox setsid has no -f; use nohup).
N=${1:-12}
MS=${2:-3000}
OUT=/mnt/sdcard/aov_soak10_stdout.txt
: > "$OUT"
nohup sh /mnt/sdcard/test_aov_fephy_soak10.sh "$N" "$MS" >>"$OUT" 2>&1 &
echo "launched soak n=$N ms=$MS pid=$!"
