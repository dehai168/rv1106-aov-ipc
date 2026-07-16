#include "record/record_service.h"

#include "common/event_bus.h"
#include "common/log.h"
#include "media/media_service.h"
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

typedef struct {
  uint8_t *data;
  uint32_t len;
  int key;
  int64_t pts;
  time_t wall;
} RingPkt;

typedef struct {
  int inited;
  RecordMode mode;
  int waiting_key;
  int muxer_open;
  int flushing_prerecord;
  RecordConfig cfg;
  char cur_path[512];
  time_t seg_start;
  time_t run_deadline;  /* continuous mode */
  time_t quiet_deadline; /* motion mode */
  pthread_mutex_t lock;
  int segments;
  int frames;
  int listener_added;
  RingPkt ring[RING_CAP];
  int ring_count;
  size_t ring_bytes;
} RecordCtx;

static RecordCtx g_rec = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

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
    rkmuxer_deinit(MUXER_ID);
    g_rec.muxer_open = 0;
  }

  ret = rkmuxer_init(MUXER_ID, NULL, g_rec.cur_path, &vp, NULL);
  if (ret != 0) {
    log_error("record", "rkmuxer_init failed %d path=%s", ret, g_rec.cur_path);
    return -2;
  }
  g_rec.muxer_open = 1;
  g_rec.seg_start = now;
  g_rec.waiting_key = 1;
  g_rec.segments++;
  log_info("record", "segment open #%d %s", g_rec.segments, g_rec.cur_path);

  if (storage_free_percent() >= 0 && storage_free_percent() < g_rec.cfg.recycle_free_percent) {
    storage_recycle_oldest(g_rec.cfg.recycle_free_percent);
  }
  return 0;
}

static void close_segment_locked(void)
{
  if (g_rec.muxer_open) {
    rkmuxer_deinit(MUXER_ID);
    g_rec.muxer_open = 0;
    log_info("record", "segment close %s", g_rec.cur_path);
  }
}

static void write_frame_locked(const uint8_t *data, uint32_t len, int key, int64_t pts)
{
  if (!g_rec.muxer_open) {
    return;
  }
  if (g_rec.waiting_key && !key) {
    return;
  }
  g_rec.waiting_key = 0;
  rkmuxer_write_video_frame(MUXER_ID, (unsigned char *)data, len, pts, key);
  g_rec.frames++;
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
  g_rec.waiting_key = 0;
  for (i = start; i < g_rec.ring_count; i++) {
    if (!g_rec.ring[i].data) {
      continue;
    }
    rkmuxer_write_video_frame(MUXER_ID, g_rec.ring[i].data, g_rec.ring[i].len, g_rec.ring[i].pts,
                              g_rec.ring[i].key);
    g_rec.frames++;
  }
  g_rec.flushing_prerecord = 0;
  log_info("record", "flushed prerecord pkts=%d from=%d", g_rec.ring_count - start, start);
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
  if (storage_ensure_dirs() != 0) {
    return -2;
  }
  storage_cleanup_bad_files();
  g_rec.inited = 1;
  g_rec.mode = RECORD_MODE_OFF;
  g_rec.segments = 0;
  g_rec.frames = 0;
  ring_clear();
  if (!g_rec.listener_added) {
    if (media_stream_add_packet_listener(on_packet, NULL) != 0) {
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
  return g_rec.mode == RECORD_MODE_CONTINUOUS ||
         (g_rec.mode == RECORD_MODE_MOTION && g_rec.muxer_open);
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
  media_stream_request_idr(0);
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
