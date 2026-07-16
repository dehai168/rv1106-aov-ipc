#include "common/event_bus.h"
#include "common/log.h"

#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig) {
  (void)sig;
  g_running = 0;
}

static void on_app_start(const Event *event, void *user) {
  (void)user;
  log_info("main", "event received: APP_START (type=%d)", (int)event->type);
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  log_init(LOG_LEVEL_INFO);
  log_info("main", "ipc_app hello starting (rv1106-aov-ipc T0.2)");

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  if (event_bus_init() != 0) {
    log_error("main", "event_bus_init failed");
    return 1;
  }

  if (event_bus_subscribe(EVT_APP_START, on_app_start, NULL) != 0) {
    log_error("main", "event_bus_subscribe failed");
    event_bus_deinit();
    return 1;
  }

  Event start_evt = {.type = EVT_APP_START, .data = NULL, .data_len = 0};
  event_bus_publish(&start_evt);

  log_info("main", "running; send SIGTERM/SIGINT to stop");

  /* Keep alive briefly so adb can capture logs; exit after idle loop ticks. */
  int ticks = 0;
  while (g_running && ticks < 10) {
    sleep(1);
    ++ticks;
  }

  Event stop_evt = {.type = EVT_APP_STOP, .data = NULL, .data_len = 0};
  event_bus_publish(&stop_evt);

  event_bus_deinit();
  log_info("main", "ipc_app exit");
  return 0;
}
