#ifndef IPC_SYSTEM_SYSTEM_SERVICE_H
#define IPC_SYSTEM_SYSTEM_SERVICE_H

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYSTEM_TIMEZONE_MAX 64
#define SYSTEM_NTP_SERVER_MAX 128
#define SYSTEM_LOG_NAME_MAX 64

typedef struct {
  char device_name[64];
  char hostname[64];
  char model[32];
  char version[32];
  uint64_t uptime_sec;
  uint64_t mem_total_kb;
  uint64_t mem_free_kb;
} SystemInfo;

typedef struct {
  time_t unix_time;
  char timezone[SYSTEM_TIMEZONE_MAX];
  int ntp_enabled;
  char ntp_server[SYSTEM_NTP_SERVER_MAX];
} SystemTimeConfig;

int system_get_info(SystemInfo *out);
int system_get_time(SystemTimeConfig *out);
int system_set_time(const SystemTimeConfig *in, int apply_ntp);

/* Boot: load RTC, apply timezone, optional NTP thread when enabled. */
int system_time_init(void);
void system_time_deinit(void);

/* Schedule reboot (returns after fork). */
int system_reboot(void);

/* Delete user config and reload defaults. Does not wipe TF recordings. */
int system_factory_reset(void);

/* Resolve whitelisted log file under log dir. Returns 0 on success. */
int system_log_path(const char *name, char *out, int out_len);

#ifdef __cplusplus
}
#endif

#endif /* IPC_SYSTEM_SYSTEM_SERVICE_H */
