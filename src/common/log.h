#ifndef IPC_COMMON_LOG_H
#define IPC_COMMON_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  LOG_LEVEL_DEBUG = 0,
  LOG_LEVEL_INFO,
  LOG_LEVEL_WARN,
  LOG_LEVEL_ERROR
} LogLevel;

/* Console-only init (backward compatible). */
void log_init(LogLevel level);

/* Also append to <log_dir>/ipc_app.log when log_dir is non-NULL. */
int log_init_ex(LogLevel level, const char *log_dir);

void log_set_level(LogLevel level);
LogLevel log_get_level(void);
void log_close(void);

void log_printf(LogLevel level, const char *tag, const char *fmt, ...);

#define log_debug(tag, fmt, ...) log_printf(LOG_LEVEL_DEBUG, tag, fmt, ##__VA_ARGS__)
#define log_info(tag, fmt, ...)  log_printf(LOG_LEVEL_INFO, tag, fmt, ##__VA_ARGS__)
#define log_warn(tag, fmt, ...)  log_printf(LOG_LEVEL_WARN, tag, fmt, ##__VA_ARGS__)
#define log_error(tag, fmt, ...) log_printf(LOG_LEVEL_ERROR, tag, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* IPC_COMMON_LOG_H */
