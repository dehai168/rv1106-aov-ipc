#ifndef IPC_DETECT_DETECT_SERVICE_H
#define IPC_DETECT_DETECT_SERVICE_H

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int sensitivity;      /* 0..4, default 2 */
  int square_pct;       /* motion if 1000*square/(w*h) > this; default 8 */
  int hit_frames;       /* consecutive hits to fire; default 2 */
  int enabled;          /* 1=on */

  /* Soft ROI in detect VI pixels; enabled=0 or w/h<=0 means full frame. */
  int region_enabled;
  int region_x;
  int region_y;
  int region_w;
  int region_h;

  /* Arm schedule: minutes from midnight; days bitmask bit0=Mon .. bit6=Sun. */
  int schedule_enabled;
  int schedule_start_min; /* 0..1439 */
  int schedule_end_min;   /* 0..1439, supports overnight if end < start */
  unsigned schedule_days; /* 0x7f = every day */
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

#ifdef __cplusplus
}
#endif

#endif
