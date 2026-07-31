#include "record/record_service.h"

#include "common/event_bus.h"
#include "common/log.h"
#include "media/media_service.h"
#include "system/config_service.h"
#include "system/storage_service.h"

#include "rkmuxer.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MUXER_ID 0
#define RING_CAP 96
#define RING_BYTES_MAX (3 * 1024 * 1024)
#define WQ_CAP 96

typedef struct {
  uint8_t *data;
  uint32_t len;
  int key;
  int64_t pts;
  time_t wall;
} RingPkt;

typedef struct {
  uint8_t *data;
  uint32_t len;
  int key;
  int64_t pts;
} WqPkt;

typedef struct {
  int inited;
  RecordMode mode;
  int waiting_key;
  int muxer_open;
  int mux_closing;
  int mux_writing;
  int seg_active;
  int flushing_prerecord;
  RecordConfig cfg;
  char cur_path[512];   /* final destination on TF */
  char stage_path[512]; /* /tmp staging file while muxing */
  time_t seg_start;
  int64_t seg_mono_us;  /* pacing clock after open / prerecord flush */
  int64_t mux_idx;      /* frames accepted into mux queue this segment */
  int64_t mux_idx_base; /* mux_idx after prerecord; live pacing origin */
  time_t run_deadline;  /* continuous mode */
  time_t quiet_deadline; /* motion mode */
  pthread_mutex_t lock;
  pthread_cond_t wq_cond;
  pthread_t writer_th;
  int writer_run;
  int writer_started;
  WqPkt wq[WQ_CAP];
  int wq_count;
  int segments;
  int frames;
  int seg_frames;
  int listener_added;
  RingPkt ring[RING_CAP];
  int ring_count;
  size_t ring_bytes;
} RecordCtx;

static RecordCtx g_rec = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .wq_cond = PTHREAD_COND_INITIALIZER,
};

static int64_t mono_us(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000LL + (int64_t)ts.tv_nsec / 1000LL;
}

static int ensure_day_dir(char *day_dir, size_t n, const struct tm *tm)
{
  snprintf(day_dir, n, "%s/%04d%02d%02d", storage_records_path(), tm->tm_year + 1900,
           tm->tm_mon + 1, tm->tm_mday);
  struct stat st;
  if (stat(day_dir, &st) == 0) {
    return S_ISDIR(st.st_mode) ? 0 : -1;
  }
  if (mkdir(day_dir, 0755) != 0) {
    log_error("record", "mkdir %s failed", day_dir);
    return -1;
  }
  return 0;
}

static void ring_free_at(int idx)
{
  if (g_rec.ring[idx].data) {
    g_rec.ring_bytes -= g_rec.ring[idx].len;
    free(g_rec.ring[idx].data);
    g_rec.ring[idx].data = NULL;
    g_rec.ring[idx].len = 0;
  }
}

static void ring_clear(void)
{
  int i;
  for (i = 0; i < g_rec.ring_count; i++) {
    ring_free_at(i);
  }
  g_rec.ring_count = 0;
  g_rec.ring_bytes = 0;
}

static void ring_pop_front(void)
{
  int i;
  if (g_rec.ring_count <= 0) {
    return;
  }
  ring_free_at(0);
  for (i = 1; i < g_rec.ring_count; i++) {
    g_rec.ring[i - 1] = g_rec.ring[i];
  }
  g_rec.ring_count--;
  memset(&g_rec.ring[g_rec.ring_count], 0, sizeof(RingPkt));
}

static void ring_trim_locked(void)
{
  int pre = g_rec.cfg.pre_record_sec > 0 ? g_rec.cfg.pre_record_sec : 4;
  time_t cutoff = time(NULL) - pre;
  while (g_rec.ring_count > 0) {
    if (g_rec.ring_count >= RING_CAP || g_rec.ring_bytes > RING_BYTES_MAX ||
        g_rec.ring[0].wall < cutoff) {
      /* keep at least from a keyframe if possible */
      if (g_rec.ring_count > 1 && !g_rec.ring[1].key && g_rec.ring[0].key &&
          g_rec.ring[0].wall >= cutoff && g_rec.ring_count < RING_CAP &&
          g_rec.ring_bytes <= RING_BYTES_MAX) {
        break;
      }
      ring_pop_front();
      continue;
    }
    break;
  }
}

static int ring_push_locked(const uint8_t *data, uint32_t len, int key, int64_t pts)
{
  uint8_t *copy;
  if (!data || len == 0) {
    return -1;
  }
  while (g_rec.ring_count >= RING_CAP || g_rec.ring_bytes + len > RING_BYTES_MAX) {
    ring_pop_front();
  }
  copy = (uint8_t *)malloc(len);
  if (!copy) {
    return -2;
  }
  memcpy(copy, data, len);
  g_rec.ring[g_rec.ring_count].data = copy;
  g_rec.ring[g_rec.ring_count].len = len;
  g_rec.ring[g_rec.ring_count].key = key;
  g_rec.ring[g_rec.ring_count].pts = pts;
  g_rec.ring[g_rec.ring_count].wall = time(NULL);
  g_rec.ring_count++;
  g_rec.ring_bytes += len;
  ring_trim_locked();
  return 0;
}

static void wq_clear_locked(void)
{
  int i;
  for (i = 0; i < g_rec.wq_count; i++) {
    free(g_rec.wq[i].data);
    g_rec.wq[i].data = NULL;
  }
  g_rec.wq_count = 0;
}

static int promote_stage_to_final(const char *stage, const char *final_path)
{
  FILE *in;
  FILE *out;
  char buf[8192];
  size_t n;
  int rc = 0;

  in = fopen(stage, "rb");
  if (!in) {
    log_error("record", "open stage %s failed", stage);
    return -1;
  }
  out = fopen(final_path, "wb");
  if (!out) {
    log_error("record", "open final %s failed", final_path);
    fclose(in);
    return -2;
  }
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
    if (fwrite(buf, 1, n, out) != n) {
      log_error("record", "write final %s failed", final_path);
      rc = -3;
      break;
    }
  }
  fclose(in);
  if (fclose(out) != 0 && rc == 0) {
    rc = -4;
  }
  return rc;
}

static void mux_wait_idle_unlocked(void)
{
  int i;
  for (i = 0; i < 200; i++) { /* ~2s */
    pthread_mutex_lock(&g_rec.lock);
    if (g_rec.wq_count == 0 && !g_rec.mux_writing) {
      pthread_mutex_unlock(&g_rec.lock);
      return;
    }
    pthread_mutex_unlock(&g_rec.lock);
    usleep(10 * 1000);
  }
  log_warn("record", "mux queue still busy after wait (wq=%d writing=%d)", g_rec.wq_count,
           g_rec.mux_writing);
}

static void *mux_writer_thread(void *arg)
{
  (void)arg;
  for (;;) {
    WqPkt pkt;

    pthread_mutex_lock(&g_rec.lock);
    while (g_rec.wq_count <= 0 && g_rec.writer_run) {
      pthread_cond_wait(&g_rec.wq_cond, &g_rec.lock);
    }
    if (g_rec.wq_count <= 0 && !g_rec.writer_run) {
      pthread_mutex_unlock(&g_rec.lock);
      break;
    }
    pkt = g_rec.wq[0];
    memmove(&g_rec.wq[0], &g_rec.wq[1], (size_t)(g_rec.wq_count - 1) * sizeof(WqPkt));
    g_rec.wq_count--;
    /* Always write queued packets; close() stops enqueue first, then drains. */
    g_rec.mux_writing = 1;
    pthread_mutex_unlock(&g_rec.lock);

    if (pkt.data && pkt.len > 0) {
      rkmuxer_write_video_frame(MUXER_ID, pkt.data, pkt.len, pkt.pts, pkt.key);
    }
    free(pkt.data);

    pthread_mutex_lock(&g_rec.lock);
    g_rec.mux_writing = 0;
    g_rec.frames++;
    g_rec.seg_frames++;
    pthread_mutex_unlock(&g_rec.lock);
  }
  return NULL;
}

static int writer_start(void)
{
  if (g_rec.writer_started) {
    return 0;
  }
  g_rec.writer_run = 1;
  if (pthread_create(&g_rec.writer_th, NULL, mux_writer_thread, NULL) != 0) {
    g_rec.writer_run = 0;
    return -1;
  }
  g_rec.writer_started = 1;
  return 0;
}

static void writer_stop(void)
{
  if (!g_rec.writer_started) {
    return;
  }
  pthread_mutex_lock(&g_rec.lock);
  g_rec.writer_run = 0;
  pthread_cond_signal(&g_rec.wq_cond);
  pthread_mutex_unlock(&g_rec.lock);
  pthread_join(g_rec.writer_th, NULL);
  g_rec.writer_started = 0;
  pthread_mutex_lock(&g_rec.lock);
  wq_clear_locked();
  pthread_mutex_unlock(&g_rec.lock);
}

static int open_segment_locked(void)
{
  time_t now = time(NULL);
  struct tm tm;
  localtime_r(&now, &tm);
  char day_dir[256];
  VideoParam vp;
  int ret;

  if (ensure_day_dir(day_dir, sizeof(day_dir), &tm) != 0) {
    return -1;
  }

  snprintf(g_rec.cur_path, sizeof(g_rec.cur_path), "%s/%04d%02d%02d_%02d%02d%02d.mp4", day_dir,
           tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
  /* Mux to tmpfs first — direct exFAT write after AOV remount can stall rkmuxer. */
  snprintf(g_rec.stage_path, sizeof(g_rec.stage_path), "/tmp/ipc_rec_%d_%ld.mp4", g_rec.segments + 1,
           (long)now);

  memset(&vp, 0, sizeof(vp));
  snprintf(vp.format, sizeof(vp.format), "NV12");
  snprintf(vp.codec, sizeof(vp.codec), "H.265");
  vp.width = g_rec.cfg.main_w;
  vp.height = g_rec.cfg.main_h;
  vp.vir_width = g_rec.cfg.main_w;
  vp.vir_height = g_rec.cfg.main_h;
  vp.bit_rate = g_rec.cfg.main_bitrate_kbps * 1024;
  vp.frame_rate_num = g_rec.cfg.main_fps > 0 ? g_rec.cfg.main_fps : 15;
  vp.frame_rate_den = 1;

  if (g_rec.muxer_open) {
    g_rec.mux_closing = 1;
    pthread_mutex_unlock(&g_rec.lock);
    mux_wait_idle_unlocked();
    pthread_mutex_lock(&g_rec.lock);
    rkmuxer_deinit(MUXER_ID);
    g_rec.muxer_open = 0;
    if (g_rec.stage_path[0]) {
      unlink(g_rec.stage_path);
    }
  }

  g_rec.mux_closing = 0;
  ret = rkmuxer_init(MUXER_ID, NULL, g_rec.stage_path, &vp, NULL);
  if (ret != 0) {
    log_error("record", "rkmuxer_init failed %d path=%s", ret, g_rec.stage_path);
    g_rec.stage_path[0] = '\0';
    return -2;
  }
  g_rec.muxer_open = 1;
  g_rec.seg_active = 1;
  g_rec.seg_start = now;
  g_rec.seg_mono_us = mono_us();
  g_rec.mux_idx = 0;
  g_rec.mux_idx_base = 0;
  g_rec.waiting_key = 1;
  g_rec.seg_frames = 0;
  g_rec.segments++;
  log_info("record", "segment open #%d stage=%s -> %s", g_rec.segments, g_rec.stage_path,
           g_rec.cur_path);

  if (storage_free_percent() >= 0 && storage_free_percent() < g_rec.cfg.recycle_free_percent) {
    storage_recycle_oldest(g_rec.cfg.recycle_free_percent);
  }
  return 0;
}

static void close_segment_locked(void)
{
  char final_path[512];
  char stage_path[512];
  int empty;
  int frames;

  if (!g_rec.muxer_open && g_rec.stage_path[0] == '\0') {
    return;
  }
  /* Stop enqueue from VENC callback, drain writer, then deinit muxer. */
  g_rec.muxer_open = 0;
  snprintf(final_path, sizeof(final_path), "%s", g_rec.cur_path);
  snprintf(stage_path, sizeof(stage_path), "%s", g_rec.stage_path);
  pthread_mutex_unlock(&g_rec.lock);
  mux_wait_idle_unlocked();
  pthread_mutex_lock(&g_rec.lock);
  empty = (g_rec.seg_frames <= 0);
  frames = g_rec.seg_frames;
  g_rec.mux_closing = 1;
  pthread_mutex_unlock(&g_rec.lock);
  rkmuxer_deinit(MUXER_ID);
  pthread_mutex_lock(&g_rec.lock);
  g_rec.mux_closing = 0;
  g_rec.seg_active = 0;
  g_rec.stage_path[0] = '\0';

  if (empty) {
    if (stage_path[0]) {
      unlink(stage_path);
    }
    log_warn("record", "segment empty, removed %s", stage_path[0] ? stage_path : final_path);
    return;
  }
  if (promote_stage_to_final(stage_path, final_path) != 0) {
    log_error("record", "promote failed stage=%s final=%s (keep stage)", stage_path, final_path);
    log_info("record", "segment close (staged) %s frames=%d", stage_path, frames);
    return;
  }
  unlink(stage_path);
  log_info("record", "segment close %s frames=%d", final_path, frames);
}

static void write_frame_locked(const uint8_t *data, uint32_t len, int key, int64_t pts)
{
  uint8_t *copy;
  int fps;
  int64_t period;
  int64_t mux_pts;
  int64_t allowed;

  (void)pts;
  if (!g_rec.muxer_open || g_rec.mux_closing || !data || len == 0) {
    return;
  }
  if (g_rec.waiting_key && !key) {
    return;
  }

  fps = g_rec.cfg.main_fps > 0 ? g_rec.cfg.main_fps : 15;
  period = 1000000LL / fps;
  /*
   * rkmuxer stamps each written frame at 1/frame_rate regardless of present_time
   * deltas, so duration ≈ nb_frames / fps. Pace accepts to cfg fps so a 30s
   * wall segment yields ~30s playback (not ~2× when sensor runs ~30fps).
   */
  if (!g_rec.flushing_prerecord) {
    if (g_rec.seg_mono_us <= 0) {
      g_rec.seg_mono_us = mono_us();
    }
    allowed =
        g_rec.mux_idx_base + ((mono_us() - g_rec.seg_mono_us) * fps) / 1000000LL + 1;
    if (g_rec.mux_idx >= allowed && !key) {
      return;
    }
  }

  if (g_rec.wq_count >= WQ_CAP) {
    return; /* drop — never block VENC GetStream callback */
  }
  copy = (uint8_t *)malloc(len);
  if (!copy) {
    return;
  }
  memcpy(copy, data, len);
  mux_pts = g_rec.mux_idx * period;
  g_rec.mux_idx++;
  g_rec.waiting_key = 0;
  g_rec.wq[g_rec.wq_count].data = copy;
  g_rec.wq[g_rec.wq_count].len = len;
  g_rec.wq[g_rec.wq_count].key = key;
  g_rec.wq[g_rec.wq_count].pts = mux_pts;
  g_rec.wq_count++;
  pthread_cond_signal(&g_rec.wq_cond);
}

static void flush_prerecord_locked(void)
{
  int i;
  int start = -1;
  int pre = g_rec.cfg.pre_record_sec > 0 ? g_rec.cfg.pre_record_sec : 4;
  time_t cutoff = time(NULL) - pre;

  for (i = 0; i < g_rec.ring_count; i++) {
    if (g_rec.ring[i].key && g_rec.ring[i].wall >= cutoff) {
      start = i;
      break;
    }
  }
  if (start < 0) {
    for (i = 0; i < g_rec.ring_count; i++) {
      if (g_rec.ring[i].key) {
        start = i;
      }
    }
  }
  if (start < 0) {
    start = 0;
  }

  g_rec.flushing_prerecord = 1;
  /* Only drop waiting_key if we actually flush from a keyframe. An empty ring
   * used to clear waiting_key and then feed non-IDR to rkmuxer (can hang). */
  if (g_rec.ring_count <= 0 || start < 0 || !g_rec.ring[start].key) {
    g_rec.flushing_prerecord = 0;
    log_info("record", "flushed prerecord pkts=0 (wait key)");
    ring_clear();
    return;
  }
  for (i = start; i < g_rec.ring_count; i++) {
    if (!g_rec.ring[i].data) {
      continue;
    }
    write_frame_locked(g_rec.ring[i].data, g_rec.ring[i].len, g_rec.ring[i].key,
                       g_rec.ring[i].pts);
  }
  g_rec.flushing_prerecord = 0;
  /* Live pacing continues after prerecord frame count. */
  g_rec.mux_idx_base = g_rec.mux_idx;
  g_rec.seg_mono_us = mono_us();
  log_info("record", "flushed prerecord pkts=%d from=%d frames=%d", g_rec.ring_count - start, start,
           g_rec.seg_frames);
  ring_clear();
}

static void on_packet(int chn, const uint8_t *data, uint32_t len, int key_frame, int64_t pts_us,
                      void *user)
{
  (void)user;
  time_t now;
  if (chn != 0 || !data || len == 0) {
    return;
  }

  pthread_mutex_lock(&g_rec.lock);
  if (g_rec.mode == RECORD_MODE_OFF) {
    pthread_mutex_unlock(&g_rec.lock);
    return;
  }

  ring_push_locked(data, len, key_frame, pts_us);
  now = time(NULL);

  if (g_rec.mode == RECORD_MODE_CONTINUOUS) {
    if (g_rec.run_deadline > 0 && now >= g_rec.run_deadline) {
      g_rec.mode = RECORD_MODE_OFF;
      close_segment_locked();
      pthread_mutex_unlock(&g_rec.lock);
      return;
    }
    if (!g_rec.muxer_open && open_segment_locked() != 0) {
      pthread_mutex_unlock(&g_rec.lock);
      return;
    }
    if (g_rec.cfg.segment_sec > 0 && g_rec.frames > 0 &&
        (now - g_rec.seg_start) >= g_rec.cfg.segment_sec && key_frame) {
      close_segment_locked();
      if (open_segment_locked() != 0) {
        pthread_mutex_unlock(&g_rec.lock);
        return;
      }
    }
    write_frame_locked(data, len, key_frame, pts_us);
  } else if (g_rec.mode == RECORD_MODE_MOTION) {
    if (g_rec.muxer_open) {
      if (g_rec.quiet_deadline > 0 && now >= g_rec.quiet_deadline) {
        close_segment_locked();
        Event e = {.type = EVT_RECORD_STOP, .data = NULL, .data_len = 0};
        pthread_mutex_unlock(&g_rec.lock);
        event_bus_publish(&e);
        log_info("record", "motion quiet -> idle");
        return;
      }
      if (g_rec.cfg.segment_sec > 0 && g_rec.frames > 0 &&
          (now - g_rec.seg_start) >= g_rec.cfg.segment_sec && key_frame) {
        close_segment_locked();
        if (open_segment_locked() != 0) {
          pthread_mutex_unlock(&g_rec.lock);
          return;
        }
        g_rec.waiting_key = 0;
      }
      write_frame_locked(data, len, key_frame, pts_us);
    }
  }

  pthread_mutex_unlock(&g_rec.lock);
}

static int schedule_grid_valid(const char *grid)
{
  size_t i;
  if (!grid || strlen(grid) != RECORD_SCHEDULE_GRID_LEN) {
    return 0;
  }
  for (i = 0; i < RECORD_SCHEDULE_GRID_LEN; i++) {
    if (grid[i] != '0' && grid[i] != '1') {
      return 0;
    }
  }
  return 1;
}

void record_config_load(RecordConfig *out)
{
  char grid[RECORD_SCHEDULE_GRID_LEN + 8];
  char mode[32];

  if (!out) {
    return;
  }
  memset(out, 0, sizeof(*out));
  config_get_int("record.segment_sec", &out->segment_sec, 30);
  config_get_int("record.recycle_free_percent", &out->recycle_free_percent, 10);
  config_get_int("record.pre_record_sec", &out->pre_record_sec, 4);
  config_get_int("record.quiet_sec", &out->quiet_sec, 30);
  config_get_int("video.encode.main_w", &out->main_w, 1920);
  config_get_int("video.encode.main_h", &out->main_h, 1080);
  config_get_int("video.encode.main_fps", &out->main_fps, 15);
  config_get_int("video.encode.main_bitrate_kbps", &out->main_bitrate_kbps, 2048);

  memset(mode, 0, sizeof(mode));
  config_get_string("record.mode", mode, (int)sizeof(mode), "motion");
  if (strcmp(mode, "continuous") == 0) {
    memcpy(out->mode, "continuous", 11);
  } else if (strcmp(mode, "schedule") == 0) {
    memcpy(out->mode, "schedule", 9);
  } else {
    memcpy(out->mode, "motion", 7);
  }
  config_get_bool("record.schedule.enabled", &out->schedule_enabled, 0);
  memset(grid, 0, sizeof(grid));
  config_get_string("record.schedule.grid", grid, (int)sizeof(grid), "");
  if (schedule_grid_valid(grid)) {
    memcpy(out->schedule_grid, grid, RECORD_SCHEDULE_GRID_LEN);
    out->schedule_grid[RECORD_SCHEDULE_GRID_LEN] = '\0';
  } else {
    memset(out->schedule_grid, '1', RECORD_SCHEDULE_GRID_LEN);
    out->schedule_grid[RECORD_SCHEDULE_GRID_LEN] = '\0';
  }
}

int record_config_save(const RecordConfig *cfg)
{
  const char *mode;

  if (!cfg) {
    return -1;
  }
  mode = cfg->mode[0] ? cfg->mode : "motion";
  if (strcmp(mode, "continuous") != 0 && strcmp(mode, "schedule") != 0) {
    mode = "motion";
  }
  if (config_set_string("record.mode", mode) != 0 ||
      config_set_int("record.segment_sec", cfg->segment_sec) != 0 ||
      config_set_int("record.recycle_free_percent", cfg->recycle_free_percent) != 0 ||
      config_set_int("record.pre_record_sec", cfg->pre_record_sec) != 0 ||
      config_set_int("record.quiet_sec", cfg->quiet_sec) != 0 ||
      config_set_bool("record.schedule.enabled", cfg->schedule_enabled) != 0 ||
      config_set_string("record.schedule.grid",
                        schedule_grid_valid(cfg->schedule_grid) ? cfg->schedule_grid
                                                                : "") != 0 ||
      config_save() != 0) {
    return -1;
  }
  return 0;
}

int record_schedule_allows_now(const RecordConfig *cfg)
{
  time_t now;
  struct tm tm_now;
  int day_idx;

  if (!cfg) {
    return 1;
  }
  if (!cfg->schedule_enabled) {
    return 1;
  }
  if (!schedule_grid_valid(cfg->schedule_grid)) {
    return 1;
  }
  now = time(NULL);
  localtime_r(&now, &tm_now);
  if (tm_now.tm_wday == 0) {
    day_idx = 6;
  } else {
    day_idx = tm_now.tm_wday - 1;
  }
  return cfg->schedule_grid[day_idx * 24 + tm_now.tm_hour] == '1' ? 1 : 0;
}

int record_init(const RecordConfig *cfg)
{
  if (!cfg) {
    return -1;
  }
  g_rec.cfg = *cfg;
  if (g_rec.cfg.segment_sec <= 0) {
    g_rec.cfg.segment_sec = 60;
  }
  if (g_rec.cfg.recycle_free_percent <= 0) {
    g_rec.cfg.recycle_free_percent = 10;
  }
  if (g_rec.cfg.pre_record_sec <= 0) {
    g_rec.cfg.pre_record_sec = 4;
  }
  if (g_rec.cfg.quiet_sec <= 0) {
    g_rec.cfg.quiet_sec = 30;
  }
  if (!g_rec.cfg.mode[0]) {
    snprintf(g_rec.cfg.mode, sizeof(g_rec.cfg.mode), "motion");
  }
  if (storage_ensure_dirs() != 0) {
    return -2;
  }
  storage_cleanup_bad_files();
  g_rec.inited = 1;
  g_rec.mode = RECORD_MODE_OFF;
  g_rec.segments = 0;
  g_rec.frames = 0;
  ring_clear();
  wq_clear_locked();
  if (writer_start() != 0) {
    log_error("record", "mux writer thread start failed");
    g_rec.inited = 0;
    return -4;
  }
  if (!g_rec.listener_added) {
    if (media_stream_add_packet_listener(on_packet, NULL) != 0) {
      writer_stop();
      return -3;
    }
    g_rec.listener_added = 1;
  }
  log_info("record", "init segment=%ds pre=%ds quiet=%ds recycle<%d%%", g_rec.cfg.segment_sec,
           g_rec.cfg.pre_record_sec, g_rec.cfg.quiet_sec, g_rec.cfg.recycle_free_percent);
  return 0;
}

void record_deinit(void)
{
  record_stop();
  if (g_rec.listener_added) {
    media_stream_remove_packet_listener(on_packet, NULL);
    g_rec.listener_added = 0;
  }
  writer_stop();
  ring_clear();
  g_rec.inited = 0;
}

int record_start(void)
{
  Event e;
  if (!g_rec.inited) {
    return -1;
  }
  pthread_mutex_lock(&g_rec.lock);
  g_rec.mode = RECORD_MODE_CONTINUOUS;
  g_rec.run_deadline = 0;
  g_rec.quiet_deadline = 0;
  g_rec.waiting_key = 1;
  g_rec.segments = 0;
  g_rec.frames = 0;
  pthread_mutex_unlock(&g_rec.lock);
  media_stream_request_idr(0);
  e.type = EVT_RECORD_START;
  e.data = NULL;
  e.data_len = 0;
  event_bus_publish(&e);
  log_info("record", "start continuous");
  return 0;
}

int record_stop(void)
{
  int segs;
  int frames;
  Event e;
  pthread_mutex_lock(&g_rec.lock);
  g_rec.mode = RECORD_MODE_OFF;
  close_segment_locked();
  segs = g_rec.segments;
  frames = g_rec.frames;
  pthread_mutex_unlock(&g_rec.lock);
  e.type = EVT_RECORD_STOP;
  e.data = NULL;
  e.data_len = 0;
  event_bus_publish(&e);
  log_info("record", "stop segments=%d frames=%d", segs, frames);
  return 0;
}

int record_is_running(void)
{
  int running;
  pthread_mutex_lock(&g_rec.lock);
  running = g_rec.mode == RECORD_MODE_CONTINUOUS ||
            (g_rec.mode == RECORD_MODE_MOTION &&
             (g_rec.muxer_open || g_rec.seg_active || g_rec.wq_count > 0 || g_rec.mux_writing));
  pthread_mutex_unlock(&g_rec.lock);
  return running;
}

RecordMode record_mode(void)
{
  return g_rec.mode;
}

int record_arm_motion(void)
{
  if (!g_rec.inited) {
    return -1;
  }
  pthread_mutex_lock(&g_rec.lock);
  g_rec.mode = RECORD_MODE_MOTION;
  g_rec.run_deadline = 0;
  g_rec.quiet_deadline = 0;
  close_segment_locked();
  pthread_mutex_unlock(&g_rec.lock);
  /* Avoid RequestIDR here — it can hang after AOV mem resume on this board. */
  log_info("record", "arm motion mode pre=%ds quiet=%ds", g_rec.cfg.pre_record_sec,
           g_rec.cfg.quiet_sec);
  return 0;
}

int record_on_motion(void)
{
  Event e;
  int started = 0;
  if (!g_rec.inited) {
    return -1;
  }
  pthread_mutex_lock(&g_rec.lock);
  if (g_rec.mode != RECORD_MODE_MOTION) {
    g_rec.mode = RECORD_MODE_MOTION;
  }
  g_rec.quiet_deadline = time(NULL) + g_rec.cfg.quiet_sec;
  if (!g_rec.muxer_open) {
    if (open_segment_locked() != 0) {
      pthread_mutex_unlock(&g_rec.lock);
      return -2;
    }
    flush_prerecord_locked();
    started = 1;
  }
  pthread_mutex_unlock(&g_rec.lock);
  if (started) {
    e.type = EVT_RECORD_START;
    e.data = NULL;
    e.data_len = 0;
    event_bus_publish(&e);
    log_info("record", "motion -> RECORDING");
  } else {
    log_info("record", "motion extend quiet -> %ld", (long)g_rec.quiet_deadline);
  }
  return 0;
}

int record_extend_recording(int sec)
{
  if (!g_rec.inited || sec <= 0) {
    return -1;
  }
  pthread_mutex_lock(&g_rec.lock);
  g_rec.quiet_deadline = time(NULL) + sec;
  pthread_mutex_unlock(&g_rec.lock);
  log_info("record", "extend recording %ds (deadline %ld)", sec, (long)g_rec.quiet_deadline);
  return 0;
}

void record_poll_quiet_timeout(void)
{
  time_t now;
  Event e;

  if (!g_rec.inited) {
    return;
  }
  pthread_mutex_lock(&g_rec.lock);
  if (g_rec.mode != RECORD_MODE_MOTION || !g_rec.muxer_open) {
    pthread_mutex_unlock(&g_rec.lock);
    return;
  }
  now = time(NULL);
  if (g_rec.quiet_deadline > 0 && now >= g_rec.quiet_deadline) {
    close_segment_locked();
    e.type = EVT_RECORD_STOP;
    e.data = NULL;
    e.data_len = 0;
    pthread_mutex_unlock(&g_rec.lock);
    event_bus_publish(&e);
    log_info("record", "motion quiet -> idle (poll)");
    return;
  }
  pthread_mutex_unlock(&g_rec.lock);
}

int record_frame_count(void)
{
  int n;
  pthread_mutex_lock(&g_rec.lock);
  n = g_rec.seg_frames > 0 ? g_rec.seg_frames : g_rec.frames;
  pthread_mutex_unlock(&g_rec.lock);
  return n;
}

void record_set_main_fps(int fps)
{
  if (!g_rec.inited || fps <= 0) {
    return;
  }
  pthread_mutex_lock(&g_rec.lock);
  g_rec.cfg.main_fps = fps;
  pthread_mutex_unlock(&g_rec.lock);
}

int record_run_for(int seconds)
{
  if (seconds <= 0) {
    seconds = 30;
  }
  if (record_start() != 0) {
    return -1;
  }
  pthread_mutex_lock(&g_rec.lock);
  g_rec.run_deadline = time(NULL) + seconds;
  pthread_mutex_unlock(&g_rec.lock);

  int left = seconds + 3;
  while (left-- > 0) {
    sleep(1);
    if (g_rec.mode == RECORD_MODE_OFF && g_rec.segments > 0) {
      break;
    }
  }
  record_stop();
  return (g_rec.segments > 0 && g_rec.frames > 0) ? 0 : -2;
}

typedef struct {
  RecordItem *out;
  int max_items;
  int count;
  time_t from;
  time_t to;
} QueryCtx;

static void walk_query(const char *dir, QueryCtx *q)
{
  DIR *dp = opendir(dir);
  struct dirent *de;
  if (!dp) {
    return;
  }
  while ((de = readdir(dp)) != NULL) {
    char path[512];
    struct stat st;
    size_t n;
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
      continue;
    }
    snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
    if (stat(path, &st) != 0) {
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      walk_query(path, q);
      continue;
    }
    n = strlen(de->d_name);
    if (n < 4 || strcmp(de->d_name + n - 4, ".mp4") != 0) {
      continue;
    }
    if (st.st_mtime < q->from || (q->to > 0 && st.st_mtime > q->to)) {
      continue;
    }
    if (q->count < q->max_items) {
      snprintf(q->out[q->count].path, sizeof(q->out[q->count].path), "%s", path);
      q->out[q->count].start_ts = st.st_mtime;
      q->out[q->count].size_bytes = (int64_t)st.st_size;
    }
    q->count++;
  }
  closedir(dp);
}

int record_query(time_t from, time_t to, RecordItem *out, int max_items, int *out_count)
{
  QueryCtx q;
  if (!out || max_items <= 0 || !out_count) {
    return -1;
  }
  memset(&q, 0, sizeof(q));
  q.out = out;
  q.max_items = max_items;
  q.from = from;
  q.to = to > 0 ? to : time(NULL);
  walk_query(storage_records_path(), &q);
  *out_count = q.count > max_items ? max_items : q.count;
  log_info("record", "query matched=%d returned=%d", q.count, *out_count);
  return 0;
}
