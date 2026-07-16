#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin
ROOT=/mnt/d/CodeProject/rv1106-aov-ipc/build/capture
mkdir -p "$ROOT"
echo "=== main.h265 ==="
ffprobe -v error -show_entries format=duration,size,bit_rate -show_entries stream=codec_name,width,height,r_frame_rate -of default=noprint_wrappers=1 "$ROOT/main.h265"
ffmpeg -y -i "$ROOT/main.h265" -frames:v 1 "$ROOT/main_frame.jpg"
echo "=== sub.h264 ==="
ffprobe -v error -show_entries format=duration,size,bit_rate -show_entries stream=codec_name,width,height,r_frame_rate -of default=noprint_wrappers=1 "$ROOT/sub.h264"
ffmpeg -y -i "$ROOT/sub.h264" -frames:v 1 "$ROOT/sub_frame.jpg"
ls -l "$ROOT/main.h265" "$ROOT/sub.h264" "$ROOT/main_frame.jpg" "$ROOT/sub_frame.jpg"
