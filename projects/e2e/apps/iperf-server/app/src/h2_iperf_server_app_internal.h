#ifndef H2_IPERF_SERVER_APP_INTERNAL_H
#define H2_IPERF_SERVER_APP_INTERNAL_H
#include "h2_atomic.h"
#include "h2_iperf_server_app.h"

typedef struct h2_iperf_server_app_worker {
  h2_iperf_server_app_t *app;
  unsigned index;
  h2_iperf_server_t *server;
  h2_pal_task_t *task;
} h2_iperf_server_app_worker_t;

struct h2_iperf_server_app {
  h2_runtime_t *runtime;
  h2_iperf_server_app_config_t config;
  h2_pal_mutex_t *mutex;
  h2_pal_task_t *manager;
  h2_atomic_bool_t stop;
  h2_atomic_bool_t shutdown;
  bool requested;
  bool network_active;
  h2_iperf_server_app_snapshot_t snapshot;
  h2_iperf_server_app_worker_t workers[2];
};
#endif
