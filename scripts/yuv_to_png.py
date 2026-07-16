#!/usr/bin/env python3
"""Convert one NV12 frame to equalized PNG for visual check."""
from pathlib import Path

try:
    import numpy as np
    from PIL import Image, ImageOps
except ImportError:
    import subprocess
    subprocess.check_call(["python3", "-m", "pip", "install", "--user", "numpy", "pillow"])
    import numpy as np
    from PIL import Image, ImageOps

w, h = 1920, 1080
fs = w * h * 3 // 2
data = Path("/mnt/d/CodeProject/rv1106-aov-ipc/build/capture/capture_0.yuv").read_bytes()
frame_idx = 4
y = np.frombuffer(data[frame_idx * fs : frame_idx * fs + w * h], dtype=np.uint8).reshape(h, w)
img = Image.fromarray(y, mode="L")
img = ImageOps.autocontrast(img)
out = Path("/mnt/d/CodeProject/rv1106-aov-ipc/build/capture/capture_eq.png")
img.save(out)
print("saved", out, "y_min", int(y.min()), "y_max", int(y.max()), "y_mean", float(y.mean()))
