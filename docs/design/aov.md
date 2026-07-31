# M6 变帧率 AOV 设计说明

> 状态：纠偏定稿（2026-07-28）  
> 适用范围：Luckfox Pico Max（RV1106G3 / SPI NAND / Buildroot）  
> 前置：Phase 1 常电链路已验收（T5.3）  
> 关联：`docs/01-architecture.md` §4.5；`docs/07-todolist-aov.md`  
> 修订原则：改内核/固件前须用户同意（D1 已允许）。

---

## 1. 产品目标（变帧率 Always-On Video）

摄像机不放过场景，但按动态智能调节工作状态：

| 状态 | 行为 | 功耗形态 |
|---|---|---|
| **超低功耗（静态）** | 约 **1fps** 抓拍/lapse；主控 `mem`；无路由/WiFi/Web | 芯片级待机（目标几十 mW，需实测） |
| **全性能（有事件）** | **50ms 内**（MVP 先接受秒级）切到 **20~30fps** 高清录像 | 接近常电媒体；仍可关网 |

```
静态 1fps（ARM mem + HPMCU 看场）
        │  MCU FastAeMd / 事件
        ▼
全性能 20~30fps 录像（关网）
        │  安静超时 quiet_sec（可选 event_record_sec）
        ▼
回到 1fps 深睡
```

**人/车识别**：MCU 休眠期只做 **AE/亮度网格运动（FastAeMd）**，不是人车分类。人车/IVS 在 **ARM 唤醒后** 跑（复用 `detect_service`）。MVP：MCU 运动 → 立刻全帧率录像；人车过滤可作为唤醒后二次确认。

### 1.1 非目标

- 休眠中保持 Web 预览 / WiFi / 路由；
- 休眠中跑完整人车深度学习；
- 未打通 MIS5001 wakeup FW 前承诺官方 EVB 同级 50ms / 几十 mW；
- Linux `rtcwake` 作为主唤醒源。

---

## 2. 官方机制（SDK 事实）

变帧率 **不是**「Linux 常驻降帧」，而是：

1. Linux：`SetSuspendTime(1000)` → ISP **单帧** → umount TF → `echo mem`
2. **HPMCU wakeup** 固件在 ARM 睡时接管 sensor/ISP；约 1s PMU 超时或 **FastAeMd/GPIO** 拉起 ARM
3. 无事件：醒后编 **1 帧** lapse → 再睡（≈1fps）
4. 有事件：MCU 置 multi-frame → Linux `MultiFrame` → **20~30fps** 连续录像

关键路径（WSL SDK `/home/user/work/luckfox-pico`）：

| 组件 | 路径 |
|---|---|
| 用户态 sleep / 定时 | `project/app/component/rkadk/.../aov/rkadk_aov.c` |
| AOV_LAPSE 单/多帧 | `.../muxer/rkadk_muxer.c` |
| MCU FastAeMd | `sysdrv/source/mcu/.../battery-ipc/fast_ae.c` |
| Trust wakeup | `RKTRUST/RV1106TOS_WAKEUP.ini` + `rv1106_hpmcu_wakeup_*.bin@0x40000` |

唤醒源优先级：

1. **MCU FastAeMd（运动）** — 进全性能  
2. **PMU 1s 超时** — 1fps lapse 节拍  
3. GPIO（可选；D6=MVP 不做 PIR）

定时寄存器（参考实现）：`io -4 0xff300048 $((ms*32))`。

---

## 3. 板级现状与卡点（2026-07-28）

已上板：

- 内核：`CONFIG_SUSPEND` / `RV1106_PMU_WAKEUP_TIMEOUT` / `ROCKCHIP_GRF` / `VIDEO_CAM_SLEEP_WAKEUP`
- DTS：`aov-reserved` on `rkcif_mipi_lvds`
- Trust：`RV1106TOS_WAKEUP.ini`（idblock+uboot）

**硬阻塞**：TF 探针多次仅 `before_mem*`，无 `RESUME_OK` → **mem 无法 resume**，1fps 深睡循环无法成立。

相对官方 EVB 缺口：

1. mem resume 不通  
2. **MIS5001 无匹配 wakeup MCU 板级**（仅有 SC200AI/SC3338 等 `rv1106_evb2_wakeup-*`）  
3. Luckfox RKADK 默认 `-DENABLE_AOV=OFF`  
4. 本仓库走 MPI+rkmuxer，自研 `aov_service` 借机制、不换整楼 RKADK  

### 3.1 官方 AOV 开发实践（必做）

向瑞芯微申请查阅 **`Rockchip_RV1106_Developer_Guide_Linux_AOV_CN.pdf`**，并按指南核对 DTS / 内核编译选项。

进 `echo mem > /sys/power/state` **之前**，必须关断「偷电大户」——尤其是 **内部 FE-PHY**：

| 步骤 | 做法（本仓库已实现） |
|---|---|
| 禁 gmac wakeup | `echo disabled > .../ffa80000.ethernet/power/wakeup` |
| 网口 down | `ip link set eth0 down` |
| 彻底掉 FE-PHY | unbind `rk_gmac-dwmac` → `rk_gmac_powerdown()` → `RV1106 FEPHY SHUTDOWN` |

仅 `ip link set eth0 down` **不够**。用户态见 `network_prepare_aov_suspend()`。

探针细节：`docs/design/aov-probe-notes.md`、`docs/design/aov-resume-blockers.md`。

---

## 4. 运行模式与状态机

| 模式 | 说明 | 默认 |
|---|---|---|
| `always_on` | Phase 1 常电 | **是** |
| `aov` | 变帧率：1fps 深睡 ↔ 事件 20~30fps | 否（显式开启） |

```json
"aov": {
  "enabled": false,
  "suspend_ms": 1000,
  "event_fps": 25,
  "quiet_sec": 30,
  "event_record_sec": 0,
  "max_cycles_before_reinit": 40,
  "down_eth": true
}
```

- `event_record_sec=0`：沿用安静超时 `quiet_sec` 结束事件录像  
- `event_record_sec>0`：事件录像固定秒数后回 1fps（如 1800=30 分钟）

### 4.1 状态机

```
ALWAYS_ON ──enter──► AOV_IDLE
                        │
                        ▼
                  PREPARE_SLEEP
             (停预览/落盘/关eth/单帧/umount TF)
                        │
                        ▼
                    SLEEPING  ◄─────────────────┐
                        │                       │
              (PMU timer / MCU MD)              │
                        ▼                       │
                    POST_WAKE                   │
              (mount TF / 读唤醒原因)            │
                        │                       │
           ┌────────────┴────────────┐          │
           │                         │          │
      timer / 无事件              MCU 运动       │
           │                         │          │
           ▼                         ▼          │
     LAPSE_1FPS                 FULL_RECORD     │
   (编1帧/可选)              (MultiFrame 20~30) │
           │                         │          │
           └──────────► PREPARE_SLEEP ◄─────────┘
                    quiet / event_record 结束
```

### 4.2 与现有模块

- 常电：`IDLE ⇄ RECORDING` 不变  
- AOV：`FULL_RECORD` 尽量复用 `record_service`；结束后回 `PREPARE_SLEEP`  
- 人车二次确认：可选，在 `POST_WAKE`/`FULL_RECORD` 调 `detect_service`

---

## 5. 软件架构

### 5.1 `aov_service`（`src/aov/`）

1. 模式切换 `always_on` ↔ `aov`  
2. `SetSuspendTime` / `EnterSleep`（RKADK 语义自研封装）  
3. 协调 media / record / detect / storage / web / network  
4. 统计：循环次数、唤醒原因、lapse/事件次数、失败与 reinits  

**不**第一期整楼换 `RKADK_RECORD`。

### 5.2 模块改造

| 模块 | 改造 |
|---|---|
| `media_service` | `media_isp_single_frame` / `media_isp_multi_frame`（AIQ pause/resume） |
| `storage_service` | `storage_prepare_suspend` / `storage_resume_from_suspend` |
| `record_service` | 休眠前收段；AOV 全性能录像；禁止休眠中写文件 |
| `detect_service` | 唤醒后短跑 / 人车二次确认（可选） |
| `web_service` | AOV 拒预览；`/api/v1/aov/*` |
| `network_service` | 进 AOV 可 down eth0（D3） |

### 5.3 API

| 方法 | 路径 | 说明 |
|---|---|---|
| GET/POST | `/api/v1/aov/config` | `enabled/suspend_ms/event_fps/...` |
| GET | `/api/v1/aov/status` | 模式、循环计数、上次唤醒原因/耗时 |
| POST | `/api/v1/aov/enter` | `{"confirm":true}` |
| POST | `/api/v1/aov/leave` | 回常电 |

错误码：`26xx`。CLI：`ipc_app --aov-probe` / `--aov enter|leave|status`。

---

## 6. Web / 网络

| 场景 | 行为 |
|---|---|
| 进 AOV 前有预览 WS | 断开，提示低功耗不可预览 |
| 休眠中 HTTP | 失败（预期） |
| eth0 | 默认可 down（D3）；调试靠 Type‑C adb |

---

## 7. 分阶段实施

| 阶段 | 内容 | 状态 |
|---|---|---|
| Phase 0 | 文档纠偏为变帧率 AOV | 本文件 |
| Phase 1 | P0：打通 mem resume（串口/PMU 32K/GPIO + MIS5001 wakeup FW） | **阻塞中** |
| Phase 2 | 1fps lapse 闭环（单帧 + TF + sleep） | 依赖 Phase 1 |
| Phase 3 | MCU md → MultiFrame 20~30fps；安静后回 1fps | 依赖 Phase 1–2 |
| Phase 4 | 24h 长稳 + 功耗表 + 关 AOV 回归 | 依赖 Phase 3 |

---

## 8. 验收指标

| 指标 | MVP | 冲刺 |
|---|---|---|
| 静态 | ≈1fps 深睡循环；关网/关 Web | 功耗显著低于常电 |
| 事件 | 人为运动 → 全帧率录像 | 唤醒切流 ≤ 50ms |
| 回睡 | quiet / event_record 后回 1fps | — |
| 长稳 | ≥1000 次或 24h | — |
| 回归 | `aov.enabled=false` ≡ Phase 1 | — |

---

## 9. 决策点（已确认）

| ID | 确认 |
|---|---|
| D1 | 允许改固件 |
| D2 | suspend_ms=1000（1fps 节拍） |
| D3 | eth 可不保持 |
| D4 | 与 Web 预览互斥 |
| D5 | 媒体失败 → 重启进程 |
| D6 | MVP 无 PIR |

---

## 12. 修订记录

| 日期 | 说明 |
|---|---|
| 2026-07-27 | 初稿：定时唤醒 + 醒后短检 |
| 2026-07-28 | **纠偏**：变帧率 AOV（1fps↔20~30fps）；MCU FastAeMd；人车=唤醒后 IVS |
