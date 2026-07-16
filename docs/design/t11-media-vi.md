# T1.1 media_service：RKAIQ + VI 出图

## 目标
初始化 RKAIQ(3A) + VI，抓取 NV12 帧到 `/userdata/capture_0.yuv`，PC 端转 JPG 验图。

## API
- `media_init(const MediaConfig *cfg)` / `media_deinit()`
- `media_capture_nv12(const char *path, int frame_count)`：阻塞采集后返回

## 配置（default_config.json → video）
- width/height（默认 1920x1080，降低内存风险）
- iq_dir（默认 `/oem/usr/share/iqfiles`）
- vi_chn（默认 0 = rkisp_mainpath）

## 运行注意
与板上 `rkipc` 互斥，部署前 `killall rkipc`，测完可恢复。
