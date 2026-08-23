#ifndef H2_GIZCLAW_SERVICE_INTERNAL_H
#define H2_GIZCLAW_SERVICE_INTERNAL_H

#include "h2_gizclaw_service.h"

#include <stdbool.h>

typedef enum h2_gizclaw_operation_state {
  H2_GIZCLAW_OPERATION_QUEUED = 0,
  H2_GIZCLAW_OPERATION_RUNNING,
  H2_GIZCLAW_OPERATION_PROGRESS_PENDING,
  H2_GIZCLAW_OPERATION_COMPLETION_PENDING,
  H2_GIZCLAW_OPERATION_DISPATCHING,
  H2_GIZCLAW_OPERATION_TERMINAL,
} h2_gizclaw_operation_state_t;

struct h2_gizclaw_cancel_token {
  h2_gizclaw_operation_t *operation;
};

struct h2_gizclaw_operation {
  h2_gizclaw_service_t *service;
  h2_gizclaw_operation_run_fn run;
  h2_gizclaw_operation_completion_fn completion;
  void *user;
  h2_gizclaw_operation_result_t result;
  h2_gizclaw_operation_state_t state;
  bool cancel_requested;
  bool caller_reference;
  bool internal_reference;
  bool completion_queued;
  bool progress_pending;
  bool progress_claimed;
  h2_gizclaw_operation_dispatch_fn progress;
  void *progress_user;
  h2_pal_result_t progress_result;
  h2_gizclaw_cancel_token_t cancel_token;
};

struct h2_gizclaw_service {
  h2_gizclaw_service_config_t config;
  h2_gizclaw_config_t client_config;
  h2_gizclaw_cancel_fn original_cancel;
  void *original_cancel_user;
  h2_pal_queue_t *request_queue;
  h2_pal_queue_t *completion_queue;
  h2_pal_mutex_t *mutex;
  h2_pal_cond_t *progress_cond;
  h2_pal_task_t *task;
  h2_gizclaw_client_t *client;
  h2_gizclaw_operation_t *current;
  size_t active_count;
  size_t caller_reference_count;
  bool started;
  bool stopping;
  bool stopped;
  bool dispatching;
  bool terminal_pending;
  bool terminal_dispatched;
  h2_pal_result_t terminal_result;
};

#ifdef H2_GIZCLAW_TESTING
typedef struct h2_gizclaw_service_client_ops {
  h2_pal_result_t (*init)(const h2_gizclaw_config_t *config,
                          h2_gizclaw_client_t **out_client);
  h2_pal_result_t (*connect)(h2_gizclaw_client_t *client);
  h2_pal_result_t (*poll)(h2_gizclaw_client_t *client, int timeout_ms);
  h2_pal_result_t (*set_event_handler)(h2_gizclaw_client_t *client,
                                       h2_gizclaw_client_event_fn on_event,
                                       void *event_user);
  h2_pal_result_t (*dispatch_event)(h2_gizclaw_client_t *client);
  h2_pal_result_t (*close)(h2_gizclaw_client_t *client);
  void (*deinit)(h2_gizclaw_client_t *client);
} h2_gizclaw_service_client_ops_t;

void h2_gizclaw_service_test_set_client_ops(
    const h2_gizclaw_service_client_ops_t *ops);
#endif

#endif
