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
