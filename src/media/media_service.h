#ifndef IPC_MEDIA_MEDIA_SERVICE_H
#define IPC_MEDIA_MEDIA_SERVICE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int width;
  int height;
  int vi_chn;          /* 0:mainpath 1:selfpath 2:bypass */
  const char *iq_dir;  /* e.g. /oem/usr/share/iqfiles */
} MediaConfig;

int media_init(const MediaConfig *cfg);
void media_deinit(void);

/* Capture frame_count NV12 frames into path (directory or full .yuv path handled by rockit save API under /userdata).
 * Returns 0 on success. Blocks until done or error.
 */
int media_capture_nv12(const char *out_dir, const char *file_name, int frame_count);

#ifdef __cplusplus
}
#endif

#endif /* IPC_MEDIA_MEDIA_SERVICE_H */
