#include "common/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static LogLevel g_level = LOG_LEVEL_INFO;

void log_init(LogLevel level) {
  g_level = level;
}

void log_set_level(LogLevel level) {
  g_level = level;
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
  strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tm_now);

  fprintf(stderr, "[%s][%s][%s] ", tbuf, level_name(level), tag ? tag : "-");

  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);

  fputc('\n', stderr);
  fflush(stderr);
}
