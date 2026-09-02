#ifndef H2_GIZCLAW_SERVICE_INTERNAL_H
#define H2_GIZCLAW_SERVICE_INTERNAL_H

#include "h2_gizclaw_service.h"
#include "h2_gizclaw_speech.h"

#include <stdatomic.h>

typedef struct h2_gizclaw_conversation_request
    h2_gizclaw_conversation_request_t;
#include <stdbool.h>

typedef struct h2_gizclaw_request_vtable {
  h2_pal_result_t (*do_request)(h2_gizclaw_request_t *request,
                                h2_gizclaw_request_callback_fn callback);
  h2_pal_result_t (*finish_input)(h2_gizclaw_request_t *request);
  h2_pal_result_t (*wait)(h2_gizclaw_request_t *request, uint32_t timeout_ms);
  h2_pal_result_t (*cancel)(h2_gizclaw_request_t *request);
  void (*release)(h2_gizclaw_request_t *request);
} h2_gizclaw_request_vtable_t;

struct h2_gizclaw_request {
  const h2_gizclaw_request_vtable_t *vtable;
};

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
  uint64_t trace_sequence;
  uint64_t queued_at_ms;
  uint64_t started_at_ms;
  uint64_t completed_at_ms;
  uint64_t progress_dispatch_total_ms;
  uint32_t poll_count;
  uint32_t progress_dispatch_count;
  uint32_t progress_dispatch_max_ms;
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
  _Atomic(h2_gizclaw_conversation_request_t *) media_request;
  _Atomic(h2_gizclaw_speech_extract_request_t *) speech_request;
  _Atomic(h2_gizclaw_track_t *) pcm_track;
  h2_pal_webrtc_track_vtable_t webrtc_track_vtable;
  h2_pal_webrtc_track_t webrtc_track;
  atomic_uint media_callback_refs;
  h2_gizclaw_client_t *client;
  h2_gizclaw_operation_t *current;
  h2_gizclaw_operation_t *pending;
  size_t active_count;
  size_t caller_reference_count;
  size_t queued_event_count;
  uint64_t next_trace_sequence;
  bool started;
  bool stopping;
  bool stopped;
  bool dispatching;
  bool terminal_pending;
  bool terminal_dispatched;
  h2_pal_result_t terminal_result;
};

h2_pal_result_t h2_gizclaw_conversation_media_attach(
    h2_gizclaw_service_t *service, h2_gizclaw_conversation_request_t *request);
void h2_gizclaw_conversation_media_detach(
    h2_gizclaw_conversation_request_t *request);
h2_pal_result_t
h2_gizclaw_service_media_read_opus(h2_gizclaw_service_t *service, uint8_t *opus,
                                   size_t capacity, size_t *out_len);
h2_pal_result_t
h2_gizclaw_service_media_write_opus(h2_gizclaw_service_t *service,
                                    const uint8_t *opus, size_t opus_len);

h2_pal_result_t h2_gizclaw_service_submit_async_internal(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_operation_run_fn start, h2_gizclaw_operation_run_fn poll,
    h2_gizclaw_operation_completion_fn completion, void *user,
    h2_gizclaw_operation_t **out_operation);

void h2_gizclaw_service_log_request(const h2_gizclaw_service_t *service,
                                    h2_pal_log_level_t level,
                                    const char *request_kind, const char *stage,
                                    uint64_t identity, h2_pal_result_t result,
                                    int detail_code, size_t frame_count,
                                    size_t byte_count);

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

void h2_gizclaw_async_rpc_test_set_ops(const h2_gizclaw_async_rpc_ops_t *ops);

int h2_gizclaw_speech_test_request_create(
    h2_gizclaw_service_t *service,
    h2_gizclaw_speech_extract_request_t **out_request);
int h2_gizclaw_speech_test_request_receive(
    h2_gizclaw_speech_extract_request_t *request, uint8_t *out_audio,
    size_t audio_capacity, size_t *out_audio_len, uint32_t timeout_ms);
void h2_gizclaw_speech_test_request_set_terminal(
    h2_gizclaw_speech_extract_request_t *request);
void h2_gizclaw_speech_test_request_destroy(
    h2_gizclaw_speech_extract_request_t *request);
#endif

#endif
