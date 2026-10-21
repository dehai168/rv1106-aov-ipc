#ifndef IPC_FACE_FACE_SERVICE_H
#define IPC_FACE_FACE_SERVICE_H

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FACE_MAX_REGIONS 4
#define FACE_SCHEDULE_GRID_LEN 168

typedef struct {
  int enabled;
  int x, y, w, h; /* normalized later; stored as 0-10000 or pixel — use 0..1000 of frame */
} FaceRegion;

typedef struct {
  int enabled;
  char model_path[256];
  int interval_ms;
  int hold_ms;
  int cooldown_ms;
  int confirm_frames;
  float score_thr;
  int min_face_px;
  int max_faces;
  int max_snap_per_frame;
  float margin;
  int face_jpeg_side;
  int save_scene;
  int link_record;
  int link_alarm;
  int quota_percent;
  int daily_limit;
  char sensitivity[16]; /* high/normal/low */
  FaceRegion regions[FACE_MAX_REGIONS];
  int schedule_enabled;
  char schedule_grid[FACE_SCHEDULE_GRID_LEN + 1];
} FaceConfig;

typedef struct {
  time_t ts;
  float score;
  float x, y, w, h; /* normalized 0..1 on grab frame */
  char face_file[160];
  char scene_file[128];
} FaceEvent;

int face_config_defaults(FaceConfig *cfg);
void face_config_load(FaceConfig *out);
int face_config_save(const FaceConfig *cfg);
void face_apply_sensitivity(FaceConfig *cfg);

int face_init(const FaceConfig *cfg);
void face_deinit(void);
int face_start(void);
int face_stop(void);
int face_is_running(void);
int face_snap_count(void);
int face_infer_count(void);
int face_last_event(FaceEvent *out);

/* Called when IVS motion fires (also subscribed via event_bus). */
void face_notify_motion(void);

#ifdef __cplusplus
}
#endif

#endif
