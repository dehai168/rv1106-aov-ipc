# 工作日志（追加式，最新在最下）

> 每个工作会话结束前必须追加一条，格式见 `02-dev-process.md` §5.1。

## 2026-07-16 session 1
- 完成：项目设计文档（01-architecture / 02-dev-process / 03-todolist / 04-worklog / 05-api 骨架）
- 进行中：无
- 阻塞/待用户：无
- 板上状态：未部署任何代码；板子经 adb 连接，硬件就绪（MIS5001 + 64G TF + 纽扣电池）
- 下一步：T0.1 WSL 编译环境搭建

## 2026-07-16 session 2
- 完成：T0.1
  - WSL 依赖安装完成；SDK clone 到 `/home/user/work/luckfox-pico`
  - 工具链 `arm-rockchip830-linux-uclibcgnueabihf-gcc 8.3.0` 可用
  - 板型：`BoardConfig-SPI_NAND-Buildroot-RV1106_Luckfox_Pico_Pro_Max-IPC.mk`
  - `./build.sh media` 成功，RKMPI 例程 `simple_vi_get_frame` / `simple_vi_bind_venc` 已产出
  - `hello_rv1106` 板上运行成功；`simple_vi_get_frame` 日志确认 `m00_b_mis5001`
  - 本仓库 WSL clone：`/home/user/work/rv1106-aov-ipc`；`docs/env.md` 已写
- 进行中：T0.2 — 骨架与交叉编译完成，待板上部署验收
- 阻塞/待用户：跑 VI 例程后 adb 掉线（Windows 见“未知 USB 设备”），请拔插 USB 或重新上电后告知
- 板上状态：最后已知曾 `killall rkipc`；掉线后未能确认是否已拉起
- 已落地：
  - 目录骨架 `src/{main,common,media,record,detect,web,system}`、`cmake/toolchain-rv1106.cmake`
  - `ipc_app`：日志 + signal + 空事件总线；`scripts/build.sh` / `scripts/deploy.ps1`
  - WSL 编译通过：`build/ipc_app`（ARM uClibc ELF）
- 下一步：板子重连 → `.\scripts\deploy.ps1` → 确认日志 → 勾掉 T0.2 → 进入 T0.3

## 2026-07-16 session 3
- 完成：
  - T0.2 真机验收 — `/userdata/ipc_app` 输出 APP_START/exit
  - T0.3 — cJSON 1.7.18；文件日志；配置中心 merge+原子保存；`--self-test` 两次 PASSED；用户配置与日志落盘确认
- 进行中：无（M0 里程碑完成）
- 阻塞/待用户：无
- 板上状态：adb 正常；rkipc 运行中；`/userdata/ipc_app` `/userdata/default_config.json` `/userdata/ipc_config.json` `/userdata/log/ipc_app.log`
- 下一步：M1 / T1.1 RKAIQ+VI 初始化（会与 rkipc 争用摄像头，部署前需 killall rkipc）

## 2026-07-16 session 4
- 完成（部分）：T1.1 media_service
  - 链接 SDK `media/out`（rockit/rkaiq/mpp/rga）
  - `ipc_app --capture N`：AIQ init → VI → 预热丢弃 15 帧 → 写 NV12 到 `video.capture_dir`（默认 `/mnt/sdcard`）
  - 真机：sensor=`m00_b_mis5001`，IQ=`mis5001_CMK-OT2115-PC1_30IRC-F16.json`，稳定拿到 1920x1080 帧
  - 注意：`/userdata` 仅约 2MB，YUV 必须写 TF 卡；跑 capture 前 `killall rkipc`
- 阻塞/待用户：当前抓帧 y_mean≈10（几乎全黑+噪点），请确认镜头保护盖已取下、环境有可见光；确认后回复“再抓”，我复测验图
- 板上状态：已恢复 `rkipc`；产物在 `/mnt/sdcard/capture_0.yuv`
- 下一步：光照确认后勾掉 T1.1 → T1.2 双码流 VENC

## 2026-07-16 session 5
- 完成：
  - T1.1 复抓验图通过（y_mean≈106，可见场景；略虚焦）
  - T1.2 双码流：VI0→VENC0 H265 + VI1→VENC1 H264；`--encode 30` 产出 main.h265(~4.6MB/802pkt) + sub.h264(~651KB/801pkt)；PC ffprobe 可解析；编码后 available mem≈159MB
- 进行中：无
- 阻塞/待用户：无（镜头虚焦可后续手动调焦）
- 板上状态：已恢复 rkipc；码流在 `/mnt/sdcard/main.h265` `/mnt/sdcard/sub.h264`
- 下一步：T1.3 编码参数动态调整；或进入 M2 录像

## 2026-07-16 session 6
- 完成：
  - T1.3：`media_stream_*` 常驻双码流；apply 重建 VENC（分辨率变则重建 VI）；`--reconfig` PASSED（hi=1424156 lo=642489）
  - T2.1：`storage_service` — exFAT `/mnt/sdcard`、容量、`records/`+`snapshots/`、坏尾清理
  - T2.2：`record_service` + `librkmuxer`（放弃 minimp4）；路径 `records/YYYYMMDD/YYYYMMDD_HHMMSS.mp4`；`--record 70` → 3 段；样本已拉 `build/sample_record.mp4`
  - T2.3：低水位回收 API + 种 2000-01-01 文件冒烟删除
  - T2.4：`record_query` 与卡上文件一致
- 进行中：无（M1/M2 里程碑完成）
- 阻塞/待用户：请在 PC 上双击播放 `build/sample_record.mp4`（或卡上任一段）确认可播；确认后可 commit
- 板上状态：已恢复 rkipc；录像在 `/mnt/sdcard/records/20260716/`
- 下一步：M3 移动侦测（T3.1）

## 2026-07-16 session 7
- 完成：
  - T3.1：VI ch2 640x360（bypasspath）随 `media_stream_start` 创建
  - T3.2：`detect_service` + Rockit IVS；板上 GetResults ≈8/s；`ipc_app --detect N`
  - T3.3：主码流 GOP 预录环 + `record_arm_motion`/`record_on_motion`；force smoke 刷预录 71pkt→MP4，quiet 12s 回 IDLE；告警 `/mnt/sdcard/alarms/alarms.log`
  - media 多 listener：`add/remove_packet_listener`
- 进行中：无（T3.4 NPU 延后）
- 阻塞/待用户：请在镜头前走动复跑 `--detect 60`，确认 IVS 实触发（当前静止场景 square=0）；确认后可 commit
- 板上状态：测完需恢复 rkipc；新录像 `20260716_135945.mp4`
- 下一步：M4 Web（T4.1）或按需调 detect 阈值后进 M4

## 2026-07-16 session 8
- 完成：
  - 用户确认镜头前晃动；复跑 `--detect` 时 IVS `square` 仍恒 0（已改 MD_OD + rkipc 阈值）；链路/预录仍正常，实触发待光照调优
  - T4.1：mongoose 7.16 + `web_service`；`/api/v1/auth/{login,logout,password,me}`；默认 admin/admin + must_change；静态 `/userdata/www`
  - 板上 `sh /userdata/test_auth.sh`：未登录 1001、错密 1001、登录拿 token、Bearer `/me` ok、index.html 可访问
- 进行中：T4.2 前端骨架
- 阻塞/待用户：无（可选：开灯再测 detect；浏览器访问 `http://172.32.0.93:8080/`）
- 板上状态：`--web` 测完后需按需恢复 rkipc
- 下一步：T4.2 Vue3+Vite 登录页与主框架

## 2026-07-16 session 8b
- 完成：T4.2 `web-ui`（Vue3+Vite+TS+router）；登录/壳层/六功能占位；build gzip≈37KB；同步到 `www/`
- 进行中：无
- 阻塞/待用户：板上 `adb push www /userdata/www` 后浏览器打开 `http://<板IP>:8080/` 登录验证；确认后可 commit
- 下一步：T4.3 实时预览（子码流 fMP4/WS）

## 2026-07-16 session 9
- 进行中：T4.3 — `fmp4_mux` + `/api/v1/preview/ws` + `PreviewView` MSE；首连起流、末连停流
- 阻塞/待用户：kill rkipc 后 `--web`，Chrome 打开预览页确认出图
- 下一步：板上验收后勾选 T4.3，再进 T4.4

## 2026-07-16 session 10
- 完成：
  - T4.3 板上验收：`adb forward` + WS 收到 `avc1.641028` + ftyp/moof 共 20 片 PASSED；补 `PasswordView`
  - T4.4 实现中：`network_service` + `GET/POST /api/v1/network/config` + `NetworkView`（改 IP 二次确认；不改 usb0）
- 进行中：T4.4 板上 API/页面冒烟（eth0 无网线时 DHCP 可能失败属预期）
- 阻塞/待用户：改有线静态 IP 真机验收前请先同意（防失联）；PC 直连 `172.32.0.93` 若不通可用 `adb forward tcp:18080 tcp:8080`
- 板上状态：测 Web 时需 `killall rkipc`
- 下一步：T4.4 验收通过后进 T4.5

## 2026-07-16 session 11
- 完成：
  - T4.4 代码收尾：`network_service` + API + `NetworkView` + `scripts/test_network.sh`（板上冒烟待登录密码确认）
  - T4.5 实现中：`media_image_*`（RKAIQ）+ `GET/POST /api/v1/video/image|encode` + `VideoView`
- 进行中：T4.5 编译与板上验收（预览页调亮度/码率）
- 阻塞/待用户：板上 `test_network.sh` 需有效登录（若已改密请告知或浏览器验证）；静态 IP 改址仍须先同意
- 板上状态：测 Web 时需 `killall rkipc`
- 下一步：T4.5 验收后进 T4.6

## 2026-07-16 session 12
- 完成：
  - T4.6 存储与回放：`storage_service` 路径扫描 + `GET /api/v1/storage/status`、`GET /api/v1/storage/records`、`GET /api/v1/storage/download`
  - `web-ui` 新增 `StorageView`：播放走 `/api/v1/storage/download`（浏览器 Range）
  - `ipc_app` + `www/` 均已重新构建
- 进行中：板上验收（录像列表与播放是否正常；format 仍未实现）

## 2026-07-16 session 13
- 完成：
  - T4.7 告警接口：`GET/POST /api/v1/alarm/motion`、`GET /api/v1/alarm/events`
  - Web 自动化：预览启动时自动拉起 detect + motion-triggered record（写入 `/alarms/alarms.log`）
  - `web-ui` 新增 `AlarmView`（配置+事件列表），路由已接入 `/alarm`
  - `ipc_app` + `www/` 已重新构建并推送到板上
- 进行中：区域编辑/布防时间等高级能力待后续实现；板上 motion->alarms.log 验收请你确认

## 2026-07-16 session 14
- 完成：
  - T4.8 系统管理：`system_service` + `GET/POST /api/v1/system/*`（info/time/reboot/reset/log）
  - `web-ui` 新增 `SystemView`（设备信息、时间、日志下载、重启/恢复出厂二次确认）
  - `scripts/test_system.sh`（不含 reboot/reset）
- 进行中：编译 + `www/` 打包 + 板上验收 T4.5–T4.8
- 阻塞/待用户：重启/恢复出厂真机测试需明确同意；登录密码若已改请设 `IPC_PASS` 后跑 `scripts/test_m4.sh`

## 2026-07-16 session 15
- 完成：
  - M4 板上冒烟脚本：`lib_login.sh`（支持 `IPC_PASS`/`admin1` 回退）、`test_storage.sh`、`test_alarm.sh`、`test_m4.sh`
  - T5.1：`system_time_init` 开机 `hwclock -s` + NTP 周期线程（`system.ntp.interval_min`）
  - T5.2：`S99ipc_app`、`ipc_watchdog.sh`、`install_init.sh`
  - `scripts/test_time.sh`
- 阻塞：板上 `test_m4.sh` 登录失败（`admin`/`admin1` 均无效），需你提供 `IPC_PASS` 或浏览器确认
- 下一步：M4 验收通过后勾选 T4.5–T4.8；T5.1/T5.2 板上验收；T5.3 长稳

## 2026-07-16 session 16
- 完成：
  - 恢复出厂：删除 `/userdata/ipc_config.json` 后重启；`test_m4.sh` **ALL M4 SMOKE OK**（T4.5–T4.8）
  - T5.1：`test_time.sh` PASSED；BusyBox TZ 映射 `Asia/Shanghai`→`CST-8`
  - T5.2：`/etc/init.d/S99ipc_app` 已安装；`test_watchdog.sh` kill-9 → 12s 内拉起 PASSED
  - T5.3：`soak_monitor.sh` 推板并启动过夜采样 → `/userdata/log/soak.csv`
- 当前登录：冒烟脚本首登改密为 **admin1**（`IPC_PASS=admin1`）
- 待明天手测：浏览器预览/存储回放/告警 motion；断电重启验 RTC+自启；看 soak.csv RSS
- 下一步：T5.3 结果入库 worklog → T5.4 README/review

## 2026-07-17 session 17
- 现象：浏览器预览无图后 Web 不可达
- 根因：
  1. 预览冷启拉起 1080p 主码流 + detect/record，进程退出
  2. watchdog 重启缺 `LD_LIBRARY_PATH=/oem/usr/lib` → `can't load library librockit.so` 死循环
  3. 曾出现双实例抢 8080（EADDRINUSE）
- 修复：
  - watchdog/S99：库路径 + flock 单实例；stderr 与 app log 分离
  - 预览不再自动起 detect/record（告警页 apply 仍可拉）
  - PreviewView 重连间隔 1s→2s
- 验证：`repro_preview_crash.py` PASS（avc1 + ≥20 fMP4）；板上 Web 已恢复
- 当前登录：`admin` / `admin1`；请硬刷新后再测预览页

## 2026-07-17 session 18
- 现象：WS 有推流、存储 MP4 可播，但预览黑屏
- 根因：`fmp4_mux` fragment `trun` flags=`0x105`（first-sample-flags）与实际写入的 duration/size/flags 布局不匹配，Chrome MSE 解不出
- 修复：`trun` flags → `0x701`（data-offset|duration|size|flags）；PreviewView 加强 MSE（Infinity duration、segments mode、调试行）
- 验证：dump 后 `ffprobe` 识别 H.264 704x576@15；板上已部署，请 Ctrl+F5 硬刷新预览页

## 2026-07-17 session 19
- 手测结果：①预览出图 ②回放 MP4 OK ③告警无事件（因先前关闭预览自动侦测）④断电时间大致准
- 修复告警：预览开启 detect VI；起流后延后 ~3s 再启 IVS+record；告警页 3s 自动刷新
- 请：Ctrl+F5 → 开预览等 5s → 走动 → 看告警页「running」与事件列表

## 2026-07-17 session 20
- 手测确认：告警页已能看到移动侦测事件 → **M4 闭环**
- 下一步：确认 T5.2 断电后 Web 是否自启；提交修复补丁；T5.3 正式长稳 / T5.4 README

## 2026-07-17 session 21
- 手测确认：断电上电后 Web **自动启动** → T5.2 完成
- 下一步：提交预览/告警/watchdog 修复 commit；启动 T5.3 正式 24h soak；并行起草 T5.4 README





