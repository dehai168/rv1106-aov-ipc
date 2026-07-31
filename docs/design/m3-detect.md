# M3 移动侦测设计

## 链路
```
VI0 → VENC0 H265 → record（预录环 + 实时）
VI1 → VENC1 H264 → Web 子预览 + JPEG combo 抓图
VI2（≤1280×720，与主预览同路）→ IVS0 MD → detect_service
         └─ VENC3 H264 主预览（按需）
```

## 与录像策略
- **motion**：预览门控；命中可 `record_on_motion`（含预录）
- **continuous / schedule**：媒体常驻时仍跑 IVS，产出告警/抓图；`link.record` 不会把连续录像切成事件模式

## 状态机（motion 录像）
- IDLE：主码流包写入 GOP 环形缓冲（约 4s）
- 连续命中 N 次 → RECORDING：先刷预录再写实时；刷新静默截止
- 静默 `quiet_sec`（默认 30s）无运动 → IDLE，关段

## 告警
- `/mnt/sdcard/alarms/alarms.log` 追加 JSON 行（时间、面积比、矩形、snapshot 文件名）
- 抓图：硬件 JPEG → `/mnt/sdcard/snapshots/`

## CLI
- `ipc_app --detect [seconds]`：跑流+侦测+运动触发录像
