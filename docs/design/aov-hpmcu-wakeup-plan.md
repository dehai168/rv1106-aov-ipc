# AOV HPMCU Wakeup 上板计划（T6.3.4 后续）

> 基于 TF 探针 2026-07-27：`mem` / `freeze` 均只写到 `before_*`，无 `RESUME_OK`。

## 1. 日志结论

| 探针 | 日志片段 | 结论 |
|---|---|---|
| `mem` 3s | `before_mem up=64` @ 16:28:56 | 进入 suspend 后 **未 resume** |
| `freeze` | `before_freeze up=72` @ 16:47:31 | **同样未 resume** |
| `suspend_stats` | success/fail 均为 0 | 内核未统计到一次完整 suspend 成功 |

**不是**「醒了但 USB/adb 没回来」——脚本在 `echo */sys/power/state` 之后从未继续写日志。

## 2. 当前固件 vs AOV 参考

| 项 | Luckfox Pico Max（现网） | Rockchip AOV 参考 |
|---|---|---|
| Trust MCU | `RV1106TOS.ini`：`hpmcu_wrap@0xff6fe000` **disabled** | `RV1106TOS_WAKEUP.ini`：`hpmcu_wakeup@0x40000` **okay** |
| 运行时 rockit | `insmod rockit.ko mcu_fw_path=./hpmcu_wrap.bin mcu_fw_addr=0xff6fe000` | wakeup 固件在 boot 阶段载入 MCU SRAM |
| 内核 fragment | `rv1106-pm.config` + `RV1106_PMU_WAKEUP_TIMEOUT` | EVB 另有 `rv1106-wakeup.config`、`aov-reserved` DTS |
| DTS | `rkcif_mipi_lvds` **无** `aov-reserved` | EVB AOV DTS 有 `aov-reserved` |

SDK 文件：
- `sysdrv/source/uboot/rkbin/bin/rv11/rv1106_hpmcu_wakeup_v1.00.bin`（109KB）
- `sysdrv/source/uboot/rkbin/RKTRUST/RV1106TOS_WAKEUP.ini`

## 3. 上板改动清单（最小顺序）

### 3.1 内核 DTS（低风险，随 boot.img）— **已完成 2026-07-27**

在 `rv1106-luckfox-pico-pro-max-ipc.dtsi` 为 `&rkcif_mipi_lvds` 增加 `aov-reserved;`

- SDK 路径：`sysdrv/source/kernel/arch/arm/boot/dts/rv1106-luckfox-pico-pro-max-ipc.dtsi`
- 备份：`rv1106-luckfox-pico-pro-max-ipc.dtsi.bak-before-aov-reserved`
- 产物：`boot-aov-pm.img`（md5 `46f4e8f01a6ccc5be7eb6b98046a768a`）
- 板上验证：`/proc/device-tree/rkcif-mipi-lvds/aov-reserved` **存在**；CSI/`/dev/video0` OK
- TF 备份：`/mnt/sdcard/boot-backup-pre-aov-reserved.img`

**resume 探针仍未跑**（避免再次卡死）；预计仍需 §3.3 WAKEUP.ini。

### 3.2 内核 config（可选，随 boot.img）

合并 `rv1106-wakeup.config` 中与 MIS5001/AOV 相关项（勿整包替换 EVB DTS）：
- `CONFIG_RV1106_PMU_WAKEUP_TIMEOUT=y`（已有）
- `CONFIG_VIDEO_CAM_SLEEP_WAKEUP=y` 等

### 3.3 U-Boot Trust / idblock（高风险）— **已刷 2026-07-27**

Luckfox `luckfox_rv1106_uboot_defconfig` 当前 `CONFIG_TRUST_INI=""`（等价默认 `RV1106TOS.ini`，wrap **disabled**）。

参考 `rv1106-spi-nor-wakeup.config`：

```
CONFIG_TRUST_INI="RV1106TOS_WAKEUP.ini"
```

**刷前必做（TF 上）：**

```bash
nanddump -f /mnt/sdcard/idblock-backup-pre-wakeup.img /dev/mtd1
nanddump -f /mnt/sdcard/uboot-backup-pre-wakeup.img /dev/mtd2
```

步骤（已完成 1–3，4 待断电读 TF 日志）：
1. ✅ SDK `luckfox_rv1106-wakeup.config` + BoardConfig fragment
2. ✅ `./build.sh uboot` → `idblock-wakeup.img` / `uboot-wakeup.img`
3. ✅ TF 备份 + 刷 mtd1、mtd2；复位后启动正常
4. ⏳ `test_aov_resume_probe.sh mem 3000` — adb 断，待读 `aov_resume_probe.log`

### 3.4 运行时 rockit（待 3.3 验证后再动）

现 `hpmcu_wrap.bin` 与 wakeup 固件用途不同；**不要**简单把 wrap 路径改成 wakeup 文件而不改地址。

## 4. 验证脚本

- 板上：`scripts/test_aov_resume_probe.sh mem 3000`
- 主机：断电后读 `/mnt/sdcard/aov_resume_probe.log` 应有 `RESUME_OK`

## 5. 回滚

- boot：`boot-stock-no-suspend.img` / `boot-aov-pm.img`（无 wakeup trust 时）
- idblock/uboot：TF 备份 `idblock-backup-*.img`、`uboot-backup-*.img`（刷前必做）
