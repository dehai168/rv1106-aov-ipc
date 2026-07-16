#ifndef IPC_RECORD_RECORD_SERVICE_H
#define IPC_RECORD_RECORD_SERVICE_H

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int segment_sec;          /* default 60 */
  int recycle_free_percent; /* recycle until free >= this, default 10 */
  int main_w;
  int main_h;
  int main_fps;
  int main_bitrate_kbps;
  int pre_record_sec; /* GOP ring length, default 4 */
  int quiet_sec;      /* motion silence -> IDLE, default 30 */
} RecordConfig;

typedef struct {
  char path[512];
  time_t start_ts;
  int64_t size_bytes;
} RecordItem;

typedef enum {
  RECORD_MODE_OFF = 0,
  RECORD_MODE_CONTINUOUS,
  RECORD_MODE_MOTION
} RecordMode;

int record_init(const RecordConfig *cfg);
void record_deinit(void);

/* Continuous record (CLI --record). */
int record_start(void);
int record_stop(void);
int record_is_running(void);

/* Motion-linked mode: keep prerecord ring; record_on_motion() starts/extends. */
int record_arm_motion(void);
int record_on_motion(void);
RecordMode record_mode(void);

/* Blocking helper for CLI: start, sleep seconds, stop. */
int record_run_for(int seconds);

/* Query mp4 under records/ with start_ts in [from, to]. to=0 means now. */
int record_query(time_t from, time_t to, RecordItem *out, int max_items, int *out_count);

#ifdef __cplusplus
}
#endif

#endif
