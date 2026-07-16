#ifndef IPC_COMMON_EVENT_BUS_H
#define IPC_COMMON_EVENT_BUS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  EVT_NONE = 0,
  EVT_APP_START,
  EVT_APP_STOP,
  EVT_MOTION_DETECT,
  EVT_RECORD_START,
  EVT_RECORD_STOP
} EventType;

typedef struct {
  EventType type;
  void *data;
  size_t data_len;
} Event;

typedef void (*EventHandler)(const Event *event, void *user);

int event_bus_init(void);
void event_bus_deinit(void);

/* Returns 0 on success, negative on error. handler must stay valid until unsubscribe. */
int event_bus_subscribe(EventType type, EventHandler handler, void *user);
int event_bus_unsubscribe(EventType type, EventHandler handler);

/* Publish is synchronous for now (handler called in caller thread). */
int event_bus_publish(const Event *event);

#ifdef __cplusplus
}
#endif

#endif /* IPC_COMMON_EVENT_BUS_H */
