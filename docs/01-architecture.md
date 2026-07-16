# 系统架构与技术方案设计

> 本文档是 rv1106-aov-ipc 项目的总体设计，是所有开发 agent 的第一参考。修改架构级决策前必须征得用户同意并更新本文档。

## 1. 项目目标

基于 Luckfox Pico Max（RV1106G3）开发一台 AOV（Always-On Video）智能 IPC，实现：

1. 移动物体侦测，触发后自动快起录像；
2. 录像以 MP4 分段写入 TF 卡，按时长分段、按空间循环覆盖，TF 卡插到 PC 可直接读取；
3. 通过网口访问的 Web 管理界面（登录/预览/网络/图像/存储回放/告警/系统管理）；
4. RTC 校时（NTP 同步 + 纽扣电池保持）。

## 2. 硬件平台

| 项 | 参数 |
|---|---|
| SoC | RV1106G3，Cortex-A7 1.2GHz，NPU 1TOPS(int4/8/16) |
| ISP | 最大 5M@30fps |
| 内存 | 256MB DDR3L（应用可用内存需精打细算） |
| Sensor | MIS5001 5MP，MIPI CSI 2-lane，最大 2592x1944 |
| 存储 | SPI NAND 256MB（系统）+ TF 卡 64GB（录像） |
| 网络 | 10/100M 以太网（无 Wi-Fi） |
| RTC | 纽扣电池已连接 |
| 调试 | ADB 直连 PC，可 push/shell |

## 3. 软件总体架构

```
┌─────────────────────────────────────────────────────────┐
│ Web 前端（静态页面，打包进固件 /oem/www）                    │
├─────────────────────────────────────────────────────────┤
│ 业务层（自研，C/C++，单进程多线程 ipc_app + 模块化服务）      │
│  ┌──────────┬──────────┬──────────┬──────────┬────────┐ │
│  │ media    │ record   │ detect   │ web      │ system │ │
│  │ 采集编码  │ MP4录像   │ 移动侦测  │ HTTP/API │ 网络/时 │ │
│  │ 双码流    │ 循环覆盖  │ +NPU确认 │ +预览推流 │ 间/配置 │ │
│  └──────────┴──────────┴──────────┴──────────┴────────┘ │
├─────────────────────────────────────────────────────────┤
│ 媒体中间件：Rockit MPI（VI/VENC/IVS）、RKAIQ(3A)、RGA、    │
│            RKNN Runtime（NPU 推理）                       │
├─────────────────────────────────────────────────────────┤
│ 系统层：Luckfox Pico SDK（buildroot rootfs、内核、驱动：    │
│        ISP/MIPI/VENC/NAND/TF/RTC/EMAC）                  │
└─────────────────────────────────────────────────────────┘
```

### 3.1 模块职责

- **media_service**：初始化 RKAIQ + VI，创建双码流 VENC：
  - 主码流：H.265 2592x1944（或 2560x1440）@15fps，录像用；
  - 子码流：H.264 704x576（或 640x480）@15fps，Web 预览用；
  - 第三路低分辨率 NV12 帧（如 640x360），供移动侦测。
- **record_service**：从主码流取帧，用 MP4 muxer 分段写 TF 卡；管理 TF 卡挂载、容量水位与循环覆盖；维护录像索引供回放查询。
- **detect_service**：低分辨率帧差/背景建模做移动侦测（首选 Rockit IVS 移动侦测通道，退路为自研帧差）；可选用 RKNN 跑轻量人形检测（如 yolov5s-int8 裁剪版）做二次确认降误报；输出事件给 record/web。
- **web_service**：嵌入式 HTTP 服务（选型 mongoose，单文件、license 友好、支持 WS）；提供 REST API + 静态页面 + WebSocket 预览流。
- **system_service**：配置中心（JSON 持久化 + 热生效）、网络配置（IP/DHCP/网关/DNS）、时间管理（NTP + RTC）、日志、系统维护（重启/恢复出厂/固件升级）。
- **aov_service**（二期）：低功耗待机与快起调度，见 §4.5。

### 3.2 进程模型

单主进程 `ipc_app`，模块以线程 + 消息/事件总线（简单发布订阅）协作，降低 256MB 内存下的多进程开销。Web 前端为纯静态资源。

## 4. 关键技术方案

### 4.1 视频链路

```
Sensor(MIS5001) → ISP(RKAIQ) → VI
   ├─ VI ch0 ─→ VENC0 H.265 主码流 ─→ record_service → MP4/TF卡
   ├─ VI ch1 ─→ VENC1 H.264 子码流 ─→ web_service → WS 预览
   └─ VI ch2 (640x360 NV12) ─→ detect_service（IVS/帧差 → 可选 RKNN 确认）
```

- 编码参数（分辨率/码率/帧率/GOP/CBR-VBR）全部走配置中心，Web 可改，改后重建对应 VENC 通道。
- 参考实现：Luckfox SDK 例程 `luckfox-pico/example`（RKMPI VI+VENC 示例）与 wiki 的 RKMPI 文档。

### 4.2 录像与存储

- TF 卡使用 **exFAT**（64GB，PC 即插即读；内核需确认 exFAT 支持，否则用 FAT32+4G 以下分段）。
- 目录结构：`/mnt/sdcard/records/YYYYMMDD/YYYYMMDD_HHMMSS.mp4`，另有 `/mnt/sdcard/snapshots/` 存事件抓图。
- 分段：按配置时长（默认 60s，可配 30–600s）切文件；每段以 IDR 帧开头。
- 循环覆盖：写入前检查容量，剩余空间低于水位（默认 10%）时按最旧日期目录逐个删除。
- MP4 封装选型：**minimp4**（header-only，轻量）；退路 mp4v2。不引入 ffmpeg（体积与内存不允许）。
- 异常安全：每段落盘时定期 `fsync` + moov 尾部写入策略；掉电导致的坏尾文件在启动时扫描清理。

### 4.3 移动侦测与录像联动（状态机）

```
IDLE ──侦测命中(连续N帧)──→ RECORDING（立即含预录缓冲）
RECORDING ──静默超时(默认30s无运动)──→ IDLE
```

- 预录：detect 侧维持主码流最近 3–5s 的 GOP 环形缓冲，触发时先写入缓冲再续写实时流，保证不丢起始画面。
- 事件同时产生：告警记录（含抓图）、Web 端事件推送。

### 4.4 Web 管理

- 后端：mongoose HTTP + REST（`/api/v1/...`），JSON 报文；登录发放 token（HttpOnly cookie，超时可配），密码加盐哈希存储；预留 HTTPS。
- 预览：子码流 H.264 经 WebSocket 以 fMP4 分片推送，前端 MSE（`mpegts.js`/`jessibuca` 二选一）播放；同时提供 RTSP（可选，方便 VLC 调试）。
- 前端：Vue3 + Vite 构建为纯静态资源，构建产物 gzip 后放入 `/oem/www`；控制体积 < 2MB。
- 功能页面与 API 域对应：登录注销 / 实时预览 / 网络参数 / 图像与编码参数 / 存储与回放（文件列表、下载、在线播放）/ 告警与智能分析（侦测开关、灵敏度、区域、布防时间）/ 系统管理（设备信息、时间、用户密码、重启、恢复出厂、升级）。

### 4.5 AOV 低功耗快起（二期）

分两阶段落地，避免一开始就被低功耗阻塞：

- **Phase 1（常电模式）**：进程常驻，侦测→录像全链路先跑通。
- **Phase 2（AOV 模式）**：利用 RV1106 AOV 能力：系统 suspend 待机，RTC 定时唤醒（如 1s/次）快起出流 → 单帧/短序列侦测 → 无事件回到 suspend，有事件转连续录像。依赖 SDK 的 AOV 参考方案（Luckfox wiki AOV 章节），涉及内核挂起/快速出流调优。

### 4.6 时间管理

- 开机：`hwclock -s` 从 RTC 恢复系统时间；
- 网络可用时：NTP（可配服务器，默认 pool.ntp.org + 阿里 NTP）定期同步，成功后 `hwclock -w` 写回 RTC；
- Web 可手动设置时间与时区。

### 4.7 配置中心

- 运行配置：`/userdata/ipc_config.json`；出厂默认：固件内 `default_config.json`。
- 启动时合并（缺项补默认），所有模块通过配置中心 API 读写；写入原子化（临时文件 + rename）。
- 恢复出厂 = 删除用户配置后重启。

## 5. 代码仓库结构（目标形态）

```
rv1106-aov-ipc/
├── docs/                  # 全部文档（本目录）
├── src/
│   ├── main/              # ipc_app 入口、事件总线
│   ├── media/             # media_service
│   ├── record/            # record_service + mp4 muxer
│   ├── detect/            # detect_service + rknn
│   ├── web/               # web_service + REST handlers
│   ├── system/            # 配置/网络/时间/日志
│   └── common/            # 公共工具
├── web-ui/                # 前端工程（Vue3 + Vite）
├── third_party/           # mongoose / minimp4 等
├── scripts/               # 编译、adb 部署、打包脚本
├── config/                # default_config.json 等
└── CMakeLists.txt         # 交叉编译构建入口
```

## 6. 资源预算（256MB 内存约束）

- 内核 + rootfs 常驻 ≈ 60–80MB；rockit/RKAIQ ≈ 60–90MB（与分辨率相关）；
- ipc_app 目标 < 50MB（含预录缓冲，主码流 4Mbps×5s ≈ 2.5MB 可控）；
- 严禁引入大型依赖（ffmpeg、node、python 服务端一律不用）；前端只做静态资源。

## 7. 外部参考

- Luckfox 开发板 wiki：https://wiki.luckfox.com/Luckfox-Pico-RV1106
- 官方资料仓库：https://github.com/LuckfoxTECH/Luckfox-Pico-docs
- SDK：https://github.com/LuckfoxTECH/luckfox-pico（含交叉编译链 `arm-rockchip830-linux-uclibcgnueabihf`、RKMPI 例程、AOV 参考）
