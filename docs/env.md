# 开发环境实况（T0.1）

> 随环境变化更新本文件。路径以 WSL 用户 `user` 为准。

## 主机环境

| 项 | 值 |
|---|---|
| Windows 工作区 | `D:\CodeProject\rv1106-aov-ipc` |
| WSL | Ubuntu 24.04.1 LTS（WSL2），用户 `user` / 密码 `12345678` |
| WSL 进入 | `wsl -u user` |
| WSL 本仓库 | `/home/user/work/rv1106-aov-ipc`（git clone 自 GitHub） |
| 官方 SDK | `/home/user/work/luckfox-pico`（`LuckfoxTECH/luckfox-pico`，shallow clone） |
| SDK commit | `824b817f8`（记录于 2026-07-16） |
| 板型配置 | `BoardConfig-SPI_NAND-Buildroot-RV1106_Luckfox_Pico_Pro_Max-IPC.mk` |
| 说明 | 仓库内板名是 `Pro_Max`，对应硬件 Luckfox Pico Max（SPI NAND + Buildroot） |

## 交叉编译工具链

| 项 | 值 |
|---|---|
| 路径 | `/home/user/work/luckfox-pico/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf` |
| 编译器 | `arm-rockchip830-linux-uclibcgnueabihf-gcc` |
| 版本 | gcc 8.3.0（crosstool-NG 1.24.0） |
| C 库 | uClibc（与板上 Buildroot 一致） |
| PATH | 已写入 `~/.bashrc` 与 `~/.bash_profile` |

常用：

```bash
export PATH=/home/user/work/luckfox-pico/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin:$PATH
arm-rockchip830-linux-uclibcgnueabihf-gcc --version
```

## 开发板（adb）

| 项 | 值 |
|---|---|
| 连接 | `adb devices` → `25a044a7af84927d` |
| 内核 | Linux luckfox 5.10.160 armv7l |
| rootfs | Buildroot 2023.02.6 |
| 可用内存 | 约 181MB（与 README 256MB DDR 一致，部分留给内核/CMA） |
| TF 卡 | 已挂载 `/mnt/sdcard`，exFAT/可用约 59.4G |
| 预装应用 | `/oem/usr/bin/rkipc`（默认后台运行） |
| Sensor | MIS5001（`m00_b_mis5001`，VI 例程日志已确认） |
| 媒体库 | `/oem/usr/lib/librockit.so` 等 |

## 验证过的编译命令

### 1) 最小 hello（验收工具链）

```bash
# WSL
export PATH=/home/user/work/luckfox-pico/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin:$PATH
arm-rockchip830-linux-uclibcgnueabihf-gcc -Wall -O2 hello.c -o hello_rv1106
# Windows / adb
adb push build/hello_rv1106 /userdata/hello_rv1106
adb shell "chmod +x /userdata/hello_rv1106; /userdata/hello_rv1106"
# 期望输出：rv1106-aov-ipc T0.1 hello from cross-compile
```

### 2) 官方 RKMPI media + samples

```bash
cd /home/user/work/luckfox-pico
ln -sf project/cfg/BoardConfig_IPC/BoardConfig-SPI_NAND-Buildroot-RV1106_Luckfox_Pico_Pro_Max-IPC.mk .BoardConfig.mk
export PATH=/home/user/work/luckfox-pico/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
./build.sh media
# 产物：
#   output/out/media_out/lib/librockit.so
#   output/out/media_out/bin/simple_vi_get_frame
#   output/out/media_out/bin/simple_vi_bind_venc
```

板上短测（会占用摄像头，需先停 rkipc）：

```bash
adb push build/simple_vi_get_frame /userdata/simple_vi_get_frame
adb shell "killall rkipc; export LD_LIBRARY_PATH=/oem/usr/lib; /userdata/simple_vi_get_frame -I 0 -w 640 -h 480 -c 5 -o 0"
# 日志应出现 sensor name = m00_b_mis5001
# 测完后恢复：
adb shell "/oem/usr/bin/rkipc -a /oem/usr/share/iqfiles >/dev/null 2>&1 &"
```

> 注：无 AIQ 的 `simple_vi_get_frame` 可能打印 ISP stream 错误但仍能加载 rockit/识别 MIS5001；完整出图用 `simple_vi_get_frame_rkaiq`（T1.1）。

## 依赖包（WSL 已装）

```bash
sudo apt-get install -y git ssh make gcc gcc-multilib g++-multilib module-assistant expect \
  g++ gawk texinfo libssl-dev bison flex fakeroot cmake unzip gperf autoconf \
  device-tree-compiler libncurses5-dev pkg-config bc python-is-python3 \
  passwd openssl openssh-server openssh-client vim file cpio rsync wget curl
```

## 仓库辅助脚本

| 脚本 | 作用 |
|---|---|
| `scripts/setup_t01.sh` | 校验工具链并编 hello |
| `scripts/build_media_sample.sh` | lunch Pro_Max + `./build.sh media` |
| `scripts/deploy_sample.sh` | 把 `simple_vi_get_frame` 拷到 `build/` |

## 已知注意点

1. WSL 从 PowerShell 传命令时 `$VAR` 易被展开，复杂操作写成 `scripts/*.sh` 再 `wsl -u user -- bash /mnt/d/.../scripts/xxx.sh`。
2. `./build.sh` 对 PATH 中空格敏感，交叉编译前用干净 PATH（见上）。
3. 板上默认 `rkipc` 占用 VI；跑自研/例程前需 `killall rkipc`，测完恢复。
4. 完整固件烧录属高危操作，T0.1 未做，仅应用层 adb push。
