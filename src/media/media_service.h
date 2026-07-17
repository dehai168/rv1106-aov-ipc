#ifndef IPC_MEDIA_MEDIA_SERVICE_H
#define IPC_MEDIA_MEDIA_SERVICE_H

#include <stdint.h>

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
  int detect_w; /* VI ch2; 0 = disabled */
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
int media_stream_apply(const MediaEncodeConfig *cfg); /* rebuild VI/VENC as needed */
void media_stream_stop(void);
int media_stream_is_up(void);
int media_stream_request_idr(int chn);

int media_stream_detect_enabled(void);
int media_stream_detect_width(void);
int media_stream_detect_height(void);

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

/* Capture one JPEG (grayscale from VI sub Y plane) while stream is up. */
int media_snapshot_jpeg(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* IPC_MEDIA_MEDIA_SERVICE_H */
