#ifndef IPC_WEB_WEB_SERVICE_H
#define IPC_WEB_WEB_SERVICE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int port;                 /* default 8080 */
  const char *www_root;     /* default /userdata/www */
  int token_ttl_sec;        /* default 86400 */
} WebConfig;

int web_init(const WebConfig *cfg);
void web_deinit(void);

int web_start(void); /* start poll thread */
void web_stop(void);
int web_is_running(void);

/* AOV: stop preview WS clients and motion automation. */
void web_preview_force_stop(void);

/* Re-apply persisted record.mode policy (after AOV leave / config change). */
int web_apply_record_policy(void);

#ifdef __cplusplus
}
#endif

#endif
