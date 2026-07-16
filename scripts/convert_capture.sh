#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin
ROOT=/mnt/d/CodeProject/rv1106-aov-ipc
python3 "$ROOT/scripts/analyze_yuv2.py"
ffmpeg -y -f rawvideo -pix_fmt nv12 -s 1920x1080 \
  -i "$ROOT/build/capture/capture_0.yuv" \
  -vf "select=eq(n\,4),eq=brightness=0.5:contrast=5" \
  -update 1 -frames:v 1 \
  "$ROOT/build/capture/capture_boost.jpg"
ls -l "$ROOT/build/capture/capture_boost.jpg" "$ROOT/build/capture/image.bmp"
