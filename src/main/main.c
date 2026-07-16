#include "common/event_bus.h"
#include "common/log.h"
#include "media/media_service.h"
#include "system/config_service.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOG_DIR "/userdata/log"
#define DEFAULT_CFG "/userdata/default_config.json"
#define USER_CFG "/userdata/ipc_config.json"

static volatile sig_atomic_t g_running = 1;
static int g_got_start_event;

static void on_signal(int sig) {
  (void)sig;
  g_running = 0;
}

static void on_app_start(const Event *event, void *user) {
  (void)user;
  g_got_start_event = 1;
  log_info("main", "event received: APP_START (type=%d)", (int)event->type);
}

static LogLevel parse_level(const char *s) {
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

static int file_exists(const char *path) {
  return access(path, F_OK) == 0;
}

static int run_self_test(void) {
  int failed = 0;

  log_init_ex(LOG_LEVEL_INFO, LOG_DIR);
  log_info("selftest", "T0.3 self-test begin");

  if (config_init(DEFAULT_CFG, USER_CFG) != 0) {
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
  if (event_bus_init() != 0 ||
      event_bus_subscribe(EVT_APP_START, on_app_start, NULL) != 0) {
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

  config_deinit();
  log_info("selftest", failed ? "FAILED" : "PASSED");
  log_close();
  return failed;
}

static int run_capture(int frame_count) {
  log_init_ex(LOG_LEVEL_INFO, LOG_DIR);
  if (config_init(DEFAULT_CFG, USER_CFG) != 0) {
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
  /* /userdata is only ~2MB; dump YUV to TF card by default. */
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
    config_deinit();
    log_close();
    return 2;
  }

  int ret = media_capture_nv12(capture_dir, "capture_0.yuv", frame_count);
  media_deinit();
  config_deinit();

  if (ret != 0) {
    log_error("main", "capture failed %d", ret);
    log_close();
    return 3;
  }
  log_info("main", "capture ok -> %s/capture_0.yuv", capture_dir);
  log_close();
  return 0;
}

static int run_normal(void) {
  log_init_ex(LOG_LEVEL_INFO, LOG_DIR);

  if (config_init(DEFAULT_CFG, USER_CFG) != 0) {
    log_error("main", "config_init failed");
    return 1;
  }

  char level_str[32];
  config_get_string("log.level", level_str, (int)sizeof(level_str), "info");
  log_set_level(parse_level(level_str));

  char name[64];
  config_get_string("device.name", name, (int)sizeof(name), "rv1106-aov-ipc");
  log_info("main", "ipc_app starting device=%s", name);

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  if (event_bus_init() != 0) {
    log_error("main", "event_bus_init failed");
    config_deinit();
    log_close();
    return 1;
  }
  event_bus_subscribe(EVT_APP_START, on_app_start, NULL);

  Event start_evt = {.type = EVT_APP_START, .data = NULL, .data_len = 0};
  event_bus_publish(&start_evt);

  int ticks = 0;
  while (g_running && ticks < 5) {
    sleep(1);
    ++ticks;
  }

  event_bus_deinit();
  config_deinit();
  log_info("main", "ipc_app exit");
  log_close();
  return 0;
}

int main(int argc, char **argv) {
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
  return run_normal();
}
