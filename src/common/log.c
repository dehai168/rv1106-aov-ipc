#include "common/log.h"

#include "system/config_service.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/*
 * Size-capped logging for a small (2.2MB) UBIFS /userdata volume.
 *
 * /userdata used to fill up with ipc_app.log + ipc_stderr.log and then every
 * write failed — including config saves — which broke web startup entirely.
 * Both files are now rotated by size and keep only a few generations:
 *
 *   log.max_kb (default 64), log.keep (default 2)
 *
 * Worst case on disk is (keep + 1) * max_kb per stream, i.e. ~384KB total.
 */

#define LOG_NAME "ipc_app.log"
#define LOG_ERR_NAME "ipc_stderr.log"
#define LOG_DEFAULT_MAX_KB 64
#define LOG_DEFAULT_KEEP 2
#define LOG_MAX_KEEP 8

static LogLevel g_level = LOG_LEVEL_INFO;
static FILE *g_fp;
static char g_dir[192];
static long g_bytes;
static long g_err_bytes;
static int g_stderr_redirected;
static int g_max_kb = LOG_DEFAULT_MAX_KB;
static int g_keep = LOG_DEFAULT_KEEP;

void log_init(LogLevel level) {
  g_level = level;
}

static long file_size(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
    return -1;
  }
  return (long)st.st_size;
}

static void refresh_policy(void) {
  int v = 0;
  /* Config may not be loaded yet on early calls; defaults are safe. */
  if (config_get_int("log.max_kb", &v, LOG_DEFAULT_MAX_KB) == 0 && v >= 8 && v <= 4096) {
    g_max_kb = v;
  } else {
    g_max_kb = LOG_DEFAULT_MAX_KB;
  }
  v = 0;
  if (config_get_int("log.keep", &v, LOG_DEFAULT_KEEP) == 0 && v >= 1 && v <= LOG_MAX_KEEP) {
    g_keep = v;
  } else {
    g_keep = LOG_DEFAULT_KEEP;
  }
}

/* Shift name -> name.1 -> name.2 ... and drop the oldest generation. */
static void rotate_files(const char *dir, const char *name) {
  char from[320];
  char to[320];
  int i;

  snprintf(from, sizeof(from), "%s/%s.%d", dir, name, g_keep);
  unlink(from);
  for (i = g_keep - 1; i >= 1; i--) {
    snprintf(from, sizeof(from), "%s/%s.%d", dir, name, i);
    snprintf(to, sizeof(to), "%s/%s.%d", dir, name, i + 1);
    if (rename(from, to) != 0 && errno != ENOENT) {
      unlink(from);
    }
  }
  snprintf(from, sizeof(from), "%s/%s", dir, name);
  snprintf(to, sizeof(to), "%s/%s.1", dir, name);
  if (rename(from, to) != 0 && errno != ENOENT) {
    unlink(from);
  }
}

static long rotate_now(const char *name) {
  char path[320];
  if (!g_dir[0]) {
    return 0;
  }
  rotate_files(g_dir, name);
  snprintf(path, sizeof(path), "%s/%s", g_dir, name);
  return file_size(path) > 0 ? file_size(path) : 0;
}

/* Re-point fd 2 at a fresh stderr log file so library noise is capped too. */
static void redirect_stderr(void) {
  char path[320];
  FILE *fp;

  if (!g_dir[0]) {
    return;
  }
  snprintf(path, sizeof(path), "%s/%s", g_dir, LOG_ERR_NAME);
  fp = fopen(path, "a");
  if (!fp) {
    fprintf(stderr, "log: open %s failed: %s\n", path, strerror(errno));
    return;
  }
  if (dup2(fileno(fp), STDERR_FILENO) < 0) {
    fclose(fp);
    return;
  }
  fclose(fp); /* fd 2 keeps the open file description alive. */
  g_stderr_redirected = 1;
  g_err_bytes = file_size(path);
  if (g_err_bytes < 0) {
    g_err_bytes = 0;
  }
}

int log_init_ex(LogLevel level, const char *log_dir) {
  char path[320];

  g_level = level;
  log_close();

  if (!log_dir || !log_dir[0]) {
    return 0;
  }

  if (mkdir(log_dir, 0755) != 0 && errno != EEXIST) {
    fprintf(stderr, "log: mkdir %s failed: %s\n", log_dir, strerror(errno));
    return -1;
  }
  snprintf(g_dir, sizeof(g_dir), "%s", log_dir);
  refresh_policy();

  snprintf(path, sizeof(path), "%s/%s", log_dir, LOG_NAME);
  if (file_size(path) > (long)g_max_kb * 1024) {
    rotate_now(LOG_NAME);
  }
  g_fp = fopen(path, "a");
  if (!g_fp) {
    fprintf(stderr, "log: open %s failed: %s\n", path, strerror(errno));
    return -2;
  }
  g_bytes = file_size(path);
  if (g_bytes < 0) {
    g_bytes = 0;
  }

  redirect_stderr();
  return 0;
}

void log_set_level(LogLevel level) {
  g_level = level;
}

LogLevel log_get_level(void) {
  return g_level;
}

void log_close(void) {
  if (g_fp) {
    fclose(g_fp);
    g_fp = NULL;
  }
  g_bytes = 0;
}

static const char *level_name(LogLevel level) {
  switch (level) {
  case LOG_LEVEL_DEBUG:
    return "DEBUG";
  case LOG_LEVEL_INFO:
    return "INFO";
  case LOG_LEVEL_WARN:
    return "WARN";
  case LOG_LEVEL_ERROR:
    return "ERROR";
  default:
    return "?";
  }
}

/* Rotate + reopen when a stream outgrew its cap. */
static void enforce_cap(void) {
  char path[320];
  static int writes;

  if (!g_dir[0]) {
    return;
  }
  /* Library noise (rkmuxer etc.) writes straight to fd 2, so resync the
   * counter with the real file size once in a while. */
  if (g_stderr_redirected && ++writes >= 32) {
    writes = 0;
    snprintf(path, sizeof(path), "%s/%s", g_dir, LOG_ERR_NAME);
    {
      long real = file_size(path);
      if (real > g_err_bytes) {
        g_err_bytes = real;
      }
    }
  }
  if (g_fp && g_bytes > (long)g_max_kb * 1024) {
    fclose(g_fp);
    g_fp = NULL;
    rotate_now(LOG_NAME);
    snprintf(path, sizeof(path), "%s/%s", g_dir, LOG_NAME);
    g_fp = fopen(path, "a");
    g_bytes = 0;
  }
  if (g_stderr_redirected && g_err_bytes > (long)g_max_kb * 1024) {
    rotate_now(LOG_ERR_NAME);
    redirect_stderr();
  }
}

void log_printf(LogLevel level, const char *tag, const char *fmt, ...) {
  if (level < g_level) {
    return;
  }

  time_t now = time(NULL);
  struct tm tm_now;
  localtime_r(&now, &tm_now);

  char tbuf[32];
  strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm_now);

  char line[1024];
  int n = snprintf(line, sizeof(line), "[%s][%s][%s] ", tbuf, level_name(level),
                   tag ? tag : "-");
  if (n < 0) {
    n = 0;
  }
  if (n >= (int)sizeof(line)) {
    n = (int)sizeof(line) - 1;
  }

  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
  va_end(ap);

  fputs(line, stderr);
  fputc('\n', stderr);
  fflush(stderr);
  g_err_bytes += (long)strlen(line) + 1;

  if (g_fp) {
    fputs(line, g_fp);
    fputc('\n', g_fp);
    fflush(g_fp);
    g_bytes += (long)strlen(line) + 1;
  }

  enforce_cap();
}
