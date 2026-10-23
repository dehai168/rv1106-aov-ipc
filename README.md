# rv1106-aov-ipc

基于 **Luckfox Pico Max（RV1106G3）+ MIS5001** 的 AOV 智能 IPC：移动侦测录像、TF 卡分段 MP4、Web 管理、可选变帧率 AOV。

## 文档（按顺序）

1. [docs/01-architecture.md](docs/01-architecture.md) — 架构
2. [docs/02-dev-process.md](docs/02-dev-process.md) — 开发流程
3. [docs/03-todolist.md](docs/03-todolist.md) — 任务进度
4. [docs/04-worklog.md](docs/04-worklog.md) — 工作日志
5. [docs/05-api.md](docs/05-api.md) — REST API
6. [docs/06-todolist-ui.md](docs/06-todolist-ui.md) — Web UI（M7）
7. [docs/env.md](docs/env.md) — 本机环境实况

## 功能概览

| 能力 | 说明 |
|---|---|
| 双码流 | 主 H.265 录像；子 H.264 预览；另有 ≤1280×720 H.264 主预览通道 |
| 录像 | 事件 / 连续 / 周计划；分段 MP4；低水位循环覆盖 |
| 侦测 | Rockit IVS + 多区域软过滤 + 7×24 布防 |
| Web | Element Plus；顶栏预览/信息/设置；`LivePreviewPlayer` 复用；Tab 化业务页；OSD 中文+位置 |
| AOV | 用户态变帧率主线可用；MCU 睡时 MD / 功耗表暂搁 |

**不做**：PTZ、音频对讲、真卡 mkfs、在线升级、ONVIF 等。

板端 OSD 字体：部署 `assets/fonts/simsun_cn_3000.ttf` → `/userdata/fonts/`（或配置 `video.osd.font_path`）。  
录像分段暂存：`/mnt/sdcard/records/.stage/`（勿用 `/tmp`，长分段会缺 moov）。

## 编译（WSL）

```bash
# 依赖：Luckfox SDK + 交叉工具链（见 docs/env.md）
cd /mnt/d/CodeProject/rv1106-aov-ipc
./scripts/build.sh
# 产物：build/ipc_app
```

前端：

```bash
./scripts/pack_www.sh   # npm build → www/（需 <1.5MB）
```

## 部署（adb）

```powershell
# 二进制（userdata 空间紧，可经 TF 中转）
adb push build/ipc_app /mnt/sdcard/ipc_app_new
adb shell "killall ipc_app; cp /mnt/sdcard/ipc_app_new /userdata/ipc_app; chmod +x /userdata/ipc_app"

# www（BusyBox tar 无 -z，用未压缩 tar）
# WSL: tar -C www -cf /tmp/www.tar .
adb push \\wsl$\Ubuntu\tmp\www.tar /mnt/sdcard/www.tar
adb shell "rm -rf /userdata/www; mkdir -p /userdata/www; tar -C /userdata/www -xf /mnt/sdcard/www.tar"

adb shell "export LD_LIBRARY_PATH=/oem/usr/lib:/usr/lib; nohup /userdata/ipc_app --web >/tmp/ipc_app.log 2>&1 &"
```

开机自启：`scripts/S99ipc_app` + `ipc_watchdog.sh`（见 T5.2）。

## 使用

1. 浏览器打开 `http://<板IP>`（80 端口；`:8080` 仍兼容）。`S99ipc_app start` 会停用 OEM 自带的 nginx 网页（rc 脚本移到 `/oem/usr/etc/init.d/disabled/`）
2. 默认账号 **admin / admin**（首登强制改密）
3. 预览：子码流 / 主预览切换；滚轮或滑条数字缩放；抓图
4. 录像计划：事件 / 连续 / 定时；存储页回收水位
5. 摄像机 → OSD：时间与通道名 RGN 烧录
6. 系统：设备名、定时重启、日志尾部预览与下载

配置：`/userdata/ipc_config.json`（合并 `default_config.json`）。  
录像/抓图：`/mnt/sdcard/records|snapshots|alarms`。

## 常用 CLI

```text
ipc_app --web
ipc_app --record 30
ipc_app --detect 25
ipc_app --aov status|enter|leave|loop N [--media]
```

## 已知限制

- 主预览为辅助 **H.264**，不是录像用的 H.265 裸流 MSE
- 主预览分辨率上限约 1280×720（省内存）
- 无 PTZ / 音频 / Wi‑Fi / 人形 NPU（T3.4 延后）
- AOV：`allow_mem_suspend` 默认可关；MCU wakeup FW 待板级

## 硬件摘要

Luckfox Pico Max：RV1106G3、256MB DDR、MIS5001 MIPI、64GB TF、有线网、RTC 纽扣电池、ADB。
