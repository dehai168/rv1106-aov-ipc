# M3 移动侦测设计

## 链路
```
VI0 → VENC0 H265 → record（预录环 + 实时）
VI1 → VENC1 H264 →（预览预留）
VI2 640x360 → IVS0 MD → detect_service → EVT_MOTION_DETECT → record_on_motion
```

## 状态机
- IDLE：主码流包写入 GOP 环形缓冲（约 4s）
- 连续命中 N 次 → RECORDING：先刷预录再写实时；刷新静默截止
- 静默 `quiet_sec`（默认 30s）无运动 → IDLE，关段

## 告警
- `/mnt/sdcard/alarms/alarms.log` 追加 JSON 行（时间、面积比、矩形）
- 抓图（JPEG）延后到有可用编码路径时再补

## CLI
- `ipc_app --detect [seconds]`：跑流+侦测+运动触发录像
