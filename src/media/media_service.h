#ifndef IPC_MEDIA_MEDIA_SERVICE_H
#define IPC_MEDIA_MEDIA_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#include "media/media_osd.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int width;
  int height;
  int vi_chn;         /* used by capture mode only */
  const char *iq_dir; /* e.g. /oem/usr/share/iqfiles */
} MediaConfig;

typedef struct {
  int main_w;
  int main_h;
  int main_fps;
  int main_bitrate_kbps;
  int main_gop;
  int sub_w;
  int sub_h;
  int sub_fps;
  int sub_bitrate_kbps;
  int sub_gop;
  int detect_w; /* >0 enables IVS on VI2; runtime size follows VI2 (preview-main) */
  int detect_h;
  const char *iq_dir;
} MediaEncodeConfig;

/* chn: 0=main H265, 1=sub H264. pts_us from VENC. */
typedef void (*MediaPacketCb)(int chn, const uint8_t *data, uint32_t len, int key_frame,
                              int64_t pts_us, void *user);

int media_init(const MediaConfig *cfg);
void media_deinit(void);

int media_capture_nv12(const char *out_dir, const char *file_name, int frame_count);

/* One-shot encode dump (uses stream session internally). */
int media_encode_raw(const MediaEncodeConfig *cfg, const char *out_dir, int seconds);

/* Persistent dual-stream session (T1.3). Optional VI ch2 when detect_w/h > 0. */
int media_stream_start(const MediaEncodeConfig *cfg);
int media_stream_reinit(const MediaEncodeConfig *cfg); /* stop + start (AOV recovery) */
int media_stream_apply(const MediaEncodeConfig *cfg); /* rebuild VI/VENC as needed */
void media_stream_stop(void);
int media_stream_is_up(void);
int media_stream_pack_count_main(void);
int media_stream_request_idr(int chn);

int media_stream_detect_enabled(void);
int media_stream_detect_width(void);
int media_stream_detect_height(void);

/* Start/stop preview-main VENC3 encode (VI2). Off by default to save ISP/CPU. */
int media_stream_preview_main_set_active(int on);
int media_stream_preview_main_active(void);

/* Optional raw dump while stream is up (NULL path disables that chn dump). */
int media_stream_set_dump(const char *main_path, const char *sub_path);
void media_stream_clear_dump(void);

/* Multi-subscriber packet fan-out (preferred). */
int media_stream_add_packet_listener(MediaPacketCb cb, void *user);
void media_stream_remove_packet_listener(MediaPacketCb cb, void *user);

/* Legacy single-slot helper (clears listeners then adds one / NULL). */
void media_stream_set_packet_cb(MediaPacketCb cb, void *user);

typedef struct {
  int brightness; /* 0-100 */
  int contrast;
  int saturation;
  int mirror; /* 0/1 */
  int flip;   /* 0/1 */
} MediaImageConfig;

int media_image_get(MediaImageConfig *out);
int media_image_set(const MediaImageConfig *in, int apply);

int media_osd_get(MediaOsdConfig *out);
int media_osd_set(const MediaOsdConfig *in, int apply);

/* One-shot hardware JPEG via VENC combo (master = sub H.264). Requires stream up. */
int media_snapshot_jpeg(const char *path);
/* Same as media_snapshot_jpeg but returns malloc'd buffer (caller free). */
int media_snapshot_jpeg_mem(uint8_t **out, size_t *out_len);
/* stream 0 = sub (sub_w x sub_h), 1 = main (main_w x main_h). */
int media_snapshot_jpeg_stream(const char *path, int stream);
int media_snapshot_jpeg_stream_mem(int stream, uint8_t **out, size_t *out_len);
/* Configured main encode geometry (record main / main snapshot). */
int media_stream_main_width(void);
int media_stream_main_height(void);

/* Grab one NV12 frame from VI1 (sub, depth>=2). Caller must media_release_*_nv12.
 * Buffer is RGA-copied into MMZ (contiguous NV12, stride=width). priv = MediaNv12Owned*. */
typedef struct {
  const uint8_t *y;
  const uint8_t *uv;
  int width;
  int height;
  int vir_width;
  int vir_height;
  int y_stride;
  int uv_stride;
  size_t nv12_size;
  void *priv; /* MediaNv12Owned* — MmzFree on release */
  int vi_chn; /* 1=sub, 2=detect */
} MediaNv12Frame;

int media_grab_sub_nv12(MediaNv12Frame *out, int timeout_ms);
void media_release_sub_nv12(MediaNv12Frame *frame);
/* Grab from VI2 (detect/IVS path). Prefer this for face — does not starve preview sub. */
int media_grab_detect_nv12(MediaNv12Frame *out, int timeout_ms);
void media_release_detect_nv12(MediaNv12Frame *frame);
int media_stream_sub_width(void);
int media_stream_sub_height(void);

/* AOV: ISP single-frame (pause AIQ) / multi-frame (resume AIQ). */
int media_isp_single_frame(void);
int media_isp_multi_frame(void);
/* Apply main encode fps for event recording (best-effort stream_apply). */
int media_aov_apply_event_fps(int fps);

#ifdef __cplusplus
}
#endif

#endif /* IPC_MEDIA_MEDIA_SERVICE_H */
