# AOV mem resume 阻塞分析（Phase 1 / T6.3.6）

> 更新：2026-07-28（指南 PDF + FE-PHY 复测 + deep 根因）  
> 目标：变帧率 AOV 地基 — 稳定 `RESUME_OK`

## 0. 根因（已修复，2026-07-28）

**症状**：`/sys/power/mem_sleep` 仅 `[s2idle]`；`echo mem` 未进 Rockchip deep，PMU timeout 无效。

**原因**：板级缺 `"rockchip,rv1106"` → `DT_MACHINE` 不匹配 → `rockchip_suspend_init()` 未跑。

**修复**：DTS 追加 `"rockchip,rv1106"` → 刷 `boot-aov-deep-compat.img` → `mem_sleep = s2idle [deep]`。

**验证（FE-PHY off + mem 3s）**：

| 时间 | 结果 |
|---|---|
| 13:17:25–28 | `RESUME_OK`（首通） |
| 13:18:43–47 | `RESUME_OK` |
| 13:21:56–59 | `RESUME_OK` |
| 13:31:02–05 | `RESUME_OK` |

`suspend_stats/success=4`，`fail=0`。dmesg：`PM: suspend entry (deep)` → `PM: suspend exit`。

## 1. 已排除

| 假设 | 结果 |
|---|---|
| 仅缺 `CONFIG_SUSPEND` | 已开；仍 FAIL（且未注册 deep ops） |
| 仅缺 `RV1106_PMU_WAKEUP_TIMEOUT` | 已开；仍 FAIL（ops 未挂上时无效） |
| 仅缺 `aov-reserved` | 已加；仍 FAIL |
| 仅缺 Trust `RV1106TOS_WAKEUP.ini` | 已刷；仍 FAIL |
| `hpmcu_wrap` 冲突 | 无 rockit 启动仍 FAIL |
| `CAM_SLEEP_WAKEUP` | 已开（MIS5001 未用）；仍 FAIL |
| USB 假死 | TF 仅 `before_mem*`，真未 resume |
| **仅关 FE-PHY** | 2026-07-28 `before_mem_fephy_off`；无 `RESUME_OK` — 必要非充分 |

## 2. 板上寄存器快照（2026-07-28）

```
0xff300000 (PMU_WAKEUP_CFG?): 0x030a0000
0xff300004:                    0x0000004e
0xff300048 (timeout cnt):      0x00005dc0   # 例：3000ms*32=96000=0x17700；此值视上次写入
0xff30004c:                    0x000fffff
rtc0: rockchip-rtc ff1c0000.rtc（有 wakeup 属性）
CONFIG_RV1106_PMU_WAKEUP_TIMEOUT=y
CONFIG_ROCKCHIP_SUSPEND_MODE=y
```

debugfs clk 未挂载，无法直接读 `clk_rtc_32k` enable_count；历史记录曾见 enable_count=0。

## 3. MIS5001 wakeup 固件可行性

SDK MCU 板级（`sysdrv/source/mcu/rt-thread/bsp/rockchip/rv1106-mcu/board/`）：

- **有** wakeup：`rv1106_evb2_wakeup-SC200AI-ADC`、`rv1106_evb2_wakeup-SC3338-ADC`
- **无** `*MIS5001*` / `*mis5001*` 板级或 `hpmcu_*_mis5001*` bin

预置 `rv1106_hpmcu_wakeup_v1.00.bin` 字符串/配套面向 SC 系 sensor。  
**结论**：Pico Max + MIS5001 要跑官方「睡时 MCU 看场 + FastAeMd」，需从 `battery-ipc` **移植/新建 MIS5001 wakeup 板级并重编 MCU FW**，不能指望现成 bin 完美适配。

MCU 共享结构（`fast_ae.h`）：`shared_data.md_flag`（0=无运动，1=运动）；`stream.c` 在 md/GPIO 时发 `CMD_AP_RESUME`。

## 4. 下一步（resume 已通之后）

1. ✅ deep + FE-PHY + PMU timeout → `RESUME_OK`
2. ✅ 板上 detach soak：**12/12 成功**（`aov_soak10_result.txt`：`done ok=12 fail=0`；累计 `success=16`）
3. ✅ 用户态 `aov.allow_mem_suspend=true` 短循环 **5/5 PASS**（`ipc_app --aov probe`）
4. 可选：进程内连续循环 / 启 media 单帧；100 次 soak；MIS5001 HPMCU 看场

## 5. 成功判据

- TF：`RESUME_OK` 连续 ≥10 次  
- 再跑 100 次成功率 ≥95%

在此之前：用户态骨架可预埋；**不上板强制深睡长循环**。

## 6. FE-PHY 关断（2026-07-28 贴士落地）

内核路径：`dwmac-rk.c` → `rk_gmac_suspend`/`rk_gmac_remove` → `rk_gmac_powerdown` → `rv1106_integrated_sphy_power(false)` → `RK_FEPHY_SHUTDOWN`（VOGRF `MACPHY_CON0`）。

若 gmac 仍绑定且作为 wakeup 源，suspend 时 **不会** powerdown FE-PHY（`device_may_wakeup` 分支）。

本仓库 `network_prepare_aov_suspend()`：

1. `wakeup=disabled`  
2. `ip link set eth0 down`  
3. `echo ffa80000.ethernet > .../rk_gmac-dwmac/unbind`

复测 mem 探针前应用此路径（`aov.down_eth=true`，默认开）。
