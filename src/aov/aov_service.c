#include "aov/aov_service.h"

#include "common/event_bus.h"
#include "common/log.h"
#include "media/media_service.h"
#include "record/record_service.h"
#include "system/config_service.h"
#include "system/network_service.h"
#include "system/storage_service.h"
#include "web/web_service.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

static AovConfig g_cfg;
static AovStatus g_st;
static int g_inited;
static int g_lapse_jpeg; /* set by CLI --media for this process */
static int g_media_loop_bound;
static int g_event_stream_active;
static int g_need_media_reinit;
static unsigned long g_cycles_since_reinit;
static MediaEncodeConfig g_loop_ecfg;
static char g_loop_iq_dir[256];
static RecordConfig g_loop_rcfg;

static int aov_reinit_media_pipeline(const char *why);
static int aov_restart_media_at_fps(const char *why, int fps);
static int aov_maybe_reinit_media(int force);
static void aov_finish_event_recording(void);

static void aov_lapse_maybe_snapshot(void)
{
  char path[160];
  char cmd[64];

  if (!g_lapse_jpeg || !media_stream_is_up()) {
    return;
  }
  snprintf(cmd, sizeof(cmd), "mkdir -p /mnt/sdcard/aov_lapse");
  system(cmd);
  snprintf(path, sizeof(path), "/mnt/sdcard/aov_lapse/lapse_%06lu.jpg", g_st.lapse_frames);
  media_isp_multi_frame();
  if (media_snapshot_jpeg(path) == 0) {
    log_info("aov", "lapse jpg %s", path);
  } else {
    log_warn("aov", "lapse jpg failed %s", path);
    /* Defer reinit to after cycle — tearing down mid-wake often hangs rockit. */
    g_need_media_reinit = 1;
  }
  /* Keep AIQ running between wakes when media-bound; prepare_sleep will pause. */
  if (!g_media_loop_bound) {
    media_isp_single_frame();
  }
}

void aov_set_lapse_jpeg(int enable)
{
  g_lapse_jpeg = enable ? 1 : 0;
}

void aov_bind_media_loop(const MediaEncodeConfig *ecfg, const RecordConfig *rcfg)
{
  if (!ecfg || !rcfg) {
    return;
  }
  g_loop_ecfg = *ecfg;
  snprintf(g_loop_iq_dir, sizeof(g_loop_iq_dir), "%s",
           ecfg->iq_dir && ecfg->iq_dir[0] ? ecfg->iq_dir : "/oem/usr/share/iqfiles");
  g_loop_ecfg.iq_dir = g_loop_iq_dir;
  g_loop_rcfg = *rcfg;
  g_media_loop_bound = 1;
  g_cycles_since_reinit = 0;
}

static int aov_restart_media_at_fps(const char *why, int fps)
{
  if (!g_media_loop_bound) {
    return -1;
  }
  if (fps <= 0) {
    fps = 1;
  }
  log_info("aov", "media restart fps=%d (%s)", fps, why);
  media_isp_multi_frame();
  record_deinit();
  g_loop_ecfg.main_fps = fps;
  g_loop_ecfg.sub_fps = (fps >= 15) ? 15 : fps;
  if (g_loop_ecfg.main_gop < fps * 2) {
    g_loop_ecfg.main_gop = fps * 2;
  }
  g_loop_rcfg.main_fps = fps;
  if (media_stream_reinit(&g_loop_ecfg) != 0) {
    log_error("aov", "media restart failed (%s)", why);
    return -1;
  }
  if (record_init(&g_loop_rcfg) != 0 || record_arm_motion() != 0) {
    log_error("aov", "record restart failed (%s)", why);
    media_stream_stop();
    return -1;
  }
  g_cycles_since_reinit = 0;
  g_st.media_reinit++;
  log_info("aov", "media+record restart ok (%s) fps=%d total=%lu", why, fps, g_st.media_reinit);
  return 0;
}

static int aov_reinit_media_pipeline(const char *why)
{
  if (!g_media_loop_bound || !media_stream_is_up()) {
    return -1;
  }
  log_info("aov", "media reinit begin (%s)", why);
  aov_finish_event_recording();
  return aov_restart_media_at_fps(why, 1);
}

static int aov_maybe_reinit_media(int force)
{
  if (!g_media_loop_bound) {
    return 0;
  }
  if (!force && !g_need_media_reinit && g_cfg.max_cycles_before_reinit <= 0) {
    return 0;
  }
  if (!force && !g_need_media_reinit &&
      g_cycles_since_reinit < (unsigned long)g_cfg.max_cycles_before_reinit) {
    return 0;
  }
  g_need_media_reinit = 0;
  return aov_reinit_media_pipeline(force ? "forced" : "periodic");
}

static int aov_prepare_event_stream(void)
{
  int fps = g_cfg.event_fps > 0 ? g_cfg.event_fps : 25;
  int packs;

  log_info("aov", "event prepare begin");
  if (media_isp_multi_frame() != 0) {
    log_warn("aov", "multi-frame failed");
  }
  packs = media_stream_pack_count_main();
  record_set_main_fps(fps);
  g_event_stream_active = 1;
  log_info("aov", "event prepare done packs=%d fps=%d", packs, fps);
  return 0;
}

static long long now_ms(void)
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (long long)tv.tv_sec * 1000LL + tv.tv_usec / 1000;
}

const char *aov_state_name(AovState s)
{
  switch (s) {
  case AOV_STATE_IDLE:
    return "idle";
  case AOV_STATE_PREPARE_SLEEP:
    return "prepare_sleep";
  case AOV_STATE_SLEEPING:
    return "sleeping";
  case AOV_STATE_POST_WAKE:
    return "post_wake";
  case AOV_STATE_LAPSE_1FPS:
    return "lapse_1fps";
  case AOV_STATE_FULL_RECORD:
    return "full_record";
  case AOV_STATE_FAULT:
    return "fault";
  default:
    return "unknown";
  }
}

const char *aov_wake_name(AovWakeReason r)
{
  switch (r) {
  case AOV_WAKE_TIMER:
    return "timer";
  case AOV_WAKE_MOTION:
    return "motion";
  case AOV_WAKE_GPIO:
    return "gpio";
  default:
    return "unknown";
  }
}

void aov_load_config(AovConfig *cfg)
{
  if (!cfg) {
    return;
  }
  memset(cfg, 0, sizeof(*cfg));
  config_get_bool("aov.enabled", &cfg->enabled, 0);
  config_get_int("aov.suspend_ms", &cfg->suspend_ms, 1000);
  config_get_int("aov.event_fps", &cfg->event_fps, 25);
  config_get_int("aov.quiet_sec", &cfg->quiet_sec, 30);
  config_get_int("aov.event_record_sec", &cfg->event_record_sec, 0);
  config_get_int("aov.max_cycles_before_reinit", &cfg->max_cycles_before_reinit, 40);
  config_get_bool("aov.down_eth", &cfg->down_eth, 1);
  config_get_bool("aov.allow_mem_suspend", &cfg->allow_mem_suspend, 0);
  config_get_int("aov.debug_motion_interval", &cfg->debug_motion_interval, 0);
  if (cfg->suspend_ms <= 0) {
    cfg->suspend_ms = 1000;
  }
  if (cfg->event_fps <= 0) {
    cfg->event_fps = 25;
  }
}

int aov_save_config(const AovConfig *cfg)
{
  if (!cfg) {
    return -1;
  }
  config_set_bool("aov.enabled", cfg->enabled);
  config_set_int("aov.suspend_ms", cfg->suspend_ms);
  config_set_int("aov.event_fps", cfg->event_fps);
  config_set_int("aov.quiet_sec", cfg->quiet_sec);
  config_set_int("aov.event_record_sec", cfg->event_record_sec);
  config_set_int("aov.max_cycles_before_reinit", cfg->max_cycles_before_reinit);
  config_set_bool("aov.down_eth", cfg->down_eth);
  config_set_bool("aov.allow_mem_suspend", cfg->allow_mem_suspend);
  config_set_int("aov.debug_motion_interval", cfg->debug_motion_interval);
  return config_save();
}

int aov_init(void)
{
  if (g_inited) {
    return 0;
  }
  aov_load_config(&g_cfg);
  memset(&g_st, 0, sizeof(g_st));
  g_st.mode = AOV_MODE_ALWAYS_ON;
  g_st.state = AOV_STATE_IDLE;
  g_inited = 1;
  log_info("aov", "init allow_mem=%d suspend_ms=%d", g_cfg.allow_mem_suspend,
           g_cfg.suspend_ms);
  return 0;
}

void aov_deinit(void)
{
  if (!g_inited) {
    return;
  }
  if (g_st.in_aov) {
    aov_leave();
  }
  g_inited = 0;
}

void aov_get_status(AovStatus *st)
{
  if (!st) {
    return;
  }
  *st = g_st;
}

int aov_is_active(void)
{
  return g_inited && g_st.in_aov;
}

AovWakeReason aov_poll_wake_reason(void)
{
  if (g_cfg.debug_motion_interval > 0 && g_st.cycles > 0 &&
      (g_st.cycles % g_cfg.debug_motion_interval) == 0) {
    log_info("aov", "debug motion wake (cycle %lu)", g_st.cycles);
    return AOV_WAKE_MOTION;
  }
  /* MCU shared_data.md_flag mapping requires MIS5001 wakeup FW. */
  return AOV_WAKE_UNKNOWN;
}

static void aov_finish_event_recording(void)
{
  int i;
  int max_wait;
  int frames;

  if (!record_is_running()) {
    if (g_event_stream_active) {
      g_event_stream_active = 0;
      record_set_main_fps(1);
      media_isp_single_frame();
    }
    return;
  }
  max_wait = g_cfg.event_record_sec > 0 ? g_cfg.event_record_sec + 5 : g_cfg.quiet_sec + 5;
  if (max_wait < 8) {
    max_wait = 8;
  }
  log_info("aov", "wait event record finish (max %ds)", max_wait);
  for (i = 0; i < max_wait && record_is_running(); i++) {
    frames = record_frame_count();
    log_info("aov", "event record tick %d/%d frames=%d", i + 1, max_wait, frames);
    /* No encoded frames after a few seconds → force stop (avoid silent hang). */
    if (frames <= 0 && i >= 3) {
      log_warn("aov", "event record stuck at 0 frames, force stop");
      record_stop();
      break;
    }
    record_poll_quiet_timeout();
    sleep(1);
  }
  frames = record_frame_count();
  if (record_is_running()) {
    log_warn("aov", "event record timeout, force stop (frames=%d)", frames);
    record_stop();
  } else {
    log_info("aov", "event record done frames=%d", frames);
  }
  g_st.state = AOV_STATE_LAPSE_1FPS;
  g_event_stream_active = 0;
  record_set_main_fps(1);
  media_isp_single_frame();
}

int aov_set_suspend_time_ms(int ms)
{
  char cmd[128];
  unsigned long val;

  if (ms <= 0) {
    ms = 1000;
  }
  val = (unsigned long)ms * 32UL;
  snprintf(cmd, sizeof(cmd), "io -4 0xff300048 %lu", val);
  if (system(cmd) != 0) {
    log_error("aov", "SetSuspendTime failed: %s", cmd);
    return -1;
  }
  log_info("aov", "SetSuspendTime %dms (reg=%lu)", ms, val);
  return 0;
}

static int prepare_sleep(void)
{
  g_st.state = AOV_STATE_PREPARE_SLEEP;

  web_preview_force_stop();
  aov_finish_event_recording();
  /* Keep motion armed so VENC packs refill the prerecord ring across sleep cycles.
   * record_stop() would set mode=OFF and drop all packets until the next arm. */
  if (g_media_loop_bound) {
    record_arm_motion();
  } else {
    record_stop();
  }

  if (g_cfg.down_eth) {
    /* Rockchip AOV tip: cut FE-PHY before mem, not just ip link down */
    if (network_prepare_aov_suspend() != 0) {
      log_warn("aov", "FE-PHY/gmac prepare failed (continue)");
    }
  }

  /* Pausing AIQ across mem is correct for power, but resume after motion wake
   * has hung rockit on this board; keep AIQ running when media loop is bound. */
  if (!g_media_loop_bound) {
    if (media_isp_single_frame() != 0) {
      log_warn("aov", "ISP single-frame failed (continue)");
    }
  } else {
    media_isp_multi_frame();
  }

  if (storage_prepare_suspend() != 0) {
    log_error("aov", "storage_prepare_suspend failed");
    if (g_cfg.down_eth) {
      network_restore_after_aov();
    }
    g_st.sleep_fail++;
    g_st.state = AOV_STATE_FAULT;
    return -1;
  }
  return 0;
}

static int write_mem_suspend(void)
{
  int fd;
  ssize_t n;

  fd = open("/sys/power/state", O_WRONLY);
  if (fd < 0) {
    log_error("aov", "open /sys/power/state: %s", strerror(errno));
    return -1;
  }
  n = write(fd, "mem", 3);
  close(fd);
  if (n != 3) {
    log_error("aov", "write mem failed: %s", strerror(errno));
    return -1;
  }
  return 0;
}

int aov_enter_sleep(void)
{
  long long t0;

  if (!g_inited) {
    return -1;
  }
  if (prepare_sleep() != 0) {
    return -1;
  }
  if (aov_set_suspend_time_ms(g_cfg.suspend_ms) != 0) {
    storage_resume_from_suspend();
    g_st.sleep_fail++;
    return -1;
  }

  g_st.state = AOV_STATE_SLEEPING;
  t0 = now_ms();

  if (!g_cfg.allow_mem_suspend) {
    log_warn("aov", "dry-run sleep (allow_mem_suspend=0); skip echo mem");
    usleep(50 * 1000);
    g_st.last_wake_ms = (int)(now_ms() - t0);
    return aov_on_wake(AOV_WAKE_TIMER);
  }

  if (write_mem_suspend() != 0) {
    storage_resume_from_suspend();
    media_isp_multi_frame();
    if (g_cfg.down_eth) {
      network_restore_after_aov();
    }
    g_st.sleep_fail++;
    g_st.state = AOV_STATE_FAULT;
    return -1;
  }

  /* Resumes here after wake */
  g_st.last_wake_ms = (int)(now_ms() - t0);
  g_st.cycles++;
  return aov_on_wake(aov_poll_wake_reason());
}

int aov_on_wake(AovWakeReason reason)
{
  AovWakeReason r = reason;

  g_st.state = AOV_STATE_POST_WAKE;
  if (r == AOV_WAKE_UNKNOWN) {
    r = AOV_WAKE_TIMER;
  }
  g_st.last_wake = r;

  if (storage_resume_from_suspend() != 0) {
    log_error("aov", "storage resume failed");
    g_st.state = AOV_STATE_FAULT;
    return -1;
  }

  if (g_cfg.down_eth) {
    network_resume_from_aov(); /* keep FE-PHY off across AOV cycles */
  }

  if (r == AOV_WAKE_MOTION || r == AOV_WAKE_GPIO) {
    g_st.state = AOV_STATE_FULL_RECORD;
    if (aov_prepare_event_stream() != 0) {
      log_warn("aov", "event stream prepare failed");
    }
    record_on_motion();
    if (g_cfg.event_record_sec > 0) {
      record_extend_recording(g_cfg.event_record_sec);
    }
    g_st.event_records++;
    log_info("aov", "FULL_RECORD wake=%s", aov_wake_name(r));
  } else {
    g_st.state = AOV_STATE_LAPSE_1FPS;
    g_st.lapse_frames++;
    log_info("aov", "LAPSE_1FPS wake=%s", aov_wake_name(r));
    if (!g_media_loop_bound) {
      if (media_isp_single_frame() != 0) {
        log_warn("aov", "keep single-frame failed");
      }
    } else if (media_isp_multi_frame() != 0) {
      log_warn("aov", "multi-frame for lapse failed");
    }
    aov_lapse_maybe_snapshot();
  }

  {
    Event ev = {.type = EVT_AOV_WAKE, .data = NULL, .data_len = 0};
    event_bus_publish(&ev);
  }
  return 0;
}

int aov_enter(void)
{
  Event ev;

  if (!g_inited) {
    return -1;
  }
  aov_load_config(&g_cfg);
  g_cfg.enabled = 1;
  aov_save_config(&g_cfg);

  web_preview_force_stop();
  g_st.mode = AOV_MODE_AOV;
  g_st.in_aov = 1;
  g_st.state = AOV_STATE_IDLE;

  ev.type = EVT_AOV_ENTER;
  ev.data = NULL;
  ev.data_len = 0;
  event_bus_publish(&ev);
  log_info("aov", "ENTER AOV (allow_mem=%d)", g_cfg.allow_mem_suspend);
  return 0;
}

int aov_leave(void)
{
  Event ev;

  if (!g_inited) {
    return -1;
  }
  record_stop();
  media_isp_multi_frame();
  storage_resume_from_suspend();
  if (g_cfg.down_eth) {
    network_restore_after_aov();
  }

  g_cfg.enabled = 0;
  aov_save_config(&g_cfg);
  g_st.mode = AOV_MODE_ALWAYS_ON;
  g_st.in_aov = 0;
  g_st.state = AOV_STATE_IDLE;

  ev.type = EVT_AOV_LEAVE;
  ev.data = NULL;
  ev.data_len = 0;
  event_bus_publish(&ev);
  log_info("aov", "LEAVE AOV");
  return 0;
}

int aov_probe_once(void)
{
  int rc;

  if (!g_inited && aov_init() != 0) {
    return -1;
  }
  aov_load_config(&g_cfg);
  log_info("aov", "probe_once allow_mem=%d", g_cfg.allow_mem_suspend);
  if (!g_st.in_aov) {
    aov_enter();
  }
  rc = aov_enter_sleep();
  /* CLI probe is one-shot: always leave so FE-PHY/DHCP restore across process exits */
  aov_leave();
  return rc;
}

int aov_run_lapse_loop(int max_cycles)
{
  int i;
  int rc = 0;

  if (max_cycles <= 0) {
    max_cycles = 5;
  }
  if (!g_inited && aov_init() != 0) {
    return -1;
  }
  aov_load_config(&g_cfg);
  log_info("aov", "lapse_loop n=%d allow_mem=%d", max_cycles, g_cfg.allow_mem_suspend);
  if (!g_st.in_aov) {
    if (aov_enter() != 0) {
      return -1;
    }
  }
  for (i = 0; i < max_cycles; i++) {
    if (aov_enter_sleep() != 0) {
      log_error("aov", "lapse_loop fail at cycle %d/%d", i + 1, max_cycles);
      rc = -1;
      break;
    }
    log_info("aov", "lapse_loop cycle %d/%d ok wake_ms=%d", i + 1, max_cycles,
             g_st.last_wake_ms);
    if (g_media_loop_bound) {
      g_cycles_since_reinit++;
      if (aov_maybe_reinit_media(0) != 0) {
        log_error("aov", "lapse_loop reinit failed at cycle %d/%d", i + 1, max_cycles);
        rc = -1;
        break;
      }
    }
  }
  /* If last cycle triggered motion record, wait for event to finish before leave. */
  if (g_st.state == AOV_STATE_FULL_RECORD || record_is_running()) {
    aov_finish_event_recording();
  }
  aov_leave();
  return rc;
}
