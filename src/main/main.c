#include "common/event_bus.h"
#include "common/log.h"
#include "detect/detect_service.h"
#include "media/media_service.h"
#include "record/record_service.h"
#include "system/config_service.h"
#include "system/storage_service.h"
#include "system/system_service.h"
#include "web/web_service.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#define LOG_DIR "/userdata/log"
#define DEFAULT_CFG "/userdata/default_config.json"
#define USER_CFG "/userdata/ipc_config.json"

static volatile sig_atomic_t g_running = 1;
static int g_got_start_event;

static void on_signal(int sig)
{
  (void)sig;
  g_running = 0;
}

static void on_app_start(const Event *event, void *user)
{
  (void)user;
  g_got_start_event = 1;
  log_info("main", "event received: APP_START (type=%d)", (int)event->type);
}

static LogLevel parse_level(const char *s)
{
  if (s && strcmp(s, "debug") == 0) {
    return LOG_LEVEL_DEBUG;
  }
  if (s && strcmp(s, "warn") == 0) {
    return LOG_LEVEL_WARN;
  }
  if (s && strcmp(s, "error") == 0) {
    return LOG_LEVEL_ERROR;
  }
  return LOG_LEVEL_INFO;
}

static int file_exists(const char *path)
{
  return access(path, F_OK) == 0;
}

static int app_config_init(void)
{
  if (config_init(DEFAULT_CFG, USER_CFG) != 0) {
    return -1;
  }
  if (system_time_init() != 0) {
    log_warn("main", "system_time_init failed");
  }
  return 0;
}

static void app_config_fini(void)
{
  system_time_deinit();
  config_deinit();
}

static long file_size(const char *path)
{
  struct stat st;
  if (stat(path, &st) != 0) {
    return -1;
  }
  return (long)st.st_size;
}

static void load_encode_cfg(MediaEncodeConfig *ecfg, char *iq_dir, int iq_len)
{
  memset(ecfg, 0, sizeof(*ecfg));
  config_get_string("video.iq_dir", iq_dir, iq_len, "/oem/usr/share/iqfiles");
  config_get_int("video.encode.main_w", &ecfg->main_w, 1920);
  config_get_int("video.encode.main_h", &ecfg->main_h, 1080);
  config_get_int("video.encode.main_fps", &ecfg->main_fps, 15);
  config_get_int("video.encode.main_bitrate_kbps", &ecfg->main_bitrate_kbps, 2048);
  config_get_int("video.encode.main_gop", &ecfg->main_gop, 30);
  config_get_int("video.encode.sub_w", &ecfg->sub_w, 704);
  config_get_int("video.encode.sub_h", &ecfg->sub_h, 576);
  config_get_int("video.encode.sub_fps", &ecfg->sub_fps, 15);
  config_get_int("video.encode.sub_bitrate_kbps", &ecfg->sub_bitrate_kbps, 1024);
  config_get_int("video.encode.sub_gop", &ecfg->sub_gop, 30);
  config_get_int("detect.width", &ecfg->detect_w, 640);
  config_get_int("detect.height", &ecfg->detect_h, 360);
  ecfg->iq_dir = iq_dir;
}

static void on_detect_motion(const DetectEvent *ev, void *user)
{
  (void)user;
  (void)ev;
  record_on_motion();
}

static int run_self_test(void)
{
  int failed = 0;

  log_init_ex(LOG_LEVEL_INFO, LOG_DIR);
  log_info("selftest", "T0.3 self-test begin");

  if (app_config_init() != 0) {
    log_error("selftest", "config_init failed");
    return 1;
  }

  if (config_set_string("device.name", "selftest-device") != 0 ||
      config_set_int("selftest.counter", 42) != 0 ||
      config_set_bool("selftest.enabled", 1) != 0 || config_save() != 0) {
    log_error("selftest", "config set/save failed");
    failed = 1;
  }

  if (config_reload() != 0) {
    log_error("selftest", "config_reload failed");
    failed = 1;
  }

  char name[64];
  int counter = 0;
  int enabled = 0;
  if (config_get_string("device.name", name, (int)sizeof(name), "") != 0 ||
      strcmp(name, "selftest-device") != 0) {
    log_error("selftest", "device.name mismatch: %s", name);
    failed = 1;
  }
  if (config_get_int("selftest.counter", &counter, 0) != 0 || counter != 42) {
    log_error("selftest", "counter mismatch: %d", counter);
    failed = 1;
  }
  if (config_get_bool("selftest.enabled", &enabled, 0) != 0 || enabled != 1) {
    log_error("selftest", "enabled mismatch: %d", enabled);
    failed = 1;
  }

  if (!file_exists(USER_CFG)) {
    log_error("selftest", "user config file missing");
    failed = 1;
  }

  g_got_start_event = 0;
  if (event_bus_init() != 0 || event_bus_subscribe(EVT_APP_START, on_app_start, NULL) != 0) {
    log_error("selftest", "event_bus setup failed");
    failed = 1;
  } else {
    Event ev = {.type = EVT_APP_START, .data = NULL, .data_len = 0};
    event_bus_publish(&ev);
    if (!g_got_start_event) {
      log_error("selftest", "event not received");
      failed = 1;
    }
    event_bus_deinit();
  }

  log_info("selftest", "file log marker");
  if (!file_exists(LOG_DIR "/ipc_app.log")) {
    log_error("selftest", "log file missing");
    failed = 1;
  }

  app_config_fini();
  log_info("selftest", failed ? "FAILED" : "PASSED");
  log_close();
  return failed;
}

static int run_capture(int frame_count)
{
  log_init_ex(LOG_LEVEL_INFO, LOG_DIR);
  if (app_config_init() != 0) {
    log_error("main", "config_init failed");
    return 1;
  }

  char level_str[32];
  char iq_dir[256];
  char capture_dir[256];
  int width = 1920;
  int height = 1080;
  int vi_chn = 0;

  config_get_string("log.level", level_str, (int)sizeof(level_str), "info");
  log_set_level(parse_level(level_str));
  config_get_int("video.width", &width, 1920);
  config_get_int("video.height", &height, 1080);
  config_get_int("video.vi_chn", &vi_chn, 0);
  config_get_string("video.iq_dir", iq_dir, (int)sizeof(iq_dir), "/oem/usr/share/iqfiles");
  config_get_string("video.capture_dir", capture_dir, (int)sizeof(capture_dir), "/mnt/sdcard");

  MediaConfig mcfg = {
      .width = width,
      .height = height,
      .vi_chn = vi_chn,
      .iq_dir = iq_dir,
  };

  log_info("main", "capture start %dx%d iq=%s dir=%s frames=%d", width, height, iq_dir,
           capture_dir, frame_count);
  if (media_init(&mcfg) != 0) {
    log_error("main", "media_init failed");
    app_config_fini();
    log_close();
    return 2;
  }

  int ret = media_capture_nv12(capture_dir, "capture_0.yuv", frame_count);
  media_deinit();
  app_config_fini();

  if (ret != 0) {
    log_error("main", "capture failed %d", ret);
    log_close();
    return 3;
  }
  log_info("main", "capture ok -> %s/capture_0.yuv", capture_dir);
  log_close();
  return 0;
}

static int run_encode(int seconds)
{
  log_init_ex(LOG_LEVEL_INFO, LOG_DIR);
  if (app_config_init() != 0) {
    log_error("main", "config_init failed");
    return 1;
  }

  char level_str[32];
  char iq_dir[256];
  char out_dir[256];
  MediaEncodeConfig ecfg;

  config_get_string("log.level", level_str, (int)sizeof(level_str), "info");
  log_set_level(parse_level(level_str));
  config_get_string("video.encode.out_dir", out_dir, (int)sizeof(out_dir), "/mnt/sdcard");
  load_encode_cfg(&ecfg, iq_dir, (int)sizeof(iq_dir));

  log_info("main", "encode %ds -> %s", seconds, out_dir);
  int ret = media_encode_raw(&ecfg, out_dir, seconds);
  app_config_fini();
  if (ret != 0) {
    log_error("main", "encode failed %d", ret);
    log_close();
    return 4;
  }
  log_info("main", "encode ok");
  log_close();
  return 0;
}

/* T1.3: start stream @2048kbps dump 5s, apply 800kbps dump 5s, compare sizes. */
static int run_reconfig(void)
{
  log_init_ex(LOG_LEVEL_INFO, LOG_DIR);
  if (app_config_init() != 0) {
    log_error("main", "config_init failed");
    return 1;
  }

  char level_str[32];
  char iq_dir[256];
  char out_dir[128];
  MediaEncodeConfig ecfg;
  char hi_path[160];
  char lo_path[160];

  config_get_string("log.level", level_str, (int)sizeof(level_str), "info");
  log_set_level(parse_level(level_str));
  config_get_string("video.encode.out_dir", out_dir, (int)sizeof(out_dir), "/mnt/sdcard");
  load_encode_cfg(&ecfg, iq_dir, (int)sizeof(iq_dir));

  ecfg.main_bitrate_kbps = 2048;
  snprintf(hi_path, sizeof(hi_path), "%s/main_hi.h265", out_dir);
  snprintf(lo_path, sizeof(lo_path), "%s/main_lo.h265", out_dir);

  log_info("main", "reconfig test start");
  if (media_stream_start(&ecfg) != 0) {
    log_error("main", "stream_start failed");
    app_config_fini();
    log_close();
    return 2;
  }

  if (media_stream_set_dump(hi_path, NULL) != 0) {
    media_stream_stop();
    app_config_fini();
    log_close();
    return 3;
  }
  sleep(5);
  media_stream_clear_dump();

  ecfg.main_bitrate_kbps = 800;
  if (media_stream_apply(&ecfg) != 0) {
    log_error("main", "stream_apply failed");
    media_stream_stop();
    app_config_fini();
    log_close();
    return 4;
  }

  if (media_stream_set_dump(lo_path, NULL) != 0) {
    media_stream_stop();
    app_config_fini();
    log_close();
    return 5;
  }
  sleep(5);
  media_stream_clear_dump();
  media_stream_stop();

  long hi = file_size(hi_path);
  long lo = file_size(lo_path);
  log_info("main", "reconfig sizes hi=%ld lo=%ld", hi, lo);
  app_config_fini();

  if (hi < 10000 || lo < 1000 || lo >= hi) {
    log_error("main", "reconfig FAILED: expected lo < hi");
    log_close();
    return 6;
  }
  log_info("main", "reconfig PASSED");
  log_close();
  return 0;
}

static int run_record(int seconds)
{
  log_init_ex(LOG_LEVEL_INFO, LOG_DIR);
  if (app_config_init() != 0) {
    log_error("main", "config_init failed");
    return 1;
  }

  char level_str[32];
  char iq_dir[256];
  char mount[128];
  MediaEncodeConfig ecfg;
  RecordConfig rcfg;
  StorageStatus st;
  RecordItem items[32];
  int nitems = 0;
  int segment_sec = 30;
  int recycle_pct = 10;

  config_get_string("log.level", level_str, (int)sizeof(level_str), "info");
  log_set_level(parse_level(level_str));
  config_get_string("storage.mount", mount, (int)sizeof(mount), "/mnt/sdcard");
  config_get_int("record.segment_sec", &segment_sec, 30);
  config_get_int("record.recycle_free_percent", &recycle_pct, 10);
  load_encode_cfg(&ecfg, iq_dir, (int)sizeof(iq_dir));

  if (storage_init(mount) != 0) {
    log_error("main", "storage_init failed");
    app_config_fini();
    log_close();
    return 2;
  }
  if (storage_get_status(&st) != 0 || !st.mounted) {
    log_error("main", "TF not mounted at %s", mount);
    storage_deinit();
    app_config_fini();
    log_close();
    return 3;
  }
  log_info("main", "storage ok fstype=%s free=%llu/%llu", st.fstype,
           (unsigned long long)st.free_bytes, (unsigned long long)st.total_bytes);

  memset(&rcfg, 0, sizeof(rcfg));
  rcfg.segment_sec = segment_sec;
  rcfg.recycle_free_percent = recycle_pct;
  rcfg.main_w = ecfg.main_w;
  rcfg.main_h = ecfg.main_h;
  rcfg.main_fps = ecfg.main_fps;
  rcfg.main_bitrate_kbps = ecfg.main_bitrate_kbps;
  config_get_int("record.pre_record_sec", &rcfg.pre_record_sec, 4);
  config_get_int("record.quiet_sec", &rcfg.quiet_sec, 30);

  /* continuous CLI record does not need detect VI */
  ecfg.detect_w = 0;
  ecfg.detect_h = 0;

  if (media_stream_start(&ecfg) != 0) {
    log_error("main", "stream_start failed");
    storage_deinit();
    app_config_fini();
    log_close();
    return 4;
  }
  if (record_init(&rcfg) != 0 || record_run_for(seconds) != 0) {
    log_error("main", "record_run_for failed");
    record_deinit();
    media_stream_stop();
    storage_deinit();
    app_config_fini();
    log_close();
    return 5;
  }

  if (record_query(0, 0, items, 32, &nitems) != 0 || nitems < 1) {
    log_error("main", "record_query empty");
    record_deinit();
    media_stream_stop();
    storage_deinit();
    app_config_fini();
    log_close();
    return 6;
  }
  for (int i = 0; i < nitems && i < 5; i++) {
    log_info("main", "record[%d] %s size=%lld", i, items[i].path,
             (long long)items[i].size_bytes);
  }

  /* recycle API smoke: plant ancient tiny mp4 then delete oldest */
  {
    char plant_dir[256];
    char plant_file[288];
    FILE *fp;
    snprintf(plant_dir, sizeof(plant_dir), "%s/20200101", storage_records_path());
    mkdir(plant_dir, 0755);
    snprintf(plant_file, sizeof(plant_file), "%s/20200101_000000.mp4", plant_dir);
    fp = fopen(plant_file, "wb");
    if (fp) {
      const char pad[64] = {0};
      fwrite(pad, 1, sizeof(pad), fp);
      fclose(fp);
      {
        struct utimbuf ub = {.actime = 946684800, .modtime = 946684800}; /* 2000-01-01 */
        utime(plant_file, &ub);
      }
    }
    storage_recycle_oldest(recycle_pct); /* normally 0 deletes when free high */
    if (storage_delete_oldest(1) < 1) {
      log_error("main", "recycle smoke failed to delete planted file");
      record_deinit();
      media_stream_stop();
      storage_deinit();
      app_config_fini();
      log_close();
      return 7;
    }
    log_info("main", "recycle smoke deleted planted oldest file");
  }

  record_deinit();
  media_stream_stop();
  storage_deinit();
  app_config_fini();
  log_info("main", "record ok segments_listed=%d", nitems);
  log_close();
  return 0;
}

/* T3: stream + IVS detect + motion-triggered record. Wave hand in front of camera. */
static int run_detect(int seconds)
{
  log_init_ex(LOG_LEVEL_INFO, LOG_DIR);
  if (app_config_init() != 0) {
    log_error("main", "config_init failed");
    return 1;
  }

  char level_str[32];
  char iq_dir[256];
  char mount[128];
  MediaEncodeConfig ecfg;
  RecordConfig rcfg;
  DetectConfig dcfg;
  StorageStatus st;
  RecordItem items[16];
  int nitems = 0;
  int quiet_sec = 12;
  int enabled = 1;

  config_get_string("log.level", level_str, (int)sizeof(level_str), "info");
  log_set_level(parse_level(level_str));
  config_get_string("storage.mount", mount, (int)sizeof(mount), "/mnt/sdcard");
  load_encode_cfg(&ecfg, iq_dir, (int)sizeof(iq_dir));

  memset(&dcfg, 0, sizeof(dcfg));
  config_get_bool("detect.enabled", &enabled, 1);
  dcfg.enabled = enabled;
  config_get_int("detect.sensitivity", &dcfg.sensitivity, 2);
  config_get_int("detect.square_pct", &dcfg.square_pct, 8);
  config_get_int("detect.hit_frames", &dcfg.hit_frames, 2);

  if (storage_init(mount) != 0) {
    log_error("main", "storage_init failed");
    app_config_fini();
    log_close();
    return 2;
  }
  if (storage_get_status(&st) != 0 || !st.mounted) {
    log_error("main", "TF not mounted");
    storage_deinit();
    app_config_fini();
    log_close();
    return 3;
  }

  if (event_bus_init() != 0) {
    storage_deinit();
    app_config_fini();
    log_close();
    return 4;
  }

  log_info("main", "detect test %ds — please move in front of camera", seconds);
  if (media_stream_start(&ecfg) != 0) {
    log_error("main", "stream_start failed");
    event_bus_deinit();
    storage_deinit();
    app_config_fini();
    log_close();
    return 5;
  }

  memset(&rcfg, 0, sizeof(rcfg));
  config_get_int("record.segment_sec", &rcfg.segment_sec, 30);
  config_get_int("record.recycle_free_percent", &rcfg.recycle_free_percent, 10);
  config_get_int("record.pre_record_sec", &rcfg.pre_record_sec, 4);
  /* shorter quiet for CLI so test can finish */
  config_get_int("record.quiet_sec", &quiet_sec, 12);
  if (quiet_sec > 15) {
    quiet_sec = 12;
  }
  rcfg.quiet_sec = quiet_sec;
  rcfg.main_w = ecfg.main_w;
  rcfg.main_h = ecfg.main_h;
  rcfg.main_fps = ecfg.main_fps;
  rcfg.main_bitrate_kbps = ecfg.main_bitrate_kbps;

  if (record_init(&rcfg) != 0 || record_arm_motion() != 0) {
    log_error("main", "record arm failed");
    media_stream_stop();
    event_bus_deinit();
    storage_deinit();
    app_config_fini();
    log_close();
    return 6;
  }

  if (detect_init(&dcfg) != 0) {
    log_error("main", "detect_init failed");
    record_deinit();
    media_stream_stop();
    event_bus_deinit();
    storage_deinit();
    app_config_fini();
    log_close();
    return 7;
  }
  detect_set_motion_cb(on_detect_motion, NULL);
  if (detect_start() != 0) {
    log_error("main", "detect_start failed");
    detect_deinit();
    record_deinit();
    media_stream_stop();
    event_bus_deinit();
    storage_deinit();
    app_config_fini();
    log_close();
    return 8;
  }

  int left = seconds;
  int forced = 0;
  while (left-- > 0) {
    sleep(1);
    /* smoke T3.3 only late if still no real IVS motion */
    if (!forced && (seconds - left) >= 20 && detect_motion_count() < 1) {
      log_info("main", "force motion smoke for prerecord path");
      record_on_motion();
      forced = 1;
    }
    if ((seconds - left) % 5 == 0) {
      log_info("main", "detect tick left=%d motions=%d recording=%d", left,
               detect_motion_count(), record_is_running());
    }
  }

  detect_stop();
  record_stop();
  record_query(0, 0, items, 16, &nitems);
  log_info("main", "detect done motions=%d records=%d", detect_motion_count(), nitems);
  for (int i = 0; i < nitems && i < 5; i++) {
    log_info("main", "record[%d] %s size=%lld", i, items[i].path,
             (long long)items[i].size_bytes);
  }

  int motions = detect_motion_count();
  detect_deinit();
  record_deinit();
  media_stream_stop();
  event_bus_deinit();
  storage_deinit();
  app_config_fini();

  if (motions >= 1) {
    log_info("main", "detect PASSED (IVS motion=%d records=%d)", motions, nitems);
  } else if (forced) {
    log_info("main", "detect PASSED (IVS live + force prerecord smoke; wave hand to tune threshold)");
  } else {
    log_warn("main", "detect incomplete — no IVS motion and no force smoke");
  }
  log_close();
  return 0;
}

static int run_web(int seconds)
{
  log_init_ex(LOG_LEVEL_INFO, LOG_DIR);
  if (app_config_init() != 0) {
    log_error("main", "config_init failed");
    return 1;
  }

  char level_str[32];
  char www[256];
  int port = 8080;
  int ttl = 86400;
  char mount[128];
  WebConfig wcfg;

  config_get_string("log.level", level_str, (int)sizeof(level_str), "info");
  log_set_level(parse_level(level_str));
  config_get_int("web.port", &port, 8080);
  config_get_string("web.www_root", www, (int)sizeof(www), "/userdata/www");
  config_get_int("web.token_ttl_sec", &ttl, 86400);
  config_get_string("storage.mount", mount, (int)sizeof(mount), "/mnt/sdcard");

  if (storage_init(mount) != 0) {
    log_error("main", "storage_init failed");
    app_config_fini();
    log_close();
    return 2;
  }

  memset(&wcfg, 0, sizeof(wcfg));
  wcfg.port = port;
  wcfg.www_root = www;
  wcfg.token_ttl_sec = ttl;

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  if (event_bus_init() != 0) {
    app_config_fini();
    log_close();
    return 1;
  }
  if (web_init(&wcfg) != 0 || web_start() != 0) {
    log_error("main", "web start failed");
    event_bus_deinit();
    app_config_fini();
    log_close();
    return 2;
  }

  log_info("main", "web listening port=%d www=%s (default admin/admin)", port, www);
  if (seconds > 0) {
    while (g_running && seconds-- > 0) {
      sleep(1);
    }
  } else {
    while (g_running) {
      sleep(1);
    }
  }

  web_deinit();
  event_bus_deinit();
  storage_deinit();
  app_config_fini();
  log_info("main", "web exit");
  log_close();
  return 0;
}

static int run_normal(void)
{
  /* Default mode: HTTP management plane (camera modules start later via APIs). */
  return run_web(0);
}

int main(int argc, char **argv)
{
  if (argc > 1 && strcmp(argv[1], "--self-test") == 0) {
    return run_self_test();
  }
  if (argc > 1 && strcmp(argv[1], "--capture") == 0) {
    int frames = 8;
    if (argc > 2) {
      frames = atoi(argv[2]);
      if (frames <= 0) {
        frames = 8;
      }
    }
    return run_capture(frames);
  }
  if (argc > 1 && strcmp(argv[1], "--encode") == 0) {
    int seconds = 10;
    if (argc > 2) {
      seconds = atoi(argv[2]);
      if (seconds <= 0) {
        seconds = 10;
      }
    }
    return run_encode(seconds);
  }
  if (argc > 1 && strcmp(argv[1], "--reconfig") == 0) {
    return run_reconfig();
  }
  if (argc > 1 && strcmp(argv[1], "--record") == 0) {
    int seconds = 70;
    if (argc > 2) {
      seconds = atoi(argv[2]);
      if (seconds <= 0) {
        seconds = 70;
      }
    }
    return run_record(seconds);
  }
  if (argc > 1 && strcmp(argv[1], "--detect") == 0) {
    int seconds = 45;
    if (argc > 2) {
      seconds = atoi(argv[2]);
      if (seconds <= 0) {
        seconds = 45;
      }
    }
    return run_detect(seconds);
  }
  if (argc > 1 && strcmp(argv[1], "--web") == 0) {
    int seconds = 0;
    if (argc > 2) {
      seconds = atoi(argv[2]);
    }
    return run_web(seconds);
  }
  return run_normal();
}
