# T1.2 双码流 VENC

## 链路
```
VI ch0 (mainpath) → VENC0 H.265  → /mnt/sdcard/main.h265
VI ch1 (selfpath) → VENC1 H.264  → /mnt/sdcard/sub.h264
```

## 默认参数（config video.encode）
- main: 1920x1080@15 H.265 CBR 2048kbps GOP30（2560x1440 可配，先以 1080p 稳妥验收）
- sub: 704x576@15 H.264 CBR 1024kbps GOP30

## CLI
`ipc_app --encode [seconds]`（默认 10，验收用 30）
