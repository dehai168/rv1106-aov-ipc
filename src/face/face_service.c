#include "face/face_service.h"

#include "common/event_bus.h"
#include "common/jpeg_reader.h"
#include "common/jpeg_writer.h"
#include "common/log.h"
#include "detect/detect_service.h"
#include "face/rknn_face.h"
#include "media/media_service.h"
#include "record/record_service.h"
#include "system/config_service.h"
#include "system/storage_service.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define FACE_TRACK_MAX 16
#define FACE_JPEG_BUF (256 * 1024)
#define FACE_DEFAULT_QUOTA_PERCENT 5

typedef struct {
  float x, y, w, h; /* normalized on grab frame */
  float score;
  int confirm;
  int64_t last_snap_ms;
  int alive;
} FaceTrack;

static FaceConfig g_cfg;
static RknnFaceCtx *g_rknn;
static pthread_t g_thr;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_run;
static int g_thr_on;
static int64_t g_last_motion_ms;
static int g_motion_armed; /* set only by face_notify_motion */
static int64_t g_last_infer_ms;
static int g_snap_count;
static int g_infer_count;
static int g_grab_fail;
static int g_daily_count;
static int g_daily_yday = -1;
static FaceEvent g_last;
static FaceTrack g_tracks[FACE_TRACK_MAX];

static int64_t now_ms(void)
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static int ensure_dir(const char *path)
{
  struct stat st;
  if (stat(path, &st) == 0) return S_ISDIR(st.st_mode) ? 0 : -1;
  if (mkdir(path, 0755) != 0 && errno != EEXIST) return -1;
  return 0;
}

int face_config_defaults(FaceConfig *cfg)
{
  int i;
  if (!cfg) return -1;
  memset(cfg, 0, sizeof(*cfg));
  cfg->enabled = 0;
  snprintf(cfg->model_path, sizeof(cfg->model_path), "/mnt/sdcard/models/yunet_320.rknn");
  cfg->interval_ms = 500;
  cfg->hold_ms = 2000;
  cfg->cooldown_ms = 3000;
  cfg->confirm_frames = 1; /* was 2; flaky dets rarely confirm twice */
  cfg->score_thr = 0.45f;
  cfg->min_face_px = 32;
  cfg->max_faces = 8;
  cfg->max_snap_per_frame = 2;
  cfg->margin = 1.4f;
  cfg->face_jpeg_side = 320;
  cfg->save_scene = 1;
  cfg->link_record = 1;
  cfg->link_alarm = 1;
  cfg->quota_percent = FACE_DEFAULT_QUOTA_PERCENT;
  cfg->daily_limit = 2000;
  snprintf(cfg->sensitivity, sizeof(cfg->sensitivity), "normal");
  for (i = 0; i < FACE_MAX_REGIONS; i++) cfg->regions[i].enabled = 0;
  memset(cfg->schedule_grid, '1', FACE_SCHEDULE_GRID_LEN);
  cfg->schedule_grid[FACE_SCHEDULE_GRID_LEN] = '\0';
  return 0;
}

/* sqrt(cls*obj): the INT8 export clips obj at 0.5, so real faces score ~0.5-0.7
 * and background / low-light clutter stays below ~0.4. */
void face_apply_sensitivity(FaceConfig *cfg)
{
  if (!cfg) return;
  if (strcmp(cfg->sensitivity, "high") == 0) {
    cfg->score_thr = 0.40f;
    cfg->min_face_px = 32;
  } else if (strcmp(cfg->sensitivity, "low") == 0) {
    cfg->score_thr = 0.50f;
    cfg->min_face_px = 56;
  } else {
    cfg->score_thr = 0.45f;
    cfg->min_face_px = 40;
    snprintf(cfg->sensitivity, sizeof(cfg->sensitivity), "normal");
  }
}

void face_config_load(FaceConfig *out)
{
  char key[64];
  int i;
  int score_x100 = 55;
  int margin_x100 = 140;
  face_config_defaults(out);
  config_get_bool("face.enabled", &out->enabled, out->enabled);
  config_get_string("face.model_path", out->model_path, (int)sizeof(out->model_path),
                    out->model_path);
  config_get_int("face.interval_ms", &out->interval_ms, out->interval_ms);
  config_get_int("face.hold_ms", &out->hold_ms, out->hold_ms);
  config_get_int("face.cooldown_ms", &out->cooldown_ms, out->cooldown_ms);
  config_get_int("face.confirm_frames", &out->confirm_frames, out->confirm_frames);
  config_get_int("face.min_face_px", &out->min_face_px, out->min_face_px);
  config_get_int("face.max_faces", &out->max_faces, out->max_faces);
  config_get_int("face.max_snap_per_frame", &out->max_snap_per_frame, out->max_snap_per_frame);
  config_get_int("face.face_jpeg_side", &out->face_jpeg_side, out->face_jpeg_side);
  config_get_bool("face.save_scene", &out->save_scene, out->save_scene);
  config_get_bool("face.link_record", &out->link_record, out->link_record);
  config_get_bool("face.link_alarm", &out->link_alarm, out->link_alarm);
  config_get_int("face.quota_percent", &out->quota_percent, out->quota_percent);
  config_get_int("face.daily_limit", &out->daily_limit, out->daily_limit);
  config_get_string("face.sensitivity", out->sensitivity, (int)sizeof(out->sensitivity),
                    out->sensitivity);
  config_get_int("face.score_thr_x100", &score_x100, 55);
  config_get_int("face.margin_x100", &margin_x100, 140);
  out->score_thr = score_x100 / 100.f;
  out->margin = margin_x100 / 100.f;
  config_get_bool("face.schedule.enabled", &out->schedule_enabled, 0);
  config_get_string("face.schedule.grid", out->schedule_grid, (int)sizeof(out->schedule_grid),
                    out->schedule_grid);
  for (i = 0; i < FACE_MAX_REGIONS; i++) {
    snprintf(key, sizeof(key), "face.region%d.enabled", i);
    config_get_bool(key, &out->regions[i].enabled, 0);
    snprintf(key, sizeof(key), "face.region%d.x", i);
    config_get_int(key, &out->regions[i].x, 0);
    snprintf(key, sizeof(key), "face.region%d.y", i);
    config_get_int(key, &out->regions[i].y, 0);
    snprintf(key, sizeof(key), "face.region%d.w", i);
    config_get_int(key, &out->regions[i].w, 0);
    snprintf(key, sizeof(key), "face.region%d.h", i);
    config_get_int(key, &out->regions[i].h, 0);
  }
  face_apply_sensitivity(out);
}

int face_config_save(const FaceConfig *cfg)
{
  char key[64];
  int i;
  if (!cfg) return -1;
  config_set_bool("face.enabled", cfg->enabled);
  config_set_string("face.model_path", cfg->model_path);
  config_set_string("face.trigger", "motion_gate");
  config_set_string("face.model", "yunet_320");
  config_set_int("face.interval_ms", cfg->interval_ms);
  config_set_int("face.hold_ms", cfg->hold_ms);
  config_set_int("face.cooldown_ms", cfg->cooldown_ms);
  config_set_int("face.confirm_frames", cfg->confirm_frames);
  config_set_int("face.min_face_px", cfg->min_face_px);
  config_set_int("face.max_faces", cfg->max_faces);
  config_set_int("face.max_snap_per_frame", cfg->max_snap_per_frame);
  config_set_int("face.face_jpeg_side", cfg->face_jpeg_side);
  config_set_bool("face.save_scene", cfg->save_scene);
  config_set_bool("face.link_record", cfg->link_record);
  config_set_bool("face.link_alarm", cfg->link_alarm);
  config_set_int("face.quota_percent", cfg->quota_percent);
  config_set_int("face.daily_limit", cfg->daily_limit);
  config_set_string("face.sensitivity", cfg->sensitivity);
  config_set_int("face.score_thr_x100", (int)(cfg->score_thr * 100.f + 0.5f));
  config_set_int("face.margin_x100", (int)(cfg->margin * 100.f + 0.5f));
  config_set_bool("face.schedule.enabled", cfg->schedule_enabled);
  config_set_string("face.schedule.grid", cfg->schedule_grid);
  for (i = 0; i < FACE_MAX_REGIONS; i++) {
    snprintf(key, sizeof(key), "face.region%d.enabled", i);
    config_set_bool(key, cfg->regions[i].enabled);
    snprintf(key, sizeof(key), "face.region%d.x", i);
    config_set_int(key, cfg->regions[i].x);
    snprintf(key, sizeof(key), "face.region%d.y", i);
    config_set_int(key, cfg->regions[i].y);
    snprintf(key, sizeof(key), "face.region%d.w", i);
    config_set_int(key, cfg->regions[i].w);
    snprintf(key, sizeof(key), "face.region%d.h", i);
    config_set_int(key, cfg->regions[i].h);
  }
  return config_save();
}

static int schedule_allows_now(const FaceConfig *cfg)
{
  time_t t = time(NULL);
  struct tm tm;
  int dow, hour, idx;
  if (!cfg->schedule_enabled) return 1;
  if ((int)strlen(cfg->schedule_grid) != FACE_SCHEDULE_GRID_LEN) return 1;
  localtime_r(&t, &tm);
  dow = (tm.tm_wday + 6) % 7; /* Mon=0 */
  hour = tm.tm_hour;
  idx = dow * 24 + hour;
  return cfg->schedule_grid[idx] == '1';
}

/* regions: same as motion UI — x/y/w/h in 640×360 logic pixels. Face boxes are 0..1. */
static int region_allows(const FaceConfig *cfg, float nx, float ny, float nw, float nh)
{
  int i, any = 0;
  float cx = (nx + nw * 0.5f) * 640.f;
  float cy = (ny + nh * 0.5f) * 360.f;
  for (i = 0; i < FACE_MAX_REGIONS; i++) {
    float rx, ry, rw, rh;
    if (!cfg->regions[i].enabled || cfg->regions[i].w <= 0 || cfg->regions[i].h <= 0) continue;
    any = 1;
    rx = (float)cfg->regions[i].x;
    ry = (float)cfg->regions[i].y;
    rw = (float)cfg->regions[i].w;
    rh = (float)cfg->regions[i].h;
    if (cx >= rx && cy >= ry && cx <= rx + rw && cy <= ry + rh) return 1;
  }
  return any ? 0 : 1;
}

static float iou_norm(float ax, float ay, float aw, float ah, float bx, float by, float bw,
                      float bh)
{
  float x1 = ax > bx ? ax : bx;
  float y1 = ay > by ? ay : by;
  float x2 = (ax + aw) < (bx + bw) ? (ax + aw) : (bx + bw);
  float y2 = (ay + ah) < (by + bh) ? (ay + ah) : (by + bh);
  float iw = x2 - x1, ih = y2 - y1;
  float inter, uni;
  if (iw <= 0.f || ih <= 0.f) return 0.f;
  inter = iw * ih;
  uni = aw * ah + bw * bh - inter;
  return uni > 0.f ? inter / uni : 0.f;
}

/* Letterbox RGB24 → BGR320 for YuNet (same geometry as old NV12 path). */
static void letterbox_rgb_to_bgr320(const uint8_t *rgb, int w, int h, uint8_t *bgr, float *scale,
                                   float *pad_x, float *pad_y)
{
  float s = (float)RKNN_FACE_INPUT / (w > h ? w : h);
  int nw = (int)(w * s + 0.5f);
  int nh = (int)(h * s + 0.5f);
  int px = (RKNN_FACE_INPUT - nw) / 2;
  int py = (RKNN_FACE_INPUT - nh) / 2;
  int i, j;
  *scale = s;
  *pad_x = (float)px;
  *pad_y = (float)py;
  memset(bgr, 0, RKNN_FACE_INPUT * RKNN_FACE_INPUT * 3);
  for (i = 0; i < nh; i++) {
    int sy = (int)(i / s);
    if (sy >= h) sy = h - 1;
    for (j = 0; j < nw; j++) {
      int sx = (int)(j / s);
      const uint8_t *p;
      uint8_t *dst;
      if (sx >= w) sx = w - 1;
      p = rgb + ((size_t)sy * (size_t)w + (size_t)sx) * 3;
      dst = bgr + ((py + i) * RKNN_FACE_INPUT + (px + j)) * 3;
      dst[0] = p[2]; /* B */
      dst[1] = p[1]; /* G */
      dst[2] = p[0]; /* R */
    }
  }
}

static int save_face_crop_rgb(const uint8_t *rgb, int fw, int fh, float nx, float ny, float nw,
                              float nh, float margin, int out_side, const char *path)
{
  int x1, y1, x2, y2, cw, ch, side, i, j;
  float cx = nx + nw * 0.5f;
  float cy = ny + nh * 0.5f;
  float bw = nw * margin;
  float bh = nh * margin;
  uint8_t *crop = NULL;
  uint8_t *jpeg = NULL;
  size_t jlen;
  FILE *fp;
  int rc = -1;

  /* Prefer square crop from the larger side so thin mapped boxes still save. */
  if (bw < bh) bw = bh;
  else bh = bw;

  x1 = (int)((cx - bw * 0.5f) * fw);
  y1 = (int)((cy - bh * 0.5f) * fh);
  x2 = (int)((cx + bw * 0.5f) * fw);
  y2 = (int)((cy + bh * 0.5f) * fh);
  if (x1 < 0) x1 = 0;
  if (y1 < 0) y1 = 0;
  if (x2 > fw) x2 = fw;
  if (y2 > fh) y2 = fh;
  cw = x2 - x1;
  ch = y2 - y1;
  if (cw < 8 || ch < 8) return -1;

  side = out_side > 0 ? out_side : cw;
  if (side > cw && side > ch) side = cw > ch ? cw : ch;
  if (side < 8) side = cw > ch ? ch : cw;

  crop = (uint8_t *)malloc((size_t)side * (size_t)side * 3);
  jpeg = (uint8_t *)malloc(FACE_JPEG_BUF);
  if (!crop || !jpeg) goto done;

  for (i = 0; i < side; i++) {
    int sy = y1 + i * ch / side;
    if (sy >= fh) sy = fh - 1;
    for (j = 0; j < side; j++) {
      int sx = x1 + j * cw / side;
      const uint8_t *p;
      if (sx >= fw) sx = fw - 1;
      p = rgb + ((size_t)sy * (size_t)fw + (size_t)sx) * 3;
      memcpy(crop + (i * side + j) * 3, p, 3);
    }
  }

  jlen = jpeg_write_rgb(crop, side, side, side * 3, jpeg, FACE_JPEG_BUF, 85);
  if (jlen == 0) goto done;
  fp = fopen(path, "wb");
  if (!fp) goto done;
  if (fwrite(jpeg, 1, jlen, fp) != jlen) {
    fclose(fp);
    unlink(path);
    goto done;
  }
  fclose(fp);
  rc = 0;
done:
  free(crop);
  free(jpeg);
  return rc;
}

static int write_buf_file(const char *path, const uint8_t *buf, size_t len)
{
  FILE *fp;
  if (!path || !buf || len == 0) return -1;
  fp = fopen(path, "wb");
  if (!fp) return -1;
  if (fwrite(buf, 1, len, fp) != len) {
    fclose(fp);
    unlink(path);
    return -1;
  }
  fclose(fp);
  return 0;
}

static void append_face_alarm(const FaceEvent *ev)
{
  char path[256];
  FILE *fp;
  snprintf(path, sizeof(path), "%s/alarms", storage_mount_path());
  ensure_dir(path);
  snprintf(path, sizeof(path), "%s/alarms/alarms.log", storage_mount_path());
  fp = fopen(path, "a");
  if (!fp) return;
  fprintf(fp,
          "{\"ts\":%ld,\"type\":\"face\",\"score\":%.3f,\"box\":[%.4f,%.4f,%.4f,%.4f],"
          "\"face_file\":\"%s\",\"scene_file\":\"%s\"}\n",
          (long)ev->ts, ev->score, ev->x, ev->y, ev->w, ev->h, ev->face_file, ev->scene_file);
  fclose(fp);
}

static void on_bus_motion(const Event *ev, void *user)
{
  (void)ev;
  (void)user;
  face_notify_motion();
}

void face_notify_motion(void)
{
  pthread_mutex_lock(&g_lock);
  g_last_motion_ms = now_ms();
  g_motion_armed = 1;
  pthread_mutex_unlock(&g_lock);
}

static void *face_thread(void *arg)
{
  uint8_t *bgr = NULL;
  RknnFaceBox boxes[RKNN_FACE_MAX_DET];
  (void)arg;

  bgr = (uint8_t *)malloc((size_t)RKNN_FACE_INPUT * RKNN_FACE_INPUT * 3);
  if (!bgr) {
    log_error("face", "oom bgr");
    return NULL;
  }

  while (g_run) {
    int64_t tnow;
    int gate, n = 0, i, snapped = 0, fw = 0, fh = 0;
    float scale, pad_x, pad_y;
    FaceConfig cfg;
    uint8_t *jpeg = NULL;
    uint8_t *rgb = NULL;
    size_t jlen = 0;
    /* One timestamp per grabbed frame: names its crops and its panorama. */
    struct timeval frame_tv;
    struct tm frame_tm;
    char frame_stamp[32];
    char scenename[64];
    int scene_tried = 0;

    pthread_mutex_lock(&g_lock);
    cfg = g_cfg;
    tnow = now_ms();
    gate = g_motion_armed && (tnow - g_last_motion_ms) >= 0 &&
           (tnow - g_last_motion_ms) <= cfg.hold_ms;
    if (!cfg.enabled || !gate) {
      pthread_mutex_unlock(&g_lock);
      usleep(50 * 1000);
      continue;
    }
    if (!g_rknn) {
      char path[256];
      snprintf(path, sizeof(path), "%s", g_cfg.model_path);
      pthread_mutex_unlock(&g_lock);
      {
        RknnFaceCtx *ctx = rknn_face_create(path);
        pthread_mutex_lock(&g_lock);
        if (!g_rknn && ctx) {
          g_rknn = ctx;
          log_info("face", "rknn lazy-loaded %s", path);
        } else if (ctx) {
          rknn_face_destroy(ctx);
        }
        if (!g_rknn) {
          pthread_mutex_unlock(&g_lock);
          log_warn("face", "rknn load failed");
          usleep(500 * 1000);
          continue;
        }
        pthread_mutex_unlock(&g_lock);
      }
      pthread_mutex_lock(&g_lock);
      cfg = g_cfg;
    }
    if (tnow - g_last_infer_ms < cfg.interval_ms) {
      pthread_mutex_unlock(&g_lock);
      usleep(20 * 1000);
      continue;
    }
    g_last_infer_ms = tnow;
    pthread_mutex_unlock(&g_lock);

    if (!schedule_allows_now(&cfg)) {
      usleep(100 * 1000);
      continue;
    }
    if (!media_stream_is_up()) {
      usleep(100 * 1000);
      continue;
    }

    /* VI GetChnFrame/RGA NV12 is unusable on this board — use HW JPEG (VENC combo). */
    gettimeofday(&frame_tv, NULL);
    localtime_r(&frame_tv.tv_sec, &frame_tm);
    strftime(frame_stamp, sizeof(frame_stamp), "%Y%m%d_%H%M%S", &frame_tm);
    snprintf(frame_stamp + 15, sizeof(frame_stamp) - 15, "_%03d",
             (int)((frame_tv.tv_usec / 1000) % 1000));
    scenename[0] = '\0';
    if (media_snapshot_jpeg_mem(&jpeg, &jlen) != 0 || !jpeg) {
      g_grab_fail++;
      if ((g_grab_fail % 20) == 1) {
        log_warn("face", "hw jpeg snapshot failed n=%d", g_grab_fail);
      }
      usleep(50 * 1000);
      continue;
    }
    if (jpeg_decode_rgb(jpeg, jlen, &rgb, &fw, &fh) != 0 || !rgb) {
      g_grab_fail++;
      if ((g_grab_fail % 20) == 1) {
        log_warn("face", "jpeg decode failed n=%d len=%zu", g_grab_fail, jlen);
      }
      free(jpeg);
      usleep(50 * 1000);
      continue;
    }
    {
      static int dbg_dumped;
      if (!dbg_dumped) {
        dbg_dumped = 1;
        if (write_buf_file("/mnt/sdcard/faces/_dbg_scene.jpg", jpeg, jlen) == 0) {
          log_info("face", "debug scene %dx%d (%zu bytes) -> _dbg_scene.jpg", fw, fh, jlen);
        }
      }
    }

    letterbox_rgb_to_bgr320(rgb, fw, fh, bgr, &scale, &pad_x, &pad_y);
    g_infer_count++;
    if (rknn_face_infer(g_rknn, bgr, cfg.score_thr, 0.4f, boxes, cfg.max_faces, &n) != 0) {
      if ((g_infer_count % 10) == 1) log_warn("face", "infer failed count=%d", g_infer_count);
      free(jpeg);
      free(rgb);
      continue;
    }
    if ((g_infer_count % 10) == 1) {
      log_info("face", "infer ok n=%d boxes=%d snap_fail=%d snaps=%d %dx%d", g_infer_count, n,
               g_grab_fail, g_snap_count, fw, fh);
    }

    for (i = 0; i < n && snapped < cfg.max_snap_per_frame; i++) {
      float x1 = (boxes[i].x1 - pad_x) / scale;
      float y1 = (boxes[i].y1 - pad_y) / scale;
      float x2 = (boxes[i].x2 - pad_x) / scale;
      float y2 = (boxes[i].y2 - pad_y) / scale;
      float nx, ny, nw, nh;
      float face_px, face_py;
      int ti, best = -1;
      float best_iou = 0.f;
      char daydir[256], facepath[320], scenepath[320];
      struct tm tm;
      FaceEvent ev;

      /* Box fully in letterbox pad → maps to empty/degenerate image ROI. */
      if (boxes[i].y2 <= pad_y + 1.f || boxes[i].x2 <= pad_x + 1.f ||
          boxes[i].y1 >= (float)RKNN_FACE_INPUT - pad_y - 1.f) {
        if ((g_infer_count % 10) == 1) {
          log_info("face", "skip pad-box score=%.2f y=%.1f..%.1f pad_y=%.1f", boxes[i].score,
                   boxes[i].y1, boxes[i].y2, pad_y);
        }
        continue;
      }
      if (x1 < 0) x1 = 0;
      if (y1 < 0) y1 = 0;
      if (x2 > fw) x2 = (float)fw;
      if (y2 > fh) y2 = (float)fh;
      nw = (x2 - x1) / (float)fw;
      nh = (y2 - y1) / (float)fh;
      nx = x1 / (float)fw;
      ny = y1 / (float)fh;
      face_px = (x2 - x1);
      face_py = (y2 - y1);
      if (face_px < (float)cfg.min_face_px && face_py < (float)cfg.min_face_px) {
        if ((g_infer_count % 10) == 1) {
          log_info("face", "skip tiny score=%.2f %.0fx%.0f < %d", boxes[i].score, face_px, face_py,
                   cfg.min_face_px);
        }
        continue;
      }
      if (!region_allows(&cfg, nx, ny, nw, nh)) continue;

      for (ti = 0; ti < FACE_TRACK_MAX; ti++) {
        float iou;
        if (!g_tracks[ti].alive) continue;
        iou = iou_norm(nx, ny, nw, nh, g_tracks[ti].x, g_tracks[ti].y, g_tracks[ti].w,
                       g_tracks[ti].h);
        if (iou > best_iou) {
          best_iou = iou;
          best = ti;
        }
      }
      if (best >= 0 && best_iou >= 0.3f) {
        g_tracks[best].x = nx;
        g_tracks[best].y = ny;
        g_tracks[best].w = nw;
        g_tracks[best].h = nh;
        g_tracks[best].score = boxes[i].score;
        g_tracks[best].confirm++;
      } else {
        for (ti = 0; ti < FACE_TRACK_MAX; ti++) {
          if (!g_tracks[ti].alive) {
            best = ti;
            break;
          }
        }
        if (best < 0) best = 0;
        g_tracks[best].alive = 1;
        g_tracks[best].x = nx;
        g_tracks[best].y = ny;
        g_tracks[best].w = nw;
        g_tracks[best].h = nh;
        g_tracks[best].score = boxes[i].score;
        g_tracks[best].confirm = 1;
        g_tracks[best].last_snap_ms = 0;
      }

      if (g_tracks[best].confirm < cfg.confirm_frames) continue;
      if (g_tracks[best].last_snap_ms &&
          (now_ms() - g_tracks[best].last_snap_ms) < cfg.cooldown_ms) {
        continue;
      }

      pthread_mutex_lock(&g_lock);
      {
        time_t nowt = time(NULL);
        localtime_r(&nowt, &tm);
        if (tm.tm_yday != g_daily_yday) {
          g_daily_yday = tm.tm_yday;
          g_daily_count = 0;
        }
        if (cfg.save_scene && g_daily_count >= cfg.daily_limit) {
          pthread_mutex_unlock(&g_lock);
          continue;
        }
      }
      pthread_mutex_unlock(&g_lock);

      snprintf(daydir, sizeof(daydir), "%s/%.8s", storage_faces_path(), frame_stamp);
      ensure_dir(storage_faces_path());
      ensure_dir(daydir);
      /* daily_limit / quota_percent are 联动抓图 settings; without it the
       * crops still need a cap so the TF card can't fill up. */
      {
        int quota = cfg.save_scene ? cfg.quota_percent : FACE_DEFAULT_QUOTA_PERCENT;
        if (quota > 0) {
          (void)storage_recycle_faces(quota);
        }
      }
      /* Panorama is per frame (shared by its crops); the web layer finds it
       * from the crop name: <stamp>_<k>.jpg -> <stamp>_face.jpg. */
      if (cfg.save_scene && !scene_tried) {
        scene_tried = 1;
        snprintf(scenename, sizeof(scenename), "%s%s", frame_stamp, STORAGE_FACE_SCENE_SUFFIX);
        snprintf(scenepath, sizeof(scenepath), "%s/%s", storage_snapshots_path(), scenename);
        ensure_dir(storage_snapshots_path());
        if (write_buf_file(scenepath, jpeg, jlen) != 0) scenename[0] = '\0';
      }
      snprintf(facepath, sizeof(facepath), "%s/%s_%d.jpg", daydir, frame_stamp, snapped);
      if (save_face_crop_rgb(rgb, fw, fh, nx, ny, nw, nh, cfg.margin, cfg.face_jpeg_side,
                             facepath) != 0) {
        log_warn("face", "crop save failed score=%.2f box=%.3f,%.3f,%.3f,%.3f path=%s",
                 boxes[i].score, nx, ny, nw, nh, facepath);
        continue;
      }

      memset(&ev, 0, sizeof(ev));
      ev.ts = frame_tv.tv_sec;
      ev.score = boxes[i].score;
      ev.x = nx;
      ev.y = ny;
      ev.w = nw;
      ev.h = nh;
      snprintf(ev.face_file, sizeof(ev.face_file), "%.8s/%s_%d.jpg", frame_stamp, frame_stamp,
               snapped);
      snprintf(ev.scene_file, sizeof(ev.scene_file), "%s", scenename);
      if (cfg.link_alarm) append_face_alarm(&ev);
      /* Continuous recording already covers the event; don't flip it to motion mode. */
      if (cfg.link_record && record_mode() != RECORD_MODE_CONTINUOUS) {
        record_on_event(RECORD_SRC_FACE);
      }

      g_tracks[best].last_snap_ms = now_ms();
      pthread_mutex_lock(&g_lock);
      g_last = ev;
      g_snap_count++;
      g_daily_count++;
      pthread_mutex_unlock(&g_lock);
      log_info("face", "snap score=%.2f file=%s scene=%s", ev.score, ev.face_file, ev.scene_file);
      snapped++;
    }
    free(jpeg);
    free(rgb);
  }

  free(bgr);
  return NULL;
}

int face_init(const FaceConfig *cfg)
{
  FaceConfig local;
  if (!cfg) {
    face_config_load(&local);
    cfg = &local;
  }
  face_stop();
  pthread_mutex_lock(&g_lock);
  g_cfg = *cfg;
  face_apply_sensitivity(&g_cfg);
  snprintf(g_cfg.model_path, sizeof(g_cfg.model_path), "%s",
           cfg->model_path[0] ? cfg->model_path : "/mnt/sdcard/models/yunet_320.rknn");
  g_motion_armed = 0;
  g_last_motion_ms = 0;
  pthread_mutex_unlock(&g_lock);

  /* Do not load RKNN here — preview start path used to pin CMA and starve VI/VENC.
   * Model is lazy-loaded on first motion-gated infer. */
  if (g_rknn) {
    rknn_face_destroy(g_rknn);
    g_rknn = NULL;
  }
  event_bus_subscribe(EVT_MOTION_DETECT, on_bus_motion, NULL);
  ensure_dir(storage_faces_path());
  log_info("face", "init ok enabled=%d hold=%dms (rknn lazy)", g_cfg.enabled, g_cfg.hold_ms);
  return 0;
}

void face_deinit(void)
{
  face_stop();
  event_bus_unsubscribe(EVT_MOTION_DETECT, on_bus_motion);
  if (g_rknn) {
    rknn_face_destroy(g_rknn);
    g_rknn = NULL;
  }
}

int face_start(void)
{
  if (g_thr_on) return 0;
  if (!g_cfg.enabled) {
    log_info("face", "start skipped (disabled)");
    return 0;
  }
  g_run = 1;
  if (pthread_create(&g_thr, NULL, face_thread, NULL) != 0) {
    g_run = 0;
    return -2;
  }
  g_thr_on = 1;
  log_info("face", "worker started");
  return 0;
}

int face_stop(void)
{
  if (!g_thr_on) return 0;
  g_run = 0;
  pthread_join(g_thr, NULL);
  g_thr_on = 0;
  log_info("face", "worker stopped snaps=%d", g_snap_count);
  return 0;
}

int face_is_running(void)
{
  return g_thr_on;
}

int face_snap_count(void)
{
  return g_snap_count;
}

int face_infer_count(void)
{
  return g_infer_count;
}

int face_last_event(FaceEvent *out)
{
  if (!out) return -1;
  pthread_mutex_lock(&g_lock);
  *out = g_last;
  pthread_mutex_unlock(&g_lock);
  return g_last.face_file[0] ? 0 : -1;
}
