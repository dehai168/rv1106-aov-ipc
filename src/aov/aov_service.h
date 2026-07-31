#ifndef IPC_AOV_AOV_SERVICE_H
#define IPC_AOV_AOV_SERVICE_H

#include "media/media_service.h"
#include "record/record_service.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  AOV_MODE_ALWAYS_ON = 0,
  AOV_MODE_AOV = 1
} AovMode;

typedef enum {
  AOV_STATE_IDLE = 0,
  AOV_STATE_PREPARE_SLEEP,
  AOV_STATE_SLEEPING,
  AOV_STATE_POST_WAKE,
  AOV_STATE_LAPSE_1FPS,
  AOV_STATE_FULL_RECORD,
  AOV_STATE_FAULT
} AovState;

typedef enum {
  AOV_WAKE_UNKNOWN = 0,
  AOV_WAKE_TIMER,   /* PMU 1s lapse tick */
  AOV_WAKE_MOTION,  /* MCU FastAeMd / md_flag */
  AOV_WAKE_GPIO
} AovWakeReason;

typedef struct {
  int enabled;
  int suspend_ms;
  int event_fps;
  int quiet_sec;
  int event_record_sec; /* 0 = use quiet_sec */
  int max_cycles_before_reinit;
  int down_eth;
  /* When 0, EnterSleep skips writing /sys/power/state (safe dry-run until resume works). */
  int allow_mem_suspend;
  /* Debug: every N lapse cycles treat wake as motion (0=off). Until MIS5001 MCU FW. */
  int debug_motion_interval;
} AovConfig;

typedef struct {
  AovMode mode;
  AovState state;
  AovWakeReason last_wake;
  unsigned long cycles;
  unsigned long lapse_frames;
  unsigned long event_records;
  unsigned long sleep_fail;
  unsigned long media_reinit;
  int last_wake_ms;
  int in_aov;
} AovStatus;

int aov_init(void);
void aov_deinit(void);

void aov_load_config(AovConfig *cfg);
int aov_save_config(const AovConfig *cfg);
void aov_get_status(AovStatus *st);

int aov_is_active(void);

/* Enter/leave AOV mode (does not force mem until allow_mem_suspend). */
int aov_enter(void);
int aov_leave(void);

int aov_set_suspend_time_ms(int ms);
int aov_enter_sleep(void); /* prepare + optional mem */

/* One dry/real cycle for CLI: prepare → sleep → post_wake path (simulated wake if dry). */
int aov_probe_once(void);

/* Continuous lapse: enter once → N sleep/wake cycles → leave (FE-PHY stays off between). */
int aov_run_lapse_loop(int max_cycles);

/* Called after real resume (or simulated). reason from MCU/shared mem when available. */
int aov_on_wake(AovWakeReason reason);

/* Read MCU shared md_flag if mapped; else AOV_WAKE_UNKNOWN. */
AovWakeReason aov_poll_wake_reason(void);

const char *aov_state_name(AovState s);
const char *aov_wake_name(AovWakeReason r);

/* Enable lapse JPEG on wake (CLI --media). */
void aov_set_lapse_jpeg(int enable);

/* Bind encode/record configs for lapse_loop media reinit (T6.5.4). */
void aov_bind_media_loop(const MediaEncodeConfig *ecfg, const RecordConfig *rcfg);

#ifdef __cplusplus
}
#endif

#endif /* IPC_AOV_AOV_SERVICE_H */
