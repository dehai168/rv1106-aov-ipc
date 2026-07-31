#ifndef IPC_MEDIA_MEDIA_OSD_H
#define IPC_MEDIA_MEDIA_OSD_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int enabled;
  int show_time;
  int show_name;
  int pos_x; /* overlay top-left X on stream (pixels, even) */
  int pos_y; /* overlay top-left Y on stream (pixels, even) */
  char name[64];
} MediaOsdConfig;

int media_osd_load_config(MediaOsdConfig *out);
int media_osd_save_config(const MediaOsdConfig *in);

/* Create/update/destroy RGN overlay as needed. Safe to call when stream is up. */
int media_osd_apply(const MediaOsdConfig *cfg);

/* Detach from VENC and destroy RGN; stop refresh thread. */
void media_osd_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* IPC_MEDIA_MEDIA_OSD_H */
