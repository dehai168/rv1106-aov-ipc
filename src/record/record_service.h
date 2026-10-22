#ifndef IPC_RECORD_RECORD_SERVICE_H
#define IPC_RECORD_RECORD_SERVICE_H

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RECORD_SCHEDULE_GRID_LEN 168

typedef struct {
  int segment_sec;          /* default 60 */
  int recycle_free_percent; /* recycle until free >= this, default 10 */
  int main_w;
  int main_h;
  int main_fps;
  int main_bitrate_kbps;
  int pre_record_sec; /* GOP ring length, default 4 */
  int quiet_sec;      /* motion silence -> IDLE, default 30 */

  /* Policy (persisted): motion | continuous | schedule */
  char mode[16];
  int schedule_enabled;
  char schedule_grid[RECORD_SCHEDULE_GRID_LEN + 1];
} RecordConfig;

typedef struct {
  char path[512];
  time_t start_ts;
  int64_t size_bytes;
  int kind; /* RECORD_KIND_* */
} RecordItem;

typedef enum {
  RECORD_MODE_OFF = 0,
  RECORD_MODE_CONTINUOUS,
  RECORD_MODE_MOTION
} RecordMode;

typedef enum {
  RECORD_KIND_NORMAL = 0, /* continuous/schedule 30min-style segments */
  RECORD_KIND_EVENT = 1   /* motion-linked event clips */
} RecordKind;

int record_init(const RecordConfig *cfg);
void record_deinit(void);

/* Continuous record (CLI --record). */
int record_start(void);
int record_stop(void);
int record_is_running(void);

/* Motion-linked mode: keep prerecord ring; record_on_motion() starts/extends. */
int record_arm_motion(void);
int record_on_motion(void);
/* Extend motion recording deadline (event_record_sec path). */
int record_extend_recording(int sec);
/* Close motion segment when quiet deadline passed (no new packets required). */
void record_poll_quiet_timeout(void);
/* Frames written to current/last motion segment. */
int record_frame_count(void);
void record_set_main_fps(int fps);
RecordMode record_mode(void);

/* Blocking helper for CLI: start, sleep seconds, stop. */
int record_run_for(int seconds);

/* Query mp4 under records/ with start_ts in [from, to]. to=0 means now.
 * RecordItem.kind tells event clips (records/events/...) from normal ones. */
int record_query(time_t from, time_t to, RecordItem *out, int max_items, int *out_count);

void record_config_load(RecordConfig *out);
int record_config_save(const RecordConfig *cfg);
/* 1 if grid slot for local now is armed (or schedule disabled / invalid => armed). */
int record_schedule_allows_now(const RecordConfig *cfg);

#ifdef __cplusplus
}
#endif

#endif
