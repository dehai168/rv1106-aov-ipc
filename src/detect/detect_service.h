#ifndef IPC_DETECT_DETECT_SERVICE_H
#define IPC_DETECT_DETECT_SERVICE_H

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DETECT_MAX_REGIONS 4
#define DETECT_SCHEDULE_GRID_LEN 168

typedef struct {
  int enabled;
  int x;
  int y;
  int w;
  int h;
} DetectRegion;

typedef struct {
  int sensitivity; /* 0..4, default 2 */
  int square_pct;  /* motion if 1000*square/(w*h) > this; default 8 */
  int hit_frames;  /* consecutive hits to fire; default 2 */
  int enabled;     /* 1=on */

  /* Soft ROI in detect VI pixels. No enabled region with w/h>0 => full frame. */
  DetectRegion regions[DETECT_MAX_REGIONS];

  /* Arm schedule: minutes from midnight; days bitmask bit0=Mon .. bit6=Sun. */
  int schedule_enabled;
  int schedule_start_min; /* 0..1439 (legacy fallback) */
  int schedule_end_min;   /* 0..1440, overnight if end < start */
  unsigned schedule_days; /* 0x7f = every day */
  /* 168 chars '0'/'1', row-major Mon..Sun x hour0..23; empty/invalid => use start/end/days */
  char schedule_grid[DETECT_SCHEDULE_GRID_LEN + 1];

  int link_record;   /* 1=record on motion */
  int link_snapshot; /* 1=JPEG on motion */
} DetectConfig;

typedef struct {
  time_t ts;
  int square;
  int square_pct_x10; /* 1000*square/(w*h) */
  int rect_x;
  int rect_y;
  int rect_w;
  int rect_h;
  char snapshot[128]; /* relative path under snapshots/, may be empty */
} DetectEvent;

typedef void (*DetectMotionCb)(const DetectEvent *ev, void *user);

int detect_init(const DetectConfig *cfg);
void detect_deinit(void);

int detect_start(void);
int detect_stop(void);
int detect_is_running(void);

void detect_set_motion_cb(DetectMotionCb cb, void *user);

int detect_motion_count(void);
int detect_last_event(DetectEvent *out);

/* Load/save DetectConfig from config center keys detect.*. */
void detect_config_load(DetectConfig *out);
int detect_config_save(const DetectConfig *cfg);

/* Sync schedule_days bitmask from grid (days that have any '1' hour). */
void detect_schedule_sync_days_from_grid(DetectConfig *cfg);

#ifdef __cplusplus
}
#endif

#endif /* IPC_DETECT_DETECT_SERVICE_H */
