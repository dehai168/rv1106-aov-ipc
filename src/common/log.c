#include "common/log.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static LogLevel g_level = LOG_LEVEL_INFO;
static FILE *g_fp;

void log_init(LogLevel level) {
  g_level = level;
}

int log_init_ex(LogLevel level, const char *log_dir) {
  g_level = level;
  log_close();

  if (!log_dir || !log_dir[0]) {
    return 0;
  }

  if (mkdir(log_dir, 0755) != 0 && errno != EEXIST) {
    fprintf(stderr, "log: mkdir %s failed: %s\n", log_dir, strerror(errno));
    return -1;
  }

  char path[256];
  snprintf(path, sizeof(path), "%s/ipc_app.log", log_dir);
  g_fp = fopen(path, "a");
  if (!g_fp) {
    fprintf(stderr, "log: open %s failed: %s\n", path, strerror(errno));
    return -2;
  }
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

  if (g_fp) {
    fputs(line, g_fp);
    fputc('\n', g_fp);
    fflush(g_fp);
  }
}
