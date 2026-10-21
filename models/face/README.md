# models/face — face capture assets (M8)

| File | Description |
|---|---|
| `yunet_320.rknn` | YuNet 320×320 INT8 for RV1106 (from `yunet_n_320_320.onnx`) |

Deploy: `adb push models/face/yunet_320.rknn /mnt/sdcard/models/yunet_320.rknn`

Convert (WSL, needs `rknn-toolkit2`):

```bash
cd /home/user/work/yunet_conv
. .venv/bin/activate
python onnx_to_rknn.py yunet_n_320_320.onnx \
  -o /mnt/d/CodeProject/rv1106-aov-ipc/models/face/yunet_320.rknn \
  --platform rv1106 --dataset calib
```

Smoke: `/userdata/face_rknn_smoke /mnt/sdcard/models/yunet_320.rknn 50`
