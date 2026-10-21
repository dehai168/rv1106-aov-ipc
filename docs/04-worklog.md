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

## 2026-07-17 session 22
- 补齐：存储软 format、告警灰度抓图、布防区域/时间表
  - `POST /api/v1/storage/format`（confirm）清空 records/snapshots/alarms
  - 运动触发 → `media_snapshot_jpeg` → `/mnt/sdcard/snapshots/YYYYMMDD_HHMMSS.jpg`，写入 alarms.log `snapshot`
  - `detect.region.*` / `detect.schedule.*` + AlarmView 表单；软件过滤（非 IVS ROI）
  - UI：StorageView 清空按钮；AlarmView 区域/时间/缩略图
- 烤机（T5.3）延至下午/晚间挂机；UI 细节下午手测反馈后再调

## 2026-07-17 session 23
- 用户要求：Element Plus 优化 UI；功能对齐 VIGI 模拟器（除 PTZ）；先出差异表与新 todolist
- 产出：
  - `docs/design/ui-vigi-gap.md` — VIGI vs 本板功能点对照与做/不做边界
  - `docs/06-todolist-ui.md` — M7 任务清单（T7.0–T7.8）
- 下一步：用户确认边界后从 T7.0 工程基线开工

## 2026-07-17 session 24
- 开工 M7：T7.0 + T7.1 落地
  - Element Plus 按需导入 + 中文 locale；浅色产品壳（青灰强调）
  - `AppLayout`：顶栏 Preview/信息/设置 + 设置侧栏树；旧路由 redirect
  - 登录/预览/图像码流/网络/存储/告警/系统 页面 Element 化（功能不回归）
  - 新增信息：设备信息、系统日志下载
  - 构建：www ≈746KB（gzip JS≈188KB）；已可部署硬刷验证
- 下一步：T7.2 预览抓图/全屏；T7.5 画框+7×24；T7.6 录像计划；全部基本功能后再开 M6 AOV

## 2026-07-27 session 25 — T5.3 一周挂机结论
- 采样时间：约 2026-07-17 ~ 2026-07-27；系统 `uptime ≈ 9d22h`；`ipc_app` PID **3345** 全程未换（watchdog 未拉起重启）
- 当前仍可用：登录 OK；`/system/info` OK；`uptime_sec≈856671`；TF 挂载 exFAT，空闲约 98%
- 内存：`VmRSS≈7.4MB`（soak 首日均值≈7.6MB，末段≈7.9MB）— **无明显泄漏**
- soak.csv（仅首日 24h，`DURATION_SEC=86400`）：1439 点，`http_ok` **0 失败**；pid 1316→3345（部署换进程）
- 日志：
  - **2026-07-17 13:00 之后 ERROR = 0**（此前 ERROR 几乎全是双实例抢 8080 `listen failed`，属开发期）
  - WARN 主因：每小时 NTP（`ntpdate`/`busybox ntpd -q` rc=32512，板内无此二进制；系统已有 `/usr/sbin/ntpd`）
  - MOTION：7/17 手测 1054 次 + 7/25 16 次；抓图 `media snapshot`≈960；卡上 records≈63 段 / snapshots≈434 / alarms.log 1070 行
- Loadavg 常驻 ≈10 但 `top` 显示 **CPU 100% idle**，`ipc_app` 0%CPU — 属内核/D 态任务假负载，非应用忙环
- `/userdata` 占用偏高（≈84%，日志 5.6MB）— 二期注意日志轮转
- **判定：T5.3 长稳 PASS**，可进二期 M6 AOV；并行可修 NTP 调用与日志轮转
- 下一步：产出 `docs/design/aov.md` + M6 todolist；M7 剩余 UI（画框/计划）可与 AOV 调研并行

## 2026-07-27 session 26 — M6 T6.1 设计定稿
- 产出：
  - `docs/design/aov.md`：目标/现状/SDK 机制/状态机/模块落点/API 草案/风险/指标/决策点
  - `docs/07-todolist-aov.md`：T6.1–T6.6 可执行细项
  - 修正架构表述：定时唤醒主路径为 **HPMCU 寄存器（`0xff300048`）**，Linux RTC alarm **不作 MVP**
- SDK 依据：`rkadk_aov.c`（`echo mem` + `io -4 0xff300048`）、`rkadk_aov_record_test.c`（睡前 umount/unbind TF）
- **待用户确认 D1–D6**（尤其 D1：是否允许改内核/刷固件）后进入 T6.2 探针

## 2026-07-27 session 27 — D1–D6 确认 + T6.2 探针
- 用户确认：
  - D1=允许改固件；D2=1000ms；D3=网口可不保持（Type‑C adb）；D4=预览互斥；D5=重启进程；D6=无 PIR
- 探针（现 Buildroot 内核）：
  - `/sys/power/state` 空；`echo mem` → `Function not implemented`
  - SDK `luckfox_rv1106_linux_defconfig`：`# CONFIG_SUSPEND is not set`
  - `io`/`devmem` 已存在；`ipc_app` 探针后仍存活
- **结论：现固件无法 AOV → 进入 T6.3**：为 Max 板级增加 `RK_KERNEL_DEFCONFIG_FRAGMENT="rv1106-pm.config"` 后重编/刷内核
- 记录：`docs/design/aov-probe-notes.md`

## 2026-07-27 session 28 — T6.3 内核 SUSPEND 上板（摄像头已恢复）
- 坑：
  1. **只刷 boot、不换匹配 `.ko`** → `Can't find GRF syscon`，无 `/dev/video*`（`CONFIG_PM_SLEEP` 与 OEM 旧模块 ABI 不兼容）
  2. 首次「备份」`boot-backup-pre-aov.img` 实际已是 SUSPEND 内核，不能当原厂回滚
  3. 坏内核上 `reboot`/`reboot -f` 会挂死；可用 `io -4 -w 0xff3b0c08 0xfdb9`（CRU `GLB_SRST_FST`）硬软复位
- 回滚路径：重编无 fragment 的 `boot-stock-no-suspend.img`（`# CONFIG_SUSPEND is not set`）→ 摄像头恢复
- Go 路径：
  - BoardConfig：`RK_KERNEL_DEFCONFIG_FRAGMENT="rv1106-pm.config"`
  - fragment 追加 `CONFIG_ROCKCHIP_GRF=y`
  - `./build.sh kernel` + `make -C sysdrv drv`，同步媒体相关 `.ko` 到 `/oem/usr/ko`
  - 刷 `mtd3`：先 push 再 `flash_erase` + `nandwrite -p`
- **板上现状（验证通过）**：
  - 内核 `#1 Mon Jul 27 13:25:38 CST 2026`
  - `/sys/power/state` = `freeze mem`；`CONFIG_SUSPEND=y` / `ROCKCHIP_SUSPEND_MODE=y` / `ROCKCHIP_GRF=y`
  - CSI probe OK，`/dev/video0` 存在；`ipc_app --web` 在跑
- 产物：`build/firmware/boot-aov-pm.img`、`boot-stock-no-suspend.img`、`ko-aov-pm/`；TF 上同名 + `oem-ko-backup-pre-aov/`
- 下一步：T6.3.4 / T6.2.4（`io` 写 `0xff300048` + `echo mem` 唤醒循环）；然后 T6.4 `aov_service`

## 2026-07-27 session 29 — T6.3.4 唤醒探针 FAIL
- 单次：`io -4 0xff300048 96000`（3s）+ `echo mem` → **能睡，不能自动醒**（adb 断 >2min）
- 寄存器可写可读（`0xff300048` → `0x17700`）
- 根因：当前内核 **`# CONFIG_RV1106_PMU_WAKEUP_TIMEOUT is not set`**
  - SDK `rv1106-pm.config` 只有 SUSPEND；超时唤醒在 `rv1106-wakeup.config`（`CONFIG_RV1106_PMU_WAKEUP_TIMEOUT=y`）
  - 内核 `rv1106_pm.c` 仅在该 config 下置位 `PMU_WAKEUP_TIMEOUT_EN`
- 板上 `hpmcu_wrap.bin`（4KB）是 ISP wrap 用；AOV 另有 `rv1106_hpmcu_wakeup_v1.00.bin`（109KB）— 是否替换待 PMU timeout 验证后再定
- 处理：往 `rv1106-pm.config` 追加 `CONFIG_RV1106_PMU_WAKEUP_TIMEOUT=y` 后重编/刷 boot
- **板子又卡在 suspend，需用户断电上电**

## 2026-07-27 session 30 — PMU timeout 内核仍不能自动醒
- 已刷：`#1 Mon Jul 27 15:55:04 CST 2026`，`CONFIG_RV1106_PMU_WAKEUP_TIMEOUT=y` + CSI OK
- 复测：`io -4 0xff300048 96000` + `echo mem` → adb 断，**3min 未回**
- 说明：仅开 PMU timeout **不够**；下一步候选：
  1. 加载 `rv1106_hpmcu_wakeup_v1.00.bin`→`0x40000`（RKTRUST WAKEUP.ini；与 wrap@`0xff6fe000` 不同）
  2. 查 32K/OSC（`PMU_OSC_DIS_ENA`）：无 32K 则超时永不触发
  3. 串口看 suspend/resume 日志；或 GPIO wakeup 验证 resume 路径
- **板子再次卡死，需断电上电**

## 2026-07-27 session 31 — freeze 也断 adb；怀疑 USB 不恢复
- 板上：`clk_rtc_32k` 存在（32768）但 **enable_count=0**
- `echo freeze` 同样导致 adb 丢失（freeze 本不该依赖 PMU timeout）
- 新假设：**系统可能已 resume，但 USB gadget/adb 未恢复** → 主机侧表现为“死机”
- 验证法：suspend 前后写 TF 日志 `/mnt/sdcard/aov_resume_probe.log`；断电后读卡判断是否有 `RESUME_OK`
- TF 探针结果（16:28:56）：仅有 `before_mem up=64`，**无 `RESUME_OK`**
- freeze 探针（16:47:31）：仅有 `before_freeze up=72`，**同样无 RESUME_OK**
- 上板计划：见 `docs/design/aov-hpmcu-wakeup-plan.md`（Trust `RV1106TOS_WAKEUP.ini` + DTS `aov-reserved`）

## 2026-07-27 session 31 — DTS `aov-reserved` + boot 刷写
- 改 SDK：`rv1106-luckfox-pico-pro-max-ipc.dtsi` → `&rkcif_mipi_lvds { aov-reserved; }`
- 重编/刷 `boot-aov-pm.img`（md5 `46f4e8f…`）；TF 备份 `boot-backup-pre-aov-reserved.img`
- 板上：`rkcif-mipi-lvds/aov-reserved` 存在；CSI OK
- **未再跑 suspend 探针**（避免卡死）；下一步：idblock/uboot + `RV1106TOS_WAKEUP.ini`

## 2026-07-27 session 32 — idblock/uboot + `RV1106TOS_WAKEUP.ini`
- SDK 新增 `luckfox_rv1106-wakeup.config`：`CONFIG_TRUST_INI="RV1106TOS_WAKEUP.ini"`
- BoardConfig：`RK_UBOOT_DEFCONFIG_FRAGMENT="rk-sfc.config luckfox_rv1106-wakeup.config"`
- `./build.sh uboot` 成功；FIT 含 `mcu1` @ `0x00040000`（hpmcu_wakeup）
- 产物：`build/firmware/idblock-wakeup.img`（md5 `53a15488…`）、`uboot-wakeup.img`（md5 `5ebd18f4…`）
- TF 备份：`idblock-backup-pre-wakeup.img`、`uboot-backup-pre-wakeup.img`（刷前 md5 与 mtd 一致）
- 已刷 mtd1+mtd2；CRU 复位后 **正常启动**；`/dev/video0`、`aov-reserved` OK
- 跑 `test_aov_resume_probe.sh mem 3000` → adb 丢失，**45s 未回**（与改前类似）
- **需用户断电上电** 后读 `/mnt/sdcard/aov_resume_probe.log` 确认是否有 `RESUME_OK`

## 2026-07-27 session 33 — WAKEUP trust 后 mem 探针仍 FAIL
- 用户断电上电后读 TF 日志（18:08:22 探针）：
  - `before_mem up=120` — **有**
  - `after_mem` / `RESUME_OK` / `aov_resume_ok.flag` — **均无**
- 结论：**不是 USB 假死**，系统仍未从 mem 恢复；`RV1106TOS_WAKEUP.ini` 单独不足
- 板上内核仍缺 `CONFIG_VIDEO_CAM_SLEEP_WAKEUP`；debugfs 未挂载，32K 待查
- 下一步候选（按风险递增）：
  1. 内核 fragment 加 `CONFIG_VIDEO_CAM_SLEEP_WAKEUP=y`（仅 boot.img）
  2. suspend 前读/写 PMU `0xff300000` 域（32K/OSC enable）
  3. GPIO 唤醒探针（验证 resume 路径是否通）
  4. 串口抓 suspend/resume 日志

## 2026-07-27 session 34 — 无 rockit 探针（跳过 insmod rockit）
- **运行时 `rmmod rockit` 会卡死**（refcnt 变 -1），无法用卸载方式去掉 wrap 固件
- 改 `/oem/usr/ko/insmod_ko.sh` 注释 `__insmod rockit` 行 → 重启后 `lsmod` **无 rockit**（`hpmcu_wrap` 未加载）
- 备份：`/userdata/insmod_ko.sh.bak`；恢复：`cp /userdata/insmod_ko.sh.bak /oem/usr/ko/insmod_ko.sh`
- 无 rockit 下跑 mem 3s → adb 仍断，**待断电读 TF 日志**（应有 `before_mem_no_rockit` 行）

## 2026-07-28 session 35 — 无 rockit 探针结论：仍 FAIL
- TF 日志：`before_mem_no_rockit Mon Jul 27 21:55:53` — **有**；无 `RESUME_OK` / 无 flag
- **排除**「`hpmcu_wrap` 与 wakeup 固件冲突」假说
- 已恢复 `/oem/usr/ko/insmod_ko.sh`（从 `/userdata/insmod_ko.sh.bak`）；需重启才加载 rockit
- 下一步优先：内核 `CONFIG_VIDEO_CAM_SLEEP_WAKEUP=y`（仅 boot）或 PMU 32K / GPIO 唤醒探针

## 2026-07-28 session 36 — `CONFIG_VIDEO_CAM_SLEEP_WAKEUP=y`
- `rv1106-pm.config` 追加 `CONFIG_VIDEO_CAM_SLEEP_WAKEUP=y`；`./build.sh kernel` OK
- 刷 `boot-aov-pm.img`（md5 `fb37dfda…`）；内核 `#2 Tue Jul 28 10:20:07 CST 2026`
- 板上：`CONFIG_VIDEO_CAM_SLEEP_WAKEUP=y`；`/dev/video0` OK；rockit 已恢复
- 注意：SDK 中该开关主要给 SC*/imx415 传感器 sleep/wake；**MIS5001 驱动未引用**
- 已跑 mem 3s 探针 → adb 断；**待断电读 TF 日志**

## 2026-07-28 session 39 — FE-PHY 关断进睡（官方 AOV 贴士）
- 贴士：申请 `Rockchip_RV1106_Developer_Guide_Linux_AOV_CN.pdf`；`echo mem` 前须关断内部 FE-PHY
- 实现：`network_prepare_aov_suspend()` = wakeup disabled + eth0 down + **unbind rk_gmac-dwmac**
  （驱动 `rk_gmac_powerdown` → `RV1106 FEPHY SHUTDOWN`）
- 脚本：`scripts/test_aov_fephy_off_mem.sh`；文档：`aov.md` §3.1、`aov-resume-blockers.md` §6
- 待板上复测 mem 探针是否出现 `RESUME_OK`

## 2026-07-28 session 39b — FE-PHY off 后 mem 探针仍挂起
- 已执行 `test_aov_fephy_off_mem.sh 3000`（unbind gmac 已在板上验证可回绑）
- adb 再次丢失，**短时未回** → FE-PHY 关断是必要步骤，但**不足以单独打通 resume**
- 待用户断电读 TF：应有 `before_mem_fephy_off`；有无 `RESUME_OK` 决定下一步（串口/32K/MIS5001 MCU）

## 2026-07-28 session 40 — FE-PHY FAIL 确认 + deep suspend 根因
- 用户 TF：`gmac_unbound` + `before_mem_fephy_off`；**无** `RESUME_OK` / `aov_resume_ok.flag`
- 官方 PDF：`docs/Rockchip_RV1106_Developer_Guide_Linux_AOV_CN.pdf`
- **根因**：`/sys/power/mem_sleep` 仅 `[s2idle]`；`echo deep` 无效
  - 板级 `compatible` 只有 `rv1103g-…` / `rv1106g3`，**无** `"rockchip,rv1106"`
  - `mach-rockchip` `DT_MACHINE` 不匹配 → `rockchip_suspend_init` 未跑 → 无 deep ops
  - EVB 均为 `…, "rockchip,rv1106"`
- SDK 已改：`rv1106g-luckfox-pico-pro-max.dts` 追加 `"rockchip,rv1106"`  
  备份：`*.dts.bak-before-rv1106-compat`
- `./build.sh kernel` OK；产物 `build/firmware/boot-aov-deep-compat.img`  
  md5 `46ddd28208d28348e567478dde9c9fe8`；已 push 到板上 `/mnt/sdcard/boot-aov-deep-compat.img`
- **待刷** mtd3（需确认）：
  ```bash
  nanddump -f /mnt/sdcard/boot-backup-pre-deep-compat.img /dev/mtd3
  flash_erase /dev/mtd3 0 0 && nandwrite -p /dev/mtd3 /mnt/sdcard/boot-aov-deep-compat.img && sync
  reboot
  ```
  启动后先验：`cat /sys/power/mem_sleep` 应含 `deep`；`strings`/`hexdump` 根 compatible 含 `rockchip,rv1106`；dmesg 有 `rv1106_suspend_init map`
  再跑：`sh /mnt/sdcard/...` 或 `test_aov_fephy_off_mem.sh 3000`

## 2026-07-28 session 40b — 已刷 deep-compat boot
- TF 备份：`boot-backup-pre-deep-compat.img`；刷写 `boot-aov-deep-compat.img` → mtd3 → `FLASH_OK` → reboot
- 板上验证：
  - `compatible` 含 `rockchip,rv1106`
  - `/sys/power/mem_sleep` = `s2idle [deep]`（**deep 已注册且为默认**）
- 下一步：跑 `test_aov_fephy_off_mem.sh 3000`，断电读 TF 是否有 `RESUME_OK`

## 2026-07-28 session 41 — mem resume 首通
- FE-PHY off + `echo mem`（deep）：**RESUME_OK**
  - 13:17:25→28、13:18:43→47、13:21:56→59、13:31:02→05（共 4 次）
  - `suspend_stats/success=4 fail=0`
  - dmesg：`PM: suspend entry (deep)` / `suspend exit`
- 进睡前仍须：gmac wakeup disabled + eth0 down + unbind + 写 `0xff300048`
- 脚本：`test_aov_fephy_off_mem.sh`；soak：`test_aov_fephy_soak10.sh` + `launch_fephy_soak.sh`
- 说明：前台 adb 会在进睡时断 USB 杀掉多轮循环；单次探针可验证，长 soak 需板上 detach
- 下一步：用户态打开 `aov.allow_mem_suspend` 短循环；并行评估 MIS5001 HPMCU 看场

## 2026-07-28 session 42 — detach soak ≥10
- BusyBox `setsid` 无 `-f`；改用 `nohup` 启动（`launch_fephy_soak.sh`）
- **soak12：`done ok=12 fail=0`**；`suspend_stats/success=16`（此前 4 + 本轮 12）
- 末条：`soak10 #12 OK` @ 13:39:45–48；总 `RESUME_OK` 计数 16
- 判据「连续 ≥10」已达成；可开用户态短循环 / 再冲 100 次

## 2026-07-28 session 43 — 用户态 allow_mem_suspend 短循环
- 板上旧 `ipc_app`(7/17) 无 `--aov`；重编部署；`userdata` 曾满（core dump），已清并恢复 `ipc_config.json`
- 配置：`aov.allow_mem_suspend=true`，`suspend_ms=3000`
- 修：`umount -l` 回退；AOV 周期内保持 gmac unbound；`probe_once` 结束 `leave` 再 bind；避免 wake 后 udhcpc 卡死
- **短循环 5/5 PASS**：`done ok=5 fail=0`；`wake_ms≈3070`；`LAPSE_1FPS wake=timer`
- 启动：`nohup sh /mnt/sdcard/run_aov_soak.sh 5 >/tmp/aov_userspace_soak.out 2>&1 &`
- 注意：无 AIQ 时 ISP single-frame 仅 warn（CLI probe 未启 media）

## 2026-07-28 session 44 — 进程内 lapse 循环 `loop`
- 新增 `aov_run_lapse_loop(N)`：`enter` 一次 → N 次 `enter_sleep` → `leave`（周期内 FE-PHY 保持 off）
- CLI：`ipc_app --aov loop 5`；可选 `--media` 写 `/mnt/sdcard/aov_lapse/lapse_*.jpg`
- 板上：`loop 5` → `lapse_loop cycle 5/5 ok wake_ms=3071`；`suspend_stats` 24→29
- `run_aov_soak.sh` 改为调用 `loop`（不再每轮 fork probe）

## 2026-07-30 session 45 — T6.5.3 事件 MP4 闭环 PASS
- 根因：VENC `GetStream` 回调内同步 `rkmuxer_write` 堵住 `ReleaseStream`；空预录清 `waiting_key` 导致非关键帧入 muxer
- 修复：保持 motion arm；空 flush 保留 waiting_key；**独立 mux 写线程 + /tmp staging 再 promote 到 TF**
- 验收：`loop 4 --media` + `debug_motion_interval=4` → `frames=302`，`20260730_102054.mp4` ~2.5MB **可播**
- 下一步：media+motion 长 soak（→ T6.6）；MCU md_flag 仍缺 MIS5001 wakeup FW

## 2026-07-30 session 46 — media+motion soak50 PASS
- `done ok=50 fail=0`；约 12 次 `FULL_RECORD`，每段 `frames≈190–240`，TF 多段 1.5–3.6MB MP4（末段更大）
- 下一步：阶梯加长 soak（200→更长）迈向 T6.6.1 24h；并行可测功耗

## 2026-07-30 session 47 — media+motion soak200 PASS
- `done ok=200 fail=0`；`FULL_RECORD`×50（interval=4）；当日 records 共 64 个 mp4
- 下一步：启动 ~2000 周期 soak（约一夜量级）冲 T6.6.1

## 2026-07-31 session 48 — media+motion soak2000 PASS
- `done ok=2000 fail=0`；`FULL_RECORD`×500；`FAIL|sleep_fail|force stop|stuck` 无匹配
- 变帧率闭环 + 长稳用户态路径成立（debug_motion 模拟 MCU MD）
- 下一步：T6.6.2 功耗对比；可选真 24h；T6.5.2 仍待 MIS5001 wakeup FW

## 2026-07-31 session 49 — T6.6.3 关 AOV 回归 Phase 1
- soak 后新进程 `leave` 不会 rebind（`g_gmac_unbound` 进程内）；修：`eth0` 缺失时 force bind
- 回归：`aov.enabled=false`；`--record 8` → 230f MP4；`--detect 25` → **PASSED**（IVS=4, 470f）
- 残留：T6.6.2 功耗表；T6.5.2 MCU md_flag / MIS5001 FW

## 2026-07-31 session 50 — M7 T7.2 预览工具条 + 抓图
- 前端：重连 / 抓图 / 全屏 / 适应|填充|16:9|4:3
- 后端：`POST /api/v1/preview/snapshot` → snapshots JPEG；下载复用 alarm/snapshot
- 已部署 www + ipc_app `--web`；下一步 T7.5 侦测区域画框

## 2026-07-31 session 51 — M7 T7.5 事件产品化
- 抓图改硬件 VENC JPEG combo（子码流 master）；软 NV12 花屏已弃用
- T7.5.1–5：最多 4 区域软过滤；`schedule.grid` 7×24；`link.record/snapshot`
- UI：`RegionDrawCanvas` + `ScheduleGrid`；告警表 Dialog 看图
- www≈768KB；下一步 T7.6 录像计划

## 2026-07-31 session 52 — M7 T7.6 录像计划
- `GET|POST /api/v1/record`：mode motion|continuous|schedule + grid + 预录/延录/回收%
- continuous/schedule：`policy_hold` 无预览客户端仍保持媒体；schedule 按小时启停连续录
- UI：`RecordPlanView` + 菜单「录像计划」；StorageView 回收水位
- 下一步：T7.8 文档验收收尾 / 可选增强

## 2026-07-31 session 53 — 连续录像分段时长显示偏长
- 现象：`segment_sec=30` 墙钟切段正确，播放器显示 ~50–60s
- 根因：sensor~30fps 全进 mux；rkmuxer 按 `frame_rate=15` 每帧 +1/15s → duration≈frames/15≈60s
- 修复：VENC `Src=30 Dst=cfg`；录像侧按 `main_fps` 软件限帧（rkmuxer 忽略精细 PTS）
- 板上验证：30s 段 `frames=450`，`duration=30.0s`；已部署 `/userdata/ipc_app`

## 2026-07-31 session 54 — M7 T7.8 文档与验收收尾
- `scripts/pack_www.sh`：www **773416 B**（<1.5MB）；gzip JS≈195KB / CSS≈28KB；产物 `index-21JUkFwJ.js`
- 板端：`/userdata/www` 硬刷；`scripts/smoke_m7_web.sh` → 首页+JS+API 1001 门控 PASS（板密已改，登录用参数传入）
- 文档：`03` M7=完成；`06` T7.8 全勾；`ui-vigi-gap` E3/E6/A3/A5 已有
- 已知未做（可选，不挡 M7）：主/子预览切换、数字缩放、日志 tail API、OSD、设备名可写、定时重启
- 下一步：T5.4 README/review；或上述可选增强

## 2026-07-31 session 55 — 六项可选增强 + T5.4
- 预览：主预览 VENC3 H.264（≤720p）+ `?stream=`；数字缩放
- 系统：设备名 POST、日志 `tail`、定时重启 schedule
- OSD：RGN BGRA5551 烧录主/子；`/api/v1/video/osd`
- README / 03 T5.4 / 05-api / 06 todolist 同步；pack+部署

## 2026-07-31 session 56 — 主预览/OSD/侦测/流畅度修复
- **主预览无图**：RV1106 ISP 仅 3 路；改为 VI2→VENC3；无人看 main 时 StopRecv（idle）
- **预览卡顿**：勿双路同时 fMP4；主预览降到 ~10fps/800kbps；前端追直播丢积压
- **连续录像无侦测事件**：continuous/schedule 也起 IVS；`link.record` 仅 motion 策略生效
- **OSD**：中文 FreeType + `assets/fonts/simsun_cn_3000.ttf`；`pos_x/pos_y` **16 对齐**（Y 失效根因）；改位置重建 RGN
- 文档：`01` 视频链路、`05` API、本 worklog；板上验证 MOTION + OSD pos=0/64/160/320

## 2026-08-01 session 58 — Web UI 布局与交互改版
- 登录：空账号/密码校验；中文错误提示；展示设备名（公开 API `/api/v1/system/device_name`）
- 壳层：Top/Left/Main；设备名品牌；预览用 VideoCamera；修改密码 Key 图标；PageShell title/subtitle + 白底内容区
- 实时预览：抽 `LivePreviewPlayer`；满屏；左下 HUD 分辨率/码率/帧率；右侧 icon 工具条；默认箭头光标
- 图像与码流左右分栏；移动侦测 Tab（检测/区域1-4/布防时间/联动）+ 右侧事件；录像计划/存储/网络/基本设置 Tab 化
- 存储列表：日期检索、分页、MB、预览弹框；覆盖比例；录像计划不再显示回收水位
- 后端：登录文案中文；records `offset`；公开 device_name

## 2026-08-01 session 59 — UI  polish + 预览/镜像/长分段
- 预览：空闲拆管线防抖 2s；MSE 裁旧缓冲，缓解切页/播久黑屏
- 镜像/翻转：AIQ `skip=4` + VI0/1/2 `bMirror/bFlip`；GET 以配置为准
- 录像：`segment_sec=300` 时 `/tmp` 写满导致 MP4 无 `moov`；改为 TF `records/.stage/` 暂存 + 延长 mux drain
- UI：Descriptions 强制边框；灵敏度/滑条/时区偏移/事件分页高度等跟进改版

## 2026-08-01 session 60 — 文档同步入库
- README / worklog / API 对齐 Web 改版与长分段 staging；排除官方 AOV PDF

## 2026-08-05 session 61 — 人脸抓拍 F2/F3
- F2：`/api/v1/alarm/face` GET/POST + events/image/DELETE；配置持久化；`api.ts` 客户端；`05-api.md`
- F3：`FaceCaptureView` 三 Tab（侦测 / 区域与布防 / 抓拍记录）；侧栏「事件 → 人脸抓拍」
- 板上：新 `ipc_app` 已推；face 路由未鉴权返回 1001；完整 curl 冒烟需有效 `IPC_PASS`（当前非 admin/admin1）
- 下一步：F4 区域/配额生效与真机调参；浏览器登录验 UI

## 2026-08-05 session 62 — 人脸抓拍 F4
- 区域：`region_allows` 改为 640×360（与 RegionDrawCanvas 一致）；误用 /1000 已修
- 配额：`storage_recycle_faces`；落盘前按 `face.quota_percent` 删最旧 jpg
- Web：日上限 / 存储配额%；POST clamp
- 待现场：启用抓拍 + 画区域验「区外不抓」；走动验灵敏度

## 2026-08-05 session 63 — 预览黑屏修复
- 现象：开人脸后实时预览无画面；IVS getResults timeout；snapshot GetStream fail
- 根因：预览起流后立刻 `face_init` 加载 RKNN 常驻 CMA，挤占 VI/VENC；且人脸从 VI1 GetChnFrame 与子码流争用
- 修复：RKNN 懒加载（首次运动门控才 load）；改从 VI2 取帧（depth=2）；门控需真实 motion notify
- 板上暂保持 `face.enabled=false` 便于验预览

## 2026-08-05 session 64 — 人脸 JPEG 一片红
- 根因：VI GetChnFrame 误信 `pVirAddr[1]`，UV 指针错误 → 色度垃圾呈暗红块
- 修复：一律 `MbBlk`+`MmzFlushCache`，UV=`base + VirWidth*VirHeight`

## 2026-08-05 session 65 — 人脸 JPEG 仍偏红（hor_stride + vir_height）
- 根因①：NV12 行步长须 `GetHorStride(VirWidth)`，不能当 `width`
- 根因②：Rockchip NV12 UV 在 `hor_stride × vir_height`，不是 `hor_stride × height`；拷贝到工作缓冲时误用 `fh` 导致右半 UV 错位偏红
- 修复：全程用 `vir_height` 定位/拷贝 UV；`MediaNv12Frame` 增加 `vir_width/vir_height`

## 2026-08-05 session 66 — 人脸 JPEG 仍红（编译错误 + 落盘路径）
- 中间误改引用了 SDK 不存在的 `u32Stride/u32Offset`，本地已编不过；板子仍跑旧二进制
- 修复：`pVirAddr[1]` 仅在 offset∈{hor×h, hor×vh} 时采用，否则 `y+hor×vh`
- 抓帧后压成 stride=width 紧凑 NV12；人脸裁剪改 NV12→RGB→`jpeg_write_rgb`（绕开裁剪 NV12 编码）
- 首次推理前落盘 `/mnt/sdcard/faces/_dbg_y.jpg` + `_dbg_nv12.jpg` 便于对照

## 2026-08-05 session 67 — 诊断：非 UV，是 cache
- 同刻场景 HW JPEG 正常；`_dbg_y`/`_dbg_nv12` 全是花屏 → VI GetChnFrame 软件读到的就是垃圾
- UV 偏移已正确（`uv-y = hor×vh = 921600`，src=hor*vh）
- 根因：`MmzFlushCache(..., RK_TRUE)` 方向错；设备写后 CPU 读须 `RK_FALSE`（invalidate）
- 人脸 score≈0.55 疑似花屏误检；修 cache 后应同时改善检测与落盘

## 2026-08-05 session 68 — cache 仍无效，改 RGA 导出
- 17:16 复测：场景 JPEG 正常，人脸/`_dbg_*` 仍花屏；`MmzFlushCache(RK_FALSE)` 无效
- 根因：RV1106 VI DMABUF 的 `Handle2VirAddr` 对 CPU 不可靠
- 修复：`media_grab_*_nv12` 经 RGA `imcopy(fd→virtualaddr)` 拷成 CPU 紧凑 NV12 后再推理/落盘

## 2026-08-05 session 69 — RGA/malloc 打挂整机，先保 Web
- 现象：上电后一会儿 Web 挂死，ADB 也丢；人脸开着会周期性 RGA
- 根因：`imcopy` 目标用了普通 `malloc`，RGA DMA 写非 MMZ → 系统级 hang
- 处置：暂时禁用 CPU NV12 export（grab 直接失败）；预览/录像/硬件 JPEG 不受影响
- 人脸抓拍暂不可用，待 MmzAlloc 安全路径验证后再开

## 2026-08-06 session 70 — 人脸抓拍：RGA + MmzAlloc
- `media_export_vi_nv12`：VI fd → RGA imcopy → `MmzAlloc_Cached` 目标；release 时 `MmzFree`
- 加 `g_rga_lock`；拷贝后 `MmzFlushCache(RK_FALSE)` 再给 CPU/RKNN
- 已重新开启 `face.enabled` 部署验证

## 2026-08-06 session 71 — VI2+RGA 仍花屏，改 VI1+importbuffer
- 09:53：`rga+mmz ok` 但 `_dbg_y` 仍花屏；同刻场景 HW JPEG 正常
- 改：`importbuffer_fd/physicaladdr` + `wrapbuffer_handle`；人脸改抓 **VI1**（与预览/JPEG 同路）；VI1 depth=3

## 2026-08-06 session 72 — 放弃 VI NV12，改 HW JPEG 全链路
- VI1 RGA 仍花屏（ymean~247）；判定 GetChnFrame 软件可读 NV12 不可用
- 人脸：`media_snapshot_jpeg_mem` → NanoJPEG 解码 RGB → YuNet → `jpeg_write_rgb` 裁剪
- 场景图复用同一帧 JPEG 落盘；不再调用 VI grab/RGA

## 2026-08-06 session 73 — 红脸真因：自研 `jpeg_write_rgb` 色偏
- `_dbg_scene.jpg` / 场景 HW JPEG 正常；裁剪框区域 RGB 正常；NanoJPEG 解码正常
- 坏图 `20260806_101841_390_1.jpg` 几乎纯红（mean≈70,0,1）→ 编码坏
- 主机复现：源 (92,96,107) → 旧编码器出 (0,5,35)
- 修复：`jpeg_write_rgb` 改用 `stb_image_write`；板测 `--face 45` snaps=25，新图 center≈(148,144,143)
- 部署注意：`/userdata` 空间紧，先清 log 再 `cp`，避免截断二进制

## 2026-08-06 session 74 — 人脸裁剪只有 1/4（框偏嘴鼻）
- 场景标注：检测框落在嘴/鼻，不是整脸；颜色链路已正常
- 根因：YuNet 框回归偏紧 + 旧 distance 解码；裁剪未上扩
- 尝试：OpenCV 解码 / 关键点 / 上扩 / 恒 sigmoid / 28px 过滤 → 板上几乎抓不到脸（更差）
- **回退**：恢复 distance 解码（去掉 OpenCV/关键点/28px/恒 sigmoid）；保留 stb 颜色修复
- 裁剪仅小改：中心上移 0.45×h、宽×1.25 / 高×1.8（不改检测）
- Web 已 nohup 拉起；请打开预览对着镜头复测新抓拍

## 2026-08-06 session 75 — 有推理无抓拍
- 现象：UI infer 增加但 `faces/` 无新图；日志多见 `boxes=0`；旧抓拍目录曾被清空
- 原因①：cls/obj 未做 sigmoid（logits 落在 0~1 被当成概率）→ 真脸分不够
- 原因②：letterbox 顶边细条误检映射成 h≈0，裁剪 `ch<8` 静默失败
- 修复：恒 sigmoid + 模型侧 min 16px；跳过 pad 区框；正方形裁剪；confirm=1；crop 失败打日志
- 已部署，Web 200；请开预览对人脸验证


