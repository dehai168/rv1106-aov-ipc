# AOV Phase 4 验收清单

> 前置：Phase 1 `RESUME_OK` 稳定；`aov.allow_mem_suspend=true`

## 功能

- [ ] 静态：约 1fps lapse 循环，关 eth / 关 Web 预览
- [ ] 动态：运动 → 20~30fps 录像
- [ ] 安静 / `event_record_sec` 后回 1fps
- [x] `aov.enabled=false` 后 Phase 1 预览/侦测/回放正常

## 长稳

- [ ] `scripts/run_aov_soak.sh 1000` 或 24h wall time
- [ ] 无系统性死机；媒体失败可进程自愈（D5）

## 功耗（填 worklog）

| 场景 | 电流/功率 | 备注 |
|---|---|---|
| 常电空闲（Web 在、无预览） |  | 基线 |
| AOV 静态 1fps |  | 10min 平均 |
| AOV 全性能录像 |  |  |

## 当前状态（2026-07-28）

**地基已通**：mem resume + `allow_mem_suspend` + 用户态短循环。  
**进行中**：`ipc_app --aov loop N` 进程内连续 lapse；`--media` 写 `/mnt/sdcard/aov_lapse/*.jpg`。  
**仍阻塞**：运动唤醒全帧率录像（MIS5001 HPMCU FW）、24h soak、功耗表。
