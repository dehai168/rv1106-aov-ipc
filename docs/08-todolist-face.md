# M8 人脸抓拍细项（F0–F4）

> 方案：`docs/design/face-capture.md`（已定稿）。本文件跟踪执行与冒烟数据。

## F0 RKNN + YuNet 320 上板冒烟 `[x]`

### 检查清单

- [x] 板上 `rknpu` 模块已加载
- [x] 板上 `/oem/usr/lib/librknnmrt.so` 存在
- [x] SDK 头文件 vendored：`third_party/rknn/rknn_api.h`
- [x] 冒烟程序：`tools/face_rknn_smoke.c` → `face_rknn_smoke`（零拷贝 IO）
- [x] YuNet 320 INT8 `.rknn` 转换并部署
- [x] 板上连续推理 PASS；记录耗时

### 冒烟记录

| 日期 | 项 | 结果 |
|---|---|---|
| 2026-08-05 | `lsmod rknpu` | OK |
| 2026-08-05 | `librknnmrt.so` | `/oem/usr/lib/librknnmrt.so` |
| 2026-08-05 | SDK BlazeFace `detection.rknn`（通路验证） | PASS 50× avg **3.01 ms** |
| 2026-08-05 | YuNet 320 转换 | `models/face/yunet_320.rknn` **317.7 KB**（toolkit2 2.3.2） |
| 2026-08-05 | YuNet 板上推理 | PASS 50× avg **7.51 ms**（min 7.49 / max 7.78）；load 9.5 ms |
| 2026-08-05 | IO | input `1×320×320×3` UINT8 NHWC；**12** outputs（cls/obj/bbox/kps × 8/16/32） |
| 2026-08-05 | 部署路径 | `/mnt/sdcard/models/yunet_320.rknn`（userdata 空间不足） |

### 复现

```bash
# 交叉编译
./scripts/build.sh   # 产出 build/face_rknn_smoke

# 推模型与冒烟
adb push models/face/yunet_320.rknn /mnt/sdcard/models/yunet_320.rknn
adb push build/face_rknn_smoke /userdata/face_rknn_smoke
adb shell "export LD_LIBRARY_PATH=/oem/usr/lib; /userdata/face_rknn_smoke /mnt/sdcard/models/yunet_320.rknn 50"
```

转换环境：WSL `/home/user/work/yunet_conv`（venv + rknn-toolkit2），见 `models/face/README.md`。

## F1 `face_service` `[x]`

- [x] `src/face/rknn_face.c` — YuNet 零拷贝 IO + 反量化 + decode/NMS
- [x] `src/face/face_service.c` — motion_gate（hold_ms）+ interval + 去重冷却 + 落盘
- [x] `media_grab_sub_nv12`（VI1 depth=2）
- [x] 全景引用 `snapshots/`；告警 `type=face`；`link_record` 默认开
- [x] CLI `--face N`；Web 在 `face.enabled` 时随 detect 拉起
- [x] 板上冒烟：`PIPELINE_OK infers=28 grab_fail=0`（无人脸场景 snaps=0 属预期）

模型路径：`/mnt/sdcard/models/yunet_320.rknn`

## F2 API `[x]`

- `GET/POST /api/v1/alarm/face`
- `GET .../face/events`、`.../face/image`、`DELETE .../face/events`
- 配置 `face.*`；`scripts/smoke_face_api.sh`（需有效 `IPC_PASS`）

## F3 Web `[x]`

- 侧栏：事件 → 人脸抓拍（`/settings/event/face`）
- Tab：侦测 / 区域与布防 / 抓拍记录（`FaceCaptureView`）

## F4 调参 `[x]`（代码侧）

- regions：与 UI 一致使用 **640×360** 逻辑坐标（中心点落区内才抓拍）
- schedule：非布防时段跳过推理
- 配额：`storage_recycle_faces(quota_percent)`，超限删最旧 `faces/*.jpg`；日上限已生效
- Web：侦测 Tab 露出日上限 / 存储配额%
- 真机灵敏度：建议室内 2–5m 走动验；必要时用真实人脸重做 INT8 校准
