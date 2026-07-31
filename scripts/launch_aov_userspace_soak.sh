#!/bin/sh
# Detach userspace AOV soak (needs allow_mem_suspend=1).
# Usage: sh /mnt/sdcard/launch_aov_userspace_soak.sh [N] [--media]
N=${1:-50}
MEDIA=""
if [ "$2" = "--media" ] || [ "$1" = "--media" ]; then
  MEDIA="--media"
fi
if [ "$1" = "--media" ]; then
  N=${2:-50}
fi
OUT=/tmp/aov_userspace_soak.out
cd /tmp || cd /
: > "$OUT"
: > /tmp/aov_soak.log
: > /tmp/aov_userspace_soak_result.txt
echo "running n=$N media=$MEDIA" > /tmp/aov_userspace_soak_result.txt
nohup sh /mnt/sdcard/run_aov_soak.sh "$N" $MEDIA >"$OUT" 2>&1 &
echo "launched userspace soak n=$N media=$MEDIA pid=$!"
