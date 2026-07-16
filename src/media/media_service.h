#ifndef IPC_MEDIA_MEDIA_SERVICE_H
#define IPC_MEDIA_MEDIA_SERVICE_H

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
  const char *iq_dir;
} MediaEncodeConfig;

int media_init(const MediaConfig *cfg);
void media_deinit(void);

int media_capture_nv12(const char *out_dir, const char *file_name, int frame_count);

/* Init AIQ+VI dual + VENC dual, bind, dump raw streams for `seconds`, then teardown. */
int media_encode_raw(const MediaEncodeConfig *cfg, const char *out_dir, int seconds);

#ifdef __cplusplus
}
#endif

#endif /* IPC_MEDIA_MEDIA_SERVICE_H */
