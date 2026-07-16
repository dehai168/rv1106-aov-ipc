#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin
ROOT=/mnt/d/CodeProject/rv1106-aov-ipc
ffmpeg -y -f rawvideo -pix_fmt nv12 -s 1920x1080 \
  -i "$ROOT/build/capture/capture_0.yuv" \
  -vf "select=eq(n\,4)" \
  -update 1 -frames:v 1 \
  "$ROOT/build/capture/capture_ok.jpg"
ls -l "$ROOT/build/capture/capture_ok.jpg" "$ROOT/build/capture/capture_eq.png"
