#!/usr/bin/env python3
from pathlib import Path

w, h = 1920, 1080
fs = w * h * 3 // 2
data = Path("/mnt/d/CodeProject/rv1106-aov-ipc/build/capture/capture_0.yuv").read_bytes()
y = data[4 * fs : 4 * fs + w * h]
hist = [0] * 16
for v in y[::50]:
    hist[v >> 4] += 1
print("hist16", hist)
rows = [sum(y[r * w : (r + 1) * w]) / w for r in range(0, h, 120)]
print("row_means", [round(x, 1) for x in rows])
cols = []
for c in range(0, w, 240):
    cols.append(sum(y[c::w]) / h)
print("col_means", [round(x, 1) for x in cols])
