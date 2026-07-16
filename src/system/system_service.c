#include "system/system_service.h"

#include "common/log.h"
#include "system/config_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef IPC_APP_VERSION
#define IPC_APP_VERSION "0.1.0-dev"
#endif

static int run_cmd(const char *cmd)
{
  int rc = system(cmd);
  if (rc != 0) {
    log_warn("system", "cmd failed rc=%d: %s", rc, cmd);
  }
  return rc;
}

static void read_mem_kb(uint64_t *total, uint64_t *free)
{
  FILE *fp;
  char line[128];
  uint64_t t = 0;
  uint64_t f = 0;

  if (total) {
    *total = 0;
  }
  if (free) {
    *free = 0;
  }
  fp = fopen("/proc/meminfo", "r");
  if (!fp) {
    return;
  }
  while (fgets(line, sizeof(line), fp)) {
    unsigned long v;
    if (sscanf(line, "MemTotal: %lu kB", &v) == 1) {
      t = v;
    } else if (sscanf(line, "MemAvailable: %lu kB", &v) == 1) {
      f = v;
    } else if (f == 0 && sscanf(line, "MemFree: %lu kB", &v) == 1) {
      f = v;
    }
  }
  fclose(fp);
  if (total) {
    *total = t;
  }
  if (free) {
    *free = f;
  }
}

static void load_time_cfg(SystemTimeConfig *out)
{
  memset(out, 0, sizeof(*out));
  out->unix_time = time(NULL);
  config_get_string("system.timezone", out->timezone, (int)sizeof(out->timezone), "Asia/Shanghai");
  config_get_bool("system.ntp.enabled", &out->ntp_enabled, 0);
  config_get_string("system.ntp.server", out->ntp_server, (int)sizeof(out->ntp_server),
                    "pool.ntp.org");
}

static int persist_time_cfg(const SystemTimeConfig *cfg)
{
  if (!cfg) {
    return -1;
  }
  if (config_set_string("system.timezone", cfg->timezone) != 0 ||
      config_set_bool("system.ntp.enabled", cfg->ntp_enabled) != 0 ||
      config_set_string("system.ntp.server", cfg->ntp_server) != 0 || config_save() != 0) {
    return -1;
  }
  return 0;
}

/* BusyBox often lacks zoneinfo; map common IANA names to POSIX TZ. */
static const char *posix_tz(const char *tz)
{
  if (!tz || !tz[0]) {
    return "CST-8";
  }
  if (strcmp(tz, "Asia/Shanghai") == 0 || strcmp(tz, "Asia/Chongqing") == 0 ||
      strcmp(tz, "PRC") == 0) {
    return "CST-8";
  }
  if (strcmp(tz, "UTC") == 0 || strcmp(tz, "Etc/UTC") == 0) {
    return "UTC0";
  }
  return tz;
}

static int apply_timezone(const char *tz)
{
  const char *pos = posix_tz(tz);
  if (!pos || !pos[0]) {
    return 0;
  }
  setenv("TZ", pos, 1);
  tzset();
  {
    FILE *fp = fopen("/etc/TZ", "w");
    if (fp) {
      fprintf(fp, "%s\n", pos);
      fclose(fp);
    }
  }
  return 0;
}

static int apply_system_time(time_t ts)
{
  struct timeval tv;
  tv.tv_sec = ts;
  tv.tv_usec = 0;
  if (settimeofday(&tv, NULL) != 0) {
    log_error("system", "settimeofday failed");
    return -1;
  }
  run_cmd("hwclock -w 2>/dev/null");
  return 0;
}

static int try_ntp_sync(const char *server)
{
  char cmd[256];
  if (!server || !server[0]) {
    return -1;
  }
  snprintf(cmd, sizeof(cmd), "ntpdate -u %s 2>/dev/null", server);
  if (run_cmd(cmd) == 0) {
    run_cmd("hwclock -w 2>/dev/null");
    return 0;
  }
  snprintf(cmd, sizeof(cmd), "busybox ntpd -n -q -p %s 2>/dev/null", server);
  if (run_cmd(cmd) == 0) {
    run_cmd("hwclock -w 2>/dev/null");
    return 0;
  }
  return -1;
}

static pthread_t g_ntp_thread;
static int g_ntp_started;
static volatile int g_ntp_quit;

static void *ntp_worker(void *arg)
{
  (void)arg;
  while (!g_ntp_quit) {
    int enabled = 0;
    int interval_min = 60;
    char server[SYSTEM_NTP_SERVER_MAX];

    config_get_bool("system.ntp.enabled", &enabled, 0);
    config_get_int("system.ntp.interval_min", &interval_min, 60);
    config_get_string("system.ntp.server", server, (int)sizeof(server), "pool.ntp.org");
    if (interval_min < 1) {
      interval_min = 1;
    }

    if (enabled && server[0]) {
      if (try_ntp_sync(server) == 0) {
        log_info("system", "ntp sync ok server=%s", server);
      } else {
        log_warn("system", "ntp sync failed server=%s", server);
      }
    }

    for (int s = 0; s < interval_min * 60 && !g_ntp_quit; s++) {
      sleep(1);
    }
  }
  return NULL;
}

int system_time_init(void)
{
  SystemTimeConfig tc;

  if (g_ntp_started) {
    return 0;
  }

  load_time_cfg(&tc);
  apply_timezone(tc.timezone);
  if (run_cmd("hwclock -s 2>/dev/null") == 0) {
    log_info("system", "rtc loaded (tz=%s)", tc.timezone);
  } else {
    log_warn("system", "hwclock -s failed or unavailable");
  }

  if (tc.ntp_enabled && tc.ntp_server[0] && try_ntp_sync(tc.ntp_server) == 0) {
    log_info("system", "boot ntp sync ok");
  }

  g_ntp_quit = 0;
  if (pthread_create(&g_ntp_thread, NULL, ntp_worker, NULL) != 0) {
    log_error("system", "ntp thread create failed");
    return -1;
  }
  g_ntp_started = 1;
  return 0;
}

void system_time_deinit(void)
{
  if (!g_ntp_started) {
    return;
  }
  g_ntp_quit = 1;
  pthread_join(g_ntp_thread, NULL);
  g_ntp_started = 0;
}

int system_get_info(SystemInfo *out)
{
  struct sysinfo si;
  if (!out) {
    return -1;
  }
  memset(out, 0, sizeof(*out));
  config_get_string("device.name", out->device_name, (int)sizeof(out->device_name),
                    "rv1106-aov-ipc");
  if (gethostname(out->hostname, (int)sizeof(out->hostname)) != 0) {
    snprintf(out->hostname, sizeof(out->hostname), "ipc");
  }
  snprintf(out->model, sizeof(out->model), "RV1106");
  snprintf(out->version, sizeof(out->version), "%s", IPC_APP_VERSION);
  if (sysinfo(&si) == 0) {
    out->uptime_sec = (uint64_t)si.uptime;
  }
  read_mem_kb(&out->mem_total_kb, &out->mem_free_kb);
  return 0;
}

int system_get_time(SystemTimeConfig *out)
{
  if (!out) {
    return -1;
  }
  load_time_cfg(out);
  out->unix_time = time(NULL);
  return 0;
}

int system_set_time(const SystemTimeConfig *in, int apply_ntp)
{
  SystemTimeConfig cfg;
  if (!in) {
    return -1;
  }
  cfg = *in;
  if (!cfg.timezone[0]) {
    snprintf(cfg.timezone, sizeof(cfg.timezone), "Asia/Shanghai");
  }
  if (!cfg.ntp_server[0]) {
    snprintf(cfg.ntp_server, sizeof(cfg.ntp_server), "pool.ntp.org");
  }
  if (persist_time_cfg(&cfg) != 0) {
    return -2;
  }
  apply_timezone(cfg.timezone);

  if (apply_ntp && cfg.ntp_enabled) {
    if (try_ntp_sync(cfg.ntp_server) != 0) {
      return -3;
    }
    return 0;
  }

  if (in->unix_time > 0) {
    if (apply_system_time(in->unix_time) != 0) {
      return -4;
    }
  }
  return 0;
}

int system_reboot(void)
{
  pid_t pid = fork();
  if (pid < 0) {
    return -1;
  }
  if (pid == 0) {
    sleep(1);
    sync();
    execl("/sbin/reboot", "reboot", (char *)NULL);
    execl("/bin/busybox", "busybox", "reboot", (char *)NULL);
    _exit(1);
  }
  log_info("system", "reboot scheduled");
  return 0;
}

int system_factory_reset(void)
{
  const char *user = config_user_path();
  if (user && user[0]) {
    if (unlink(user) != 0) {
      log_warn("system", "unlink %s may have failed", user);
    }
  }
  if (config_reload() != 0) {
    return -1;
  }
  log_warn("system", "factory reset: user config cleared");
  return 0;
}

int system_log_path(const char *name, char *out, int out_len)
{
  char log_dir[128];
  const char *file;
  struct stat st;

  if (!out || out_len <= 0) {
    return -1;
  }
  out[0] = '\0';
  file = (name && name[0]) ? name : "ipc_app.log";
  if (strcmp(file, "ipc_app.log") != 0) {
    return -2;
  }
  config_get_string("log.dir", log_dir, (int)sizeof(log_dir), "/userdata/log");
  snprintf(out, (size_t)out_len, "%s/%s", log_dir, file);
  if (stat(out, &st) != 0 || !S_ISREG(st.st_mode)) {
    out[0] = '\0';
    return -3;
  }
  return 0;
}
