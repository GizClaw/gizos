#ifndef H2_GIZCLAW_SERVICE_INTERNAL_H
#define H2_GIZCLAW_SERVICE_INTERNAL_H

#include "h2_gizclaw_service.h"

#include <stdbool.h>
#include <stdatomic.h>

typedef enum h2_gizclaw_operation_state {
  H2_GIZCLAW_OPERATION_QUEUED = 0,
  H2_GIZCLAW_OPERATION_RUNNING,
  H2_GIZCLAW_OPERATION_PENDING,
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
  h2_gizclaw_operation_run_fn poll;
  h2_gizclaw_operation_completion_fn completion;
  void *user;
  h2_gizclaw_operation_result_t result;
  h2_pal_semaphore_t *completed;
  atomic_bool terminal;
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
  h2_gizclaw_operation_t *next_pending;
};

typedef enum h2_gizclaw_dispatch_kind {
  H2_GIZCLAW_DISPATCH_OPERATION = 0,
  H2_GIZCLAW_DISPATCH_CLIENT_EVENT,
} h2_gizclaw_dispatch_kind_t;

typedef struct h2_gizclaw_dispatch_item {
  h2_gizclaw_dispatch_kind_t kind;
  h2_gizclaw_operation_t *operation;
  h2_gizclaw_client_event_t event;
  char *event_workspace_name;
} h2_gizclaw_dispatch_item_t;

struct h2_gizclaw_service {
  h2_gizclaw_service_config_t config;
  h2_gizclaw_config_t client_config;
  h2_gizclaw_cancel_fn original_cancel;
  void *original_cancel_user;
  h2_pal_queue_t *request_queue;
  h2_pal_queue_t *completion_queue;
  h2_pal_mutex_t *mutex;
  h2_pal_cond_t *progress_cond;
  h2_pal_task_t *net_task;
  h2_gizclaw_client_t *client;
  h2_gizclaw_operation_t *current;
  h2_gizclaw_operation_t *pending;
  size_t active_count;
  size_t caller_reference_count;
  size_t queued_event_count;
  bool started;
  bool stopping;
  bool stopped;
  bool dispatching;
  bool terminal_pending;
  bool terminal_dispatched;
  h2_pal_result_t terminal_result;
};

h2_pal_result_t h2_gizclaw_service_submit_async_internal(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_operation_run_fn start, h2_gizclaw_operation_run_fn poll,
    h2_gizclaw_operation_completion_fn completion, void *user,
    h2_gizclaw_operation_t **out_operation);

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

typedef struct h2_gizclaw_async_rpc_ops {
  int (*start)(h2_gizclaw_client_t *client, h2_gizclaw_rpc_method_t method,
               h2_gizclaw_rpc_bytes_t params_payload, uint32_t timeout_ms,
               h2_gizclaw_rpc_request_t **out_request);
  int (*result)(h2_gizclaw_rpc_request_t *request,
                h2_gizclaw_rpc_response_t *out_response);
  void (*cancel)(h2_gizclaw_rpc_request_t *request);
  void (*destroy)(h2_gizclaw_rpc_request_t *request);
} h2_gizclaw_async_rpc_ops_t;

void h2_gizclaw_async_rpc_test_set_ops(
    const h2_gizclaw_async_rpc_ops_t *ops);
#endif

#endif
