#!/usr/bin/env python3
from pathlib import Path

path = Path("/mnt/d/CodeProject/rv1106-aov-ipc/build/capture/capture_0.yuv")
w, h = 1920, 1080
frame_size = w * h * 3 // 2
data = path.read_bytes()
n = len(data) // frame_size
print(f"file={path} bytes={len(data)} frames={n} frame_size={frame_size}")
for i in range(n):
    y = data[i * frame_size : i * frame_size + w * h]
    y_min = min(y)
    y_max = max(y)
    y_mean = sum(y) / len(y)
    print(f"frame{i}: y_min={y_min} y_max={y_max} y_mean={y_mean:.1f}")
