#ifndef IPC_SYSTEM_CONFIG_SERVICE_H
#define IPC_SYSTEM_CONFIG_SERVICE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Load default JSON, merge user JSON over it. Paths may be NULL to use defaults:
 *   default: /userdata/default_config.json
 *   user:    /userdata/ipc_config.json
 */
int config_init(const char *default_path, const char *user_path);
void config_deinit(void);

/* Dot-path getters. Return 0 on success; on miss fill default and return 1. */
int config_get_string(const char *path, char *out, int out_len, const char *def_val);
int config_get_int(const char *path, int *out, int def_val);
int config_get_bool(const char *path, int *out, int def_val);

int config_set_string(const char *path, const char *value);
int config_set_int(const char *path, int value);
int config_set_bool(const char *path, int value);

/* Atomic save to user path (temp + rename). */
int config_save(void);

/* Reload user file and re-merge onto default. */
int config_reload(void);

const char *config_user_path(void);

#ifdef __cplusplus
}
#endif

#endif /* IPC_SYSTEM_CONFIG_SERVICE_H */
