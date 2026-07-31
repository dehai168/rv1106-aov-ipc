# M6 变帧率 AOV Todolist

> 设计：`docs/design/aov.md`  
> 原则：先打通 resume 地基 → 1fps lapse → 事件全帧率 → 长稳。  
> 状态：`[ ]` 待办 `[~]` 进行中 `[x]` 完成 `[!]` 阻塞  

---

## 里程碑目标

变帧率布防：静态 **1fps 深睡** ↔ 事件 **20~30fps 录像**；关网/关 Web；常电路径不受损。

总验收：

1. `aov.enabled=false` ≡ Phase 1；  
2. 静态 ≈1fps sleep/wake 循环（≥1000 次或 24h）；  
3. 人为运动 → 全帧率录像；安静后回 1fps；  
4. 功耗相对常电可重复下降；  
5. 文档与 API 同步。

---

## T6.1 调研与设计

- [x] T6.1.1–T6.1.5 初稿 + D1–D6 确认
- [x] T6.1.6 **纠偏**变帧率产品语义（2026-07-28）：`aov.md` / `01-architecture` / 本文件

---

## T6.2 / T6.3 板级与固件

- [x] T6.2 探针 → 现固件 No-Go → T6.3
- [x] T6.3.1–3 / 5：SUSPEND + GRF + boot/ko；`aov-reserved`；WAKEUP.ini
- [!] T6.3.4 mem resume — **已通**（deep-compat + FE-PHY；见 worklog session 40–43）
- [x] T6.3.6 阻塞分析 + PMU 快照 + GPIO 探针脚本 + MIS5001 FW 缺口结论

**验收（未过）**：100 次定时唤醒成功率 ≥ 95%。

---

## T6.4 用户态框架（可与 resume 并行预埋）

- [x] T6.4.1 `aov_service` 骨架 + `aov.*` 配置
- [x] T6.4.2 `storage_prepare_suspend` / `storage_resume_from_suspend`
- [x] T6.4.3 编排钩子（media 单/多帧、record 收段、eth down）
- [x] T6.4.4 CLI：`--aov-probe` / `--aov status|enter|leave`
- [x] T6.4.5 REST：`/api/v1/aov/config|status|enter|leave`
- [ ] T6.4.6 Web UI 开关（可后置）

---

## T6.5 变帧率闭环（依赖 resume）

- [x] T6.5.0 代码路径：timer→lapse / md→full_record 状态机（`allow_mem_suspend=0` dry-run 可测）
- [x] T6.5.1 板上 lapse 循环：`--media` 50/50 PASS（`wake_ms≈3.1s`，lapse JPG 落盘）
- [~] T6.5.2 MCU md_flag → MultiFrame 20~30fps（需 MIS5001 wakeup FW）；`debug_motion_interval` 可模拟动检唤醒
- [x] T6.5.3 安静/event_record_sec 后回 1fps：动检 → FULL_RECORD → **302 frames / ~2.5MB 可播 MP4** → quiet 收段 → LEAVE（2026-07-30；写线程+tmp staging）；**media+motion soak50：`done ok=50 fail=0`，12 段非空 MP4（190–240 frames）**
- [x] T6.5.4 媒体失败 → 周期/media reinit：50 周期 `reinit=24` 全部恢复继续（periodic + snapshot_fail）
- [ ] T6.5.5 布防 schedule/region 在 AOV 下生效

---

## T6.6 长稳与功耗（依赖 T6.5）

- [x] T6.6.0 soak 脚本与验收清单预埋（`scripts/run_aov_soak.sh` / `aov-phase4-acceptance.md`）
- [x] T6.6.1 长 soak：media+motion **soak2000 `done ok=2000 fail=0`**，`FULL_RECORD`×500，无 FAIL/stuck（2026-07-31）；24h 墙钟可再加一轮
- [ ] T6.6.2 功耗对比表
- [x] T6.6.3 关 AOV 回归 Phase 1：`enabled=false` + gmac rebind；`--record 8` frames=230；`--detect 25` **PASSED** IVS motion=4 frames=470（2026-07-31）
- [x] T6.6.4 architecture §4.5 已改为变帧率描述

---

## 建议顺序

```
文档纠偏 ✅ + 用户态骨架 ✅
  → T6.3.4/6 resume 根因（P0，仍阻塞）
      → 1fps lapse 上板
          → 事件全帧率 + MIS5001 MCU FW
              → 24h + 功耗
```

并行：NTP/日志轮转、M7 UI、T6.4.6 Web 开关。
