# AOV 板级探针记录（T6.2）

> 板型：Luckfox Pico Max（`Luckfox Pico Max`，内核 `5.10.160`）  
> 日期：2026-07-27  
> 决策基线：D1–D6 已确认（见 `docs/design/aov.md` §9）

---

## 1. 探针环境

| 项 | 值 |
|---|---|
| 内核 | `Linux luckfox 5.10.160 #1 Thu Mar 20 17:03:23 CST 2025 armv7l` |
| 板级配置（现场） | `BoardConfig-SPI_NAND-Buildroot-RV1106_Luckfox_Pico_Pro_Max-IPC.mk` |
| `RK_KERNEL_DEFCONFIG` | `luckfox_rv1106_linux_defconfig` |
| 调试通道 | Type‑C adb（D3：AOV 时 eth0 可不保持） |
| 应用 | `/userdata/ipc_app --web` PID 3345，探针后仍存活 |

---

## 2. 结果摘要

| 检查项 | 结果 | 含义 |
|---|---|---|
| `cat /sys/power/state` | **空** | 无可进入的休眠态字符串 |
| `echo mem > /sys/power/state` | **`write error: Function not implemented`** | 当前运行内核 **未实现 suspend(mem)** |
| `/usr/bin/io`、`/sbin/devmem` | **存在** | 用户态写唤醒寄存器的工具已具备 |
| RTC `rtc0` | 存在，可校时 | 仅作校时；不作 MVP 唤醒源 |
| SDK `luckfox_rv1106_linux_defconfig` | `# CONFIG_SUSPEND is not set` | 与板上行为一致 |
| SDK `rv1106-pm.config` | `CONFIG_SUSPEND=y` 等 | **需要合并进板级 fragment 后重编内核** |

### 判定

**当前固件：No-Go（无法用户态完成 AOV 休眠循环）。**  
下一步必须进入 **T6.3 固件使能**（D1 已允许）。

---

## 3. 已确认决策（落地配置默认值）

| ID | 确认值 | 实现时默认 |
|---|---|---|
| D1 | 允许改内核/DTS/刷固件 | 开 T6.3 |
| D2 | 1000ms | `aov.suspend_ms=1000` |
| D3 | 网口可不保持 | 进 AOV 可 `ip link set eth0 down`；调试靠 adb |
| D4 | 与 Web 预览互斥 | enter AOV 前停预览 WS |
| D5 | 媒体失败重启进程 | watchdog / 自拉起 |
| D6 | 无 PIR | 不实现 GPIO PIR 唤醒 |

---

## 4. T6.3 建议最小改动（固件）

### 4.1 内核 fragment（必做）

在 SDK 板级文件：

`project/cfg/BoardConfig_IPC/BoardConfig-SPI_NAND-Buildroot-RV1106_Luckfox_Pico_Pro_Max-IPC.mk`

增加（当前该文件 **没有** `RK_KERNEL_DEFCONFIG_FRAGMENT`）：

```bash
export RK_KERNEL_DEFCONFIG_FRAGMENT="rv1106-pm.config"
```

对应 fragment 内容（已存在于 SDK）：

```
CONFIG_PM_DEBUG=y
CONFIG_PM_SLEEP=y
CONFIG_PM_SLEEP_DEBUG=y
CONFIG_PM_WAKELOCKS=y
CONFIG_SUSPEND=y
CONFIG_ROCKCHIP_SUSPEND_MODE=y
CONFIG_ROCKCHIP_GRF=y
```

期望刷机后：

```sh
cat /sys/power/state
# 至少包含：mem
```

### 4.2 设备树 / 唤醒（第二步）

仅打开 `CONFIG_SUSPEND` **不等于** AOV 定时唤醒一定可用。还需验证：

1. `io -4 0xff300048 $((1000*32))` + `echo mem > /sys/power/state` 能否自动醒；
2. 若不能，对照 EVB `rv1106g-evb2-v12-aov-spi-nor.dts` / HPMCU wakeup 固件，按 **Pico Max 引脚**裁剪补丁（禁止整份套用 EVB DTS）。

### 4.3 刷写与回滚（已验证）

**必做：boot + 匹配 KO 一起换**（仅刷 `boot.img` 会导致 CSI `Can't find GRF syscon`）。

```bash
# WSL
cd /home/user/work/luckfox-pico
./build.sh kernel && make -C sysdrv drv

# 板上：先 push 到 TF 校验 md5，再擦写 mtd3；并同步媒体 .ko 到 /oem/usr/ko
adb shell 'flash_erase /dev/mtd3 0 0 && nandwrite -p /dev/mtd3 /mnt/sdcard/boot-aov-pm.img && sync'
# 复位：物理断电，或 io -4 -w 0xff3b0c08 0xfdb9
```

回滚：`build/firmware/boot-stock-no-suspend.img` + TF `oem-ko-backup-pre-aov/`。

---

## 5. 探针过程备注

- 曾执行一次失败的 `echo mem`；`ipc_app` 随后仍在。
- 正式测休眠前建议先停预览/侦测。
- 刷入 SUSPEND 内核后 `/proc/config.gz` 可用。

---

## 6. T6.3 上板结果（2026-07-27）

| 项 | 结果 |
|---|---|
| 内核 | `#1 Mon Jul 27 13:25:38 CST 2026` |
| `/sys/power/state` | **`freeze mem`** |
| CSI / `/dev/video0` | **OK** |
| `ipc_app --web` | 存活 |
| 定时唤醒循环 | **FAIL×3**：TF 探针仅 `before_mem`，无 `RESUME_OK` → **真未 resume**（非 USB 假象） |

---

## 7. 下一步

1. T6.3.4 / T6.2.4：`io -4 0xff300048 $((ms*32))` + `echo mem`（adb 可能掉线）
2. 通过后 T6.4 `aov_service`

---

## 8. 修订记录

| 日期 | 说明 |
|---|---|
| 2026-07-27 | 初稿：确认当前内核无 SUSPEND；Go 路径=合并 `rv1106-pm.config` 后重编 |
| 2026-07-27 | T6.3 上板：`freeze mem` + CSI OK；记录 KO 同步与回滚 |
