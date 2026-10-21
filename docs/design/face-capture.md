# 人脸抓拍方案（MVP）

> 状态：**已定稿**（2026-08-05）。仅做人脸**检测 + 抓拍落盘 + Web 浏览**，不做身份比对 / 人脸库。  
> 硬件：RV1106G3 NPU（INT8）+ 现有 VI/VENC/IVS 链路。

---

## 0. 已确认决策

| 项 | 决定 |
|---|---|
| 模型 | **YuNet 320 INT8 RKNN**（固定，无模型切换 / 调试开关） |
| Web 菜单 | 事件下独立「人脸抓拍」；页内 Tab「抓拍记录」 |
| 触发 | **仅运动门控**（量产唯一策略；无周期/始终模式） |
| 联动录像 | **默认开** |
| 预览画框 | **不做** |
| 全景存储 | **只引用 `snapshots/`**，不另存 `_scene.jpg` |

---

## 1. 目标与边界

### 1.1 要做

| 项 | 说明 |
|---|---|
| 人脸检测 | NPU 跑 YuNet 320，输出框 + 置信度 |
| 人脸抓拍 | 从高清帧裁剪人脸（含边距）存 JPEG 到 `faces/` |
| 全景关联 | 触发时 `media_snapshot_jpeg` → `snapshots/`，事件里记 `scene_file` |
| 去重节流 | IoU track + 冷却，同一人短时间不重复抓 |
| Web 配置 / 记录 | 开关、灵敏度、区域、布防、策略；抓拍记录预览下载 |
| 联动 | 默认：写告警 + 保存全景 + **触发录像** |

### 1.2 明确不做（本版）

- 人脸识别 / 1:N 比对 / 黑白名单
- 活体、属性（年龄性别口罩）
- 车牌（另里程碑）
- 云端 / 邮件 / FTP
- 预览 MSE 实时画框
- 「仅检测不抓拍」、多模型、周期/始终触发等调试入口

### 1.3 成功标准（板上）

- 室内 2–5m、正脸或小侧角：检出率可用，白天误报可控
- 推理 + 裁剪不拖垮预览/录像；RSS 可接受
- 同一目标默认 ≥3s 不重复抓拍；磁盘按配额回收

---

## 2. 系统架构

```
Sensor → ISP(AIQ) → VI
  ├─ VI0 → VENC0 主码流 → 录像（联动默认开）
  ├─ VI1 → VENC1 子码流 → Web 预览 / JPEG combo
  └─ VI2 ≤1280×720
        ├─ IVS0 MD（运动门控）
        └─ RGA resize → NPU(YuNet 320 RKNN)
                         │
                         ▼
                   face_service
                         │
          ┌──────────────┼──────────────┐
          ▼              ▼              ▼
     人脸 JPEG      全景 JPEG      alarms 事件
   faces/YYYYMMDD/  snapshots/    type=face
                    （引用，不复制）
```

新增：`src/face/face_service` + `rknn_face`（固定加载 YuNet 320）。  
复用：IVS 运动事件、`media_snapshot_jpeg`、`record_on_motion` 同类联动、存储回收。

推理输入：VI2 NV12 → 320×320；框归一化后映射到裁剪源。优先高清源裁人脸；源不可用时退 VI2。

---

## 3. 算法

### 3.1 模型（固定）

| 项 | 值 |
|---|---|
| 网络 | YuNet |
| 输入 | 320×320 |
| 格式 | INT8 `.rknn`（RV1106） |
| 部署路径 | **`/mnt/sdcard/models/yunet_320.rknn`**（userdata 仅约 2MB，模型放 TF） |

无 Web/配置项切换模型；缺失模型时 `enabled` 应用失败并打错误日志。

### 3.2 运行时参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `score_thr` | 0.55 | 由灵敏度档映射 |
| `min_face_px` | 40 | 推理坐标系最小脸宽 |
| `nms_iou` | 0.4 | 框合并 |
| `max_faces` | 8 | 单帧最多处理 |
| `confirm_frames` | 2 | 连续检出才抓 |
| `cooldown_ms` | 3000 | 同 track 冷却 |
| `hold_ms` | 2000 | 运动消失后仍允许推理的窗口 |
| `interval_ms` | 500 | 门控打开期间最小推理间隔 |
| `max_snap_per_frame` | 2 | 同帧最多落盘人脸数 |
| `margin` | 1.4 | 裁剪外扩 |
| `face_jpeg_side` | 320 | 人脸图最长边 |

### 3.3 灵敏度档位（Web）

| UI | score_thr | min_face_px |
|---|---|---|
| 高灵敏 | 0.45 | 32 |
| 标准（默认） | 0.55 | 40 |
| 低误报 | 0.65 | 56 |

不提供高级数值覆盖页（本版）。

### 3.4 触发（唯一）

**`motion_gate` only**：IVS 有运动，或运动结束后 `hold_ms` 内，才按 `interval_ms` 推理。  
静止：不跑 NPU。配置里 `trigger` 恒为 `motion_gate`，Web 不展示其它选项。

---

## 4. 抓拍策略

### 4.1 去重

IoU 匹配上一帧框形成 track；同 track 在 `cooldown_ms` 内不重复抓。

### 4.2 质量门（须同时满足）

1. `score ≥ score_thr`
2. 脸宽 ≥ `min_face_px`（映射到裁剪源）
3. 框可用（允许贴边裁剪）
4. 连续 `confirm_frames` 帧检出
5. 布防时间与区域内（若启用）

多脸：按分数排序，最多 `max_snap_per_frame` 张。

### 4.3 落盘流程（单次成功抓拍）

1. `media_snapshot_jpeg` → `snapshots/YYYYMMDD_HHMMSS.jpg`（全景，已有路径）
2. 裁剪人脸 → `faces/YYYYMMDD/YYYYMMDD_HHMMSS_mmm_n.jpg`
3. 写 `alarms.log`：`type=face`，`face_file` + `scene_file`（仅文件名，指向 snapshots）
4. 若 `link_record`：触发录像联动（默认开）

### 4.4 配额

| 项 | 默认 |
|---|---|
| 单日上限 | 2000 张 |
| 总配额 | TF 可用空间 `face.quota_percent`（默认 5%） |
| 回收 | 删最旧 `faces/`；软 format 清空 `faces/`（snapshots 随现有策略） |

### 4.5 区域与布防

独立 `face.regions`（最多 4）+ `face.schedule.grid`（7×24），UI 复用移动侦测组件。  
区域外 / 非布防：不落盘（可不推理或推理后丢弃，实现选省算力者）。

### 4.6 联动默认

| 联动 | 默认 |
|---|---|
| 保存人脸图 | on（固定） |
| 保存全景（写 snapshots + 引用） | on |
| 写告警事件 | on |
| 触发录像 | **on** |
| 预览画框 | 不做 |

---

## 5. Web

### 5.1 菜单

```
设置 → 事件
  ├─ 移动侦测     /settings/event/motion
  └─ 人脸抓拍     /settings/event/face    ← 新增
```

不单独占「存储 → 人脸相册」菜单；记录在人脸页 Tab3。

### 5.2 `FaceCaptureView` 三 Tab

**侦测**

- 启用人脸抓拍
- 灵敏度：高 / 标准 / 低误报
- 抓拍间隔：1 / 3 / 5 / 10 秒
- 单帧最多抓拍数：1–4
- 保存全景：开关（默认开）
- 联动录像：开关（默认开）
- 保存并应用  
- 文案固定说明：有移动时才检测（不提供触发方式下拉）

**区域与布防**

- `RegionDrawCanvas` + 底图刷新  
- `ScheduleGrid` 7×24  

**抓拍记录**

- 按日筛选、分页  
- 人脸缩略图；可打开关联全景（`scene_file` → snapshots API）  
- 预览 / 下载 / 删除  

### 5.3 预览页

不增加画框；不强制加状态灯（可选后续，非本版范围）。

---

## 6. 配置与 API

### 6.1 `ipc_config.json`

```json
{
  "face": {
    "enabled": false,
    "model": "yunet_320",
    "trigger": "motion_gate",
    "interval_ms": 500,
    "hold_ms": 2000,
    "cooldown_ms": 3000,
    "score_thr": 0.55,
    "min_face_px": 40,
    "confirm_frames": 2,
    "max_faces": 8,
    "max_snap_per_frame": 2,
    "margin": 1.4,
    "face_jpeg_side": 320,
    "save_scene": true,
    "link_record": true,
    "link_alarm": true,
    "quota_percent": 5,
    "daily_limit": 2000,
    "sensitivity": "normal",
    "regions": [],
    "schedule": { "enabled": false, "grid": "<168 chars of 0/1>" }
  }
}
```

`model` / `trigger` 只读约定值，POST 忽略篡改或校验失败。

### 6.2 REST

| 方法 | 路径 | 说明 |
|---|---|---|
| GET/POST | `/api/v1/alarm/face` | 配置；`apply` 热更新 |
| GET | `/api/v1/alarm/face/events?date=&limit=&offset=` | 抓拍列表 |
| GET | `/api/v1/alarm/face/image?file=` | 人脸图（`faces/` 白名单） |
| GET | `/api/v1/alarm/snapshot?file=` | 全景（复用现有） |
| DELETE | `/api/v1/alarm/face/events` | 删指定人脸文件/按日 |

事件示例：

```json
{
  "ts": "2026-08-05T11:28:03",
  "type": "face",
  "score": 0.87,
  "box": { "x": 0.41, "y": 0.22, "w": 0.08, "h": 0.12 },
  "face_file": "20260805/20260805_112803_120_0.jpg",
  "scene_file": "20260805_112803.jpg"
}
```

`box` 为 0–1 归一化；`scene_file` 指向 `snapshots/` 文件名。

---

## 7. 存储布局

```
/mnt/sdcard/
  faces/YYYYMMDD/*.jpg     # 仅人脸裁剪图
  snapshots/*.jpg          # 全景（人脸事件引用）
  alarms/alarms.log        # type=face 行
```

---

## 8. 实现分期

| 阶段 | 内容 | 预估 |
|---|---|---|
| **F0** | RKNN + YuNet 320 上板、测耗时/内存 | 2–4 天 |
| **F1** | `face_service`：motion_gate + 去重 + 人脸落盘 + 引用 snapshot + 联动录像 | 3–5 天 |
| **F2** | API + 配置持久化 | 1–2 天 |
| **F3** | Web 菜单 + 三 Tab | 2–3 天 |
| **F4** | 区域/布防/配额 + 真机调参 | 2–3 天 |

约 **2–3 周**。

---

## 9. 风险与对策

| 风险 | 对策 |
|---|---|
| 内存紧 | 小模型常驻；严格 motion 门控；限制并发 |
| 侧脸/逆光 | 灵敏度档；文档写最佳距离 |
| 连拍 | cooldown + IoU + confirm_frames |
| 模型文件缺失 | 应用失败提示；不静默空转 |
| 合规 | 首次开启本地存储说明文案 |

---

## 10. 下一步

按 F0→F4 写入 `docs/03-todolist.md`（或独立 face todolist）后开工。开工前先做 F0 板上 RKNN/YuNet 可行性冒烟。
