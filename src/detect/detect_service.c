#include "detect/detect_service.h"

#include "common/event_bus.h"
#include "common/log.h"
#include "media/media_service.h"
#include "system/config_service.h"
#include "system/storage_service.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "rk_comm_ivs.h"
#include "rk_defines.h"
#include "rk_mpi_ivs.h"
#include "rk_mpi_sys.h"

#define IVS_CHN 0
#define VI_DETECT_CHN 2

typedef struct {
  int inited;
  int running;
  int ivs_up;
  DetectConfig cfg;
  int width;
  int height;
  pthread_t tid;
  volatile int quit;
  DetectMotionCb cb;
  void *cb_user;
  int motion_count;
  int hit_streak;
  DetectEvent last;
  pthread_mutex_t lock;
} DetectCtx;

static DetectCtx g_det = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static int ensure_alarms_dir(char *dir, size_t n)
{
  snprintf(dir, n, "%s/alarms", storage_mount_path());
  struct stat st;
  if (stat(dir, &st) == 0) {
    return S_ISDIR(st.st_mode) ? 0 : -1;
  }
  if (mkdir(dir, 0755) != 0) {
    return -1;
  }
  return 0;
}

static void append_alarm(const DetectEvent *ev)
{
  char dir[160];
  char path[192];
  FILE *fp;
  if (ensure_alarms_dir(dir, sizeof(dir)) != 0) {
    return;
  }
  snprintf(path, sizeof(path), "%s/alarms.log", dir);
  fp = fopen(path, "a");
  if (!fp) {
    return;
  }
  fprintf(fp,
          "{\"ts\":%ld,\"square\":%d,\"pct_x10\":%d,\"rect\":[%d,%d,%d,%d],\"snapshot\":\"%s\"}\n",
          (long)ev->ts, ev->square, ev->square_pct_x10, ev->rect_x, ev->rect_y, ev->rect_w,
          ev->rect_h, ev->snapshot[0] ? ev->snapshot : "");
  fclose(fp);
}

static int rect_intersect(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh)
{
  int ax2 = ax + aw, ay2 = ay + ah, bx2 = bx + bw, by2 = by + bh;
  int ix1 = ax > bx ? ax : bx;
  int iy1 = ay > by ? ay : by;
  int ix2 = ax2 < bx2 ? ax2 : bx2;
  int iy2 = ay2 < by2 ? ay2 : by2;
  return (ix2 > ix1 && iy2 > iy1) ? 1 : 0;
}

static int schedule_allows_now(const DetectConfig *cfg)
{
  time_t now = time(NULL);
  struct tm tm_now;
  int min;
  unsigned day_bit;
  if (!cfg->schedule_enabled) {
    return 1;
  }
  localtime_r(&now, &tm_now);
  /* tm_wday: 0=Sun..6=Sat → bit0=Mon..bit6=Sun */
  if (tm_now.tm_wday == 0) {
    day_bit = 1u << 6;
  } else {
    day_bit = 1u << (tm_now.tm_wday - 1);
  }
  if ((cfg->schedule_days & day_bit) == 0) {
    return 0;
  }
  min = tm_now.tm_hour * 60 + tm_now.tm_min;
  if (cfg->schedule_start_min == cfg->schedule_end_min) {
    return 1; /* 24h */
  }
  if (cfg->schedule_start_min < cfg->schedule_end_min) {
    return min >= cfg->schedule_start_min && min < cfg->schedule_end_min;
  }
  /* overnight */
  return min >= cfg->schedule_start_min || min < cfg->schedule_end_min;
}

static int region_allows(const DetectConfig *cfg, const DetectEvent *ev)
{
  if (!cfg->region_enabled || cfg->region_w <= 0 || cfg->region_h <= 0) {
    return 1;
  }
  if (ev->rect_w <= 0 || ev->rect_h <= 0) {
    /* no bbox from IVS: allow (area-based hit already passed) */
    return 1;
  }
  return rect_intersect(ev->rect_x, ev->rect_y, ev->rect_w, ev->rect_h, cfg->region_x,
                        cfg->region_y, cfg->region_w, cfg->region_h);
}

static void take_snapshot(DetectEvent *ev)
{
  char path[256];
  char name[64];
  struct tm tm_now;
  localtime_r(&ev->ts, &tm_now);
  strftime(name, sizeof(name), "%Y%m%d_%H%M%S.jpg", &tm_now);
  (void)storage_ensure_dirs();
  snprintf(path, sizeof(path), "%s/%s", storage_snapshots_path(), name);
  ev->snapshot[0] = '\0';
  if (media_snapshot_jpeg(path) == 0) {
    snprintf(ev->snapshot, sizeof(ev->snapshot), "%s", name);
  }
}

static void fire_motion(DetectEvent *ev)
{
  DetectMotionCb cb;
  void *user;
  Event e;

  take_snapshot(ev);

  pthread_mutex_lock(&g_det.lock);
  g_det.last = *ev;
  g_det.motion_count++;
  cb = g_det.cb;
  user = g_det.cb_user;
  pthread_mutex_unlock(&g_det.lock);

  append_alarm(ev);
  e.type = EVT_MOTION_DETECT;
  e.data = (void *)ev;
  e.data_len = sizeof(*ev);
  event_bus_publish(&e);

  if (cb) {
    cb(ev, user);
  }
  log_info("detect", "MOTION pct_x10=%d square=%d rect=%d,%d %dx%d snap=%s", ev->square_pct_x10,
           ev->square, ev->rect_x, ev->rect_y, ev->rect_w, ev->rect_h,
           ev->snapshot[0] ? ev->snapshot : "-");
}

static int create_ivs(int width, int height, int sensitivity)
{
  IVS_CHN_ATTR_S attr;
  IVS_MD_ATTR_S md;
  int ret;

  memset(&attr, 0, sizeof(attr));
  /* Match rkipc RV1106 path: MD+OD, night mode, interval 5 */
  attr.enMode = IVS_MODE_MD_OD;
  attr.u32PicWidth = (RK_U32)width;
  attr.u32PicHeight = (RK_U32)height;
  attr.enPixelFormat = RK_FMT_YUV420SP;
  attr.s32Gop = 30;
  attr.bSmearEnable = RK_FALSE;
  attr.bWeightpEnable = RK_FALSE;
  attr.bMDEnable = RK_TRUE;
  attr.s32MDInterval = 5;
  attr.bMDNightMode = RK_TRUE;
  attr.u32MDSensibility = (sensitivity <= 1) ? 1 : (sensitivity >= 3 ? 3 : 2);
  attr.bODEnable = RK_TRUE;
  attr.s32ODInterval = 1;
  attr.s32ODPercent = 7;

  ret = RK_MPI_IVS_CreateChn(IVS_CHN, &attr);
  if (ret != RK_SUCCESS) {
    log_error("detect", "IVS_CreateChn fail %x", ret);
    return -1;
  }

  memset(&md, 0, sizeof(md));
  ret = RK_MPI_IVS_GetMdAttr(IVS_CHN, &md);
  if (ret != RK_SUCCESS) {
    RK_MPI_IVS_DestroyChn(IVS_CHN);
    return -2;
  }
  switch (sensitivity) {
  case 0:
    md.s32ThreshSad = 96;
    md.s32ThreshMove = 3;
    md.s32SwitchSad = 2;
    break;
  case 1:
    md.s32ThreshSad = 72;
    md.s32ThreshMove = 2;
    md.s32SwitchSad = 2;
    break;
  case 3:
    /* rkipc default-ish for MIS5001 */
    md.s32ThreshSad = 40;
    md.s32ThreshMove = 2;
    md.s32SwitchSad = 0;
    break;
  case 4:
    md.s32ThreshSad = 24;
    md.s32ThreshMove = 1;
    md.s32SwitchSad = 0;
    break;
  case 2:
  default:
    md.s32ThreshSad = 48;
    md.s32ThreshMove = 2;
    md.s32SwitchSad = 0;
    break;
  }
  md.bFlycatkinFlt = RK_TRUE;
  md.s32ThresDustMove = 3;
  md.s32ThresDustBlk = 3;
  md.s32ThresDustChng = 50;
  ret = RK_MPI_IVS_SetMdAttr(IVS_CHN, &md);
  if (ret != RK_SUCCESS) {
    log_error("detect", "IVS_SetMdAttr fail %x", ret);
    RK_MPI_IVS_DestroyChn(IVS_CHN);
    return -3;
  }
  return 0;
}

static int bind_vi_ivs(void)
{
  MPP_CHN_S src = {.enModId = RK_ID_VI, .s32DevId = 0, .s32ChnId = VI_DETECT_CHN};
  MPP_CHN_S dst = {.enModId = RK_ID_IVS, .s32DevId = 0, .s32ChnId = IVS_CHN};
  int ret = RK_MPI_SYS_Bind(&src, &dst);
  if (ret != RK_SUCCESS) {
    log_error("detect", "bind VI2->IVS fail %x", ret);
  }
  return ret == RK_SUCCESS ? 0 : -1;
}

static void unbind_vi_ivs(void)
{
  MPP_CHN_S src = {.enModId = RK_ID_VI, .s32DevId = 0, .s32ChnId = VI_DETECT_CHN};
  MPP_CHN_S dst = {.enModId = RK_ID_IVS, .s32DevId = 0, .s32ChnId = IVS_CHN};
  RK_MPI_SYS_UnBind(&src, &dst);
}

static void *detect_thread(void *arg)
{
  (void)arg;
  IVS_RESULT_INFO_S results;
  int area = g_det.width * g_det.height;
  if (area <= 0) {
    area = 1;
  }

  int result_n = 0;
  int last_pct = 0;
  time_t last_stat = 0;

  log_info("detect", "thread start %dx%d sens=%d thr_pct=%d hits=%d", g_det.width, g_det.height,
           g_det.cfg.sensitivity, g_det.cfg.square_pct, g_det.cfg.hit_frames);

  while (!g_det.quit) {
    memset(&results, 0, sizeof(results));
    int ret = RK_MPI_IVS_GetResults(IVS_CHN, &results, 1000);
    if (ret != RK_SUCCESS) {
      continue;
    }
    if (results.s32ResultNum >= 1 && results.pstResults) {
      IVS_MD_INFO_S *md = &results.pstResults->stMdInfo;
      int pct = (int)((1000ULL * (unsigned)md->u32Square) / (unsigned)area);
      int hit = (pct > g_det.cfg.square_pct);
      time_t now = time(NULL);
      result_n++;
      last_pct = pct;
      if (last_stat == 0 || now - last_stat >= 5) {
        log_info("detect", "ivs ok n=%d last_pct_x10=%d square=%u rects=%u", result_n, pct,
                 md->u32Square, md->u32RectNum);
        last_stat = now;
      }
      if (!schedule_allows_now(&g_det.cfg)) {
        g_det.hit_streak = 0;
        RK_MPI_IVS_ReleaseResults(IVS_CHN, &results);
        continue;
      }
      if (hit) {
        g_det.hit_streak++;
      } else {
        g_det.hit_streak = 0;
      }
      if (hit && g_det.hit_streak >= g_det.cfg.hit_frames) {
        DetectEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.ts = now;
        ev.square = (int)md->u32Square;
        ev.square_pct_x10 = pct;
        if (md->u32RectNum > 0) {
          ev.rect_x = md->stRect[0].s32X;
          ev.rect_y = md->stRect[0].s32Y;
          ev.rect_w = (int)md->stRect[0].u32Width;
          ev.rect_h = (int)md->stRect[0].u32Height;
        }
        if (region_allows(&g_det.cfg, &ev)) {
          fire_motion(&ev);
        }
        g_det.hit_streak = 0;
      }
    }
    RK_MPI_IVS_ReleaseResults(IVS_CHN, &results);
  }
  log_info("detect", "thread exit results=%d last_pct_x10=%d", result_n, last_pct);
  return NULL;
}

int detect_init(const DetectConfig *cfg)
{
  if (!cfg) {
    return -1;
  }
  if (!media_stream_is_up() || !media_stream_detect_enabled()) {
    log_error("detect", "need stream with VI ch2 (detect_w/h)");
    return -2;
  }
  g_det.cfg = *cfg;
  if (g_det.cfg.sensitivity < 0) {
    g_det.cfg.sensitivity = 0;
  }
  if (g_det.cfg.sensitivity > 4) {
    g_det.cfg.sensitivity = 4;
  }
  if (g_det.cfg.square_pct <= 0) {
    g_det.cfg.square_pct = 8;
  }
  if (g_det.cfg.hit_frames <= 0) {
    g_det.cfg.hit_frames = 2;
  }
  if (g_det.cfg.region_x < 0) {
    g_det.cfg.region_x = 0;
  }
  if (g_det.cfg.region_y < 0) {
    g_det.cfg.region_y = 0;
  }
  if (g_det.cfg.region_w < 0) {
    g_det.cfg.region_w = 0;
  }
  if (g_det.cfg.region_h < 0) {
    g_det.cfg.region_h = 0;
  }
  if (g_det.cfg.schedule_start_min < 0) {
    g_det.cfg.schedule_start_min = 0;
  }
  if (g_det.cfg.schedule_start_min > 1439) {
    g_det.cfg.schedule_start_min = 1439;
  }
  if (g_det.cfg.schedule_end_min < 0) {
    g_det.cfg.schedule_end_min = 0;
  }
  if (g_det.cfg.schedule_end_min > 1440) {
    g_det.cfg.schedule_end_min = 1440;
  }
  g_det.cfg.schedule_days &= 0x7fu;
  g_det.width = media_stream_detect_width();
  g_det.height = media_stream_detect_height();
  g_det.motion_count = 0;
  g_det.hit_streak = 0;
  g_det.inited = 1;
  log_info("detect", "init ok");
  return 0;
}

void detect_deinit(void)
{
  detect_stop();
  g_det.inited = 0;
}

int detect_start(void)
{
  if (!g_det.inited || !g_det.cfg.enabled) {
    return -1;
  }
  if (g_det.running) {
    return 0;
  }
  if (create_ivs(g_det.width, g_det.height, g_det.cfg.sensitivity) != 0) {
    return -2;
  }
  if (bind_vi_ivs() != 0) {
    RK_MPI_IVS_DestroyChn(IVS_CHN);
    return -3;
  }
  g_det.ivs_up = 1;
  g_det.quit = 0;
  if (pthread_create(&g_det.tid, NULL, detect_thread, NULL) != 0) {
    unbind_vi_ivs();
    RK_MPI_IVS_DestroyChn(IVS_CHN);
    g_det.ivs_up = 0;
    return -4;
  }
  g_det.running = 1;
  return 0;
}

int detect_stop(void)
{
  if (!g_det.running) {
    if (g_det.ivs_up) {
      unbind_vi_ivs();
      RK_MPI_IVS_DestroyChn(IVS_CHN);
      g_det.ivs_up = 0;
    }
    return 0;
  }
  g_det.quit = 1;
  pthread_join(g_det.tid, NULL);
  g_det.running = 0;
  unbind_vi_ivs();
  RK_MPI_IVS_DestroyChn(IVS_CHN);
  g_det.ivs_up = 0;
  log_info("detect", "stop motions=%d", g_det.motion_count);
  return 0;
}

int detect_is_running(void)
{
  return g_det.running;
}

void detect_set_motion_cb(DetectMotionCb cb, void *user)
{
  pthread_mutex_lock(&g_det.lock);
  g_det.cb = cb;
  g_det.cb_user = user;
  pthread_mutex_unlock(&g_det.lock);
}

int detect_motion_count(void)
{
  return g_det.motion_count;
}

int detect_last_event(DetectEvent *out)
{
  if (!out) {
    return -1;
  }
  pthread_mutex_lock(&g_det.lock);
  *out = g_det.last;
  pthread_mutex_unlock(&g_det.lock);
  return 0;
}

void detect_config_load(DetectConfig *out)
{
  if (!out) {
    return;
  }
  memset(out, 0, sizeof(*out));
  config_get_bool("detect.enabled", &out->enabled, 1);
  config_get_int("detect.sensitivity", &out->sensitivity, 2);
  config_get_int("detect.square_pct", &out->square_pct, 8);
  config_get_int("detect.hit_frames", &out->hit_frames, 2);
  config_get_bool("detect.region.enabled", &out->region_enabled, 0);
  config_get_int("detect.region.x", &out->region_x, 0);
  config_get_int("detect.region.y", &out->region_y, 0);
  config_get_int("detect.region.w", &out->region_w, 0);
  config_get_int("detect.region.h", &out->region_h, 0);
  config_get_bool("detect.schedule.enabled", &out->schedule_enabled, 0);
  config_get_int("detect.schedule.start_min", &out->schedule_start_min, 0);
  config_get_int("detect.schedule.end_min", &out->schedule_end_min, 1440);
  {
    int days = 0x7f;
    config_get_int("detect.schedule.days", &days, 0x7f);
    out->schedule_days = (unsigned)days & 0x7fu;
  }
}

int detect_config_save(const DetectConfig *cfg)
{
  if (!cfg) {
    return -1;
  }
  if (config_set_bool("detect.enabled", cfg->enabled) != 0 ||
      config_set_int("detect.sensitivity", cfg->sensitivity) != 0 ||
      config_set_int("detect.square_pct", cfg->square_pct) != 0 ||
      config_set_int("detect.hit_frames", cfg->hit_frames) != 0 ||
      config_set_bool("detect.region.enabled", cfg->region_enabled) != 0 ||
      config_set_int("detect.region.x", cfg->region_x) != 0 ||
      config_set_int("detect.region.y", cfg->region_y) != 0 ||
      config_set_int("detect.region.w", cfg->region_w) != 0 ||
      config_set_int("detect.region.h", cfg->region_h) != 0 ||
      config_set_bool("detect.schedule.enabled", cfg->schedule_enabled) != 0 ||
      config_set_int("detect.schedule.start_min", cfg->schedule_start_min) != 0 ||
      config_set_int("detect.schedule.end_min", cfg->schedule_end_min) != 0 ||
      config_set_int("detect.schedule.days", (int)cfg->schedule_days) != 0 ||
      config_save() != 0) {
    return -1;
  }
  return 0;
}
