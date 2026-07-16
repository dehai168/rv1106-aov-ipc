#include "common/event_bus.h"

#include "common/log.h"

#include <string.h>

#define EVENT_BUS_MAX_SUBS 32

typedef struct {
  int used;
  EventType type;
  EventHandler handler;
  void *user;
} Subscription;

static Subscription g_subs[EVENT_BUS_MAX_SUBS];
static int g_inited;

int event_bus_init(void) {
  memset(g_subs, 0, sizeof(g_subs));
  g_inited = 1;
  log_info("event_bus", "init ok");
  return 0;
}

void event_bus_deinit(void) {
  memset(g_subs, 0, sizeof(g_subs));
  g_inited = 0;
}

int event_bus_subscribe(EventType type, EventHandler handler, void *user) {
  if (!g_inited || !handler || type == EVT_NONE) {
    return -1;
  }

  for (int i = 0; i < EVENT_BUS_MAX_SUBS; ++i) {
    if (!g_subs[i].used) {
      g_subs[i].used = 1;
      g_subs[i].type = type;
      g_subs[i].handler = handler;
      g_subs[i].user = user;
      return 0;
    }
  }

  log_error("event_bus", "subscribe full");
  return -2;
}

int event_bus_unsubscribe(EventType type, EventHandler handler) {
  if (!g_inited || !handler) {
    return -1;
  }

  for (int i = 0; i < EVENT_BUS_MAX_SUBS; ++i) {
    if (g_subs[i].used && g_subs[i].type == type && g_subs[i].handler == handler) {
      memset(&g_subs[i], 0, sizeof(g_subs[i]));
      return 0;
    }
  }
  return -3;
}

int event_bus_publish(const Event *event) {
  if (!g_inited || !event || event->type == EVT_NONE) {
    return -1;
  }

  for (int i = 0; i < EVENT_BUS_MAX_SUBS; ++i) {
    if (g_subs[i].used && g_subs[i].type == event->type && g_subs[i].handler) {
      g_subs[i].handler(event, g_subs[i].user);
    }
  }
  return 0;
}
