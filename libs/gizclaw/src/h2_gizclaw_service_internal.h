#ifndef H2_GIZCLAW_SERVICE_INTERNAL_H
#define H2_GIZCLAW_SERVICE_INTERNAL_H

#include "h2_gizclaw_internal.h"
#include "h2_gizclaw_pcm_track_internal.h"
#include "h2_gizclaw_service.h"
#include "h2_gizclaw_speech.h"

#include <stdatomic.h>

typedef struct h2_gizclaw_conversation_request
    h2_gizclaw_conversation_request_t;
struct h2_gizclaw_speech_context;
struct h2_gizclaw_managed_request;
#include <stdbool.h>

typedef struct h2_gizclaw_operation h2_gizclaw_operation_t;
typedef struct h2_gizclaw_cancel_token h2_gizclaw_cancel_token_t;

/**
 * Execute one network operation on the sole client-owning network task.
 *
 * `user` remains caller-owned through completion callback return. The run
 * function is invoked exactly once for an accepted, non-queued-canceled
 * operation and returns that operation's result. It must cooperate with the
 * supplied cancellation token and must not retain the client or token.
 */
typedef h2_pal_result_t (*h2_gizclaw_operation_run_fn)(
    void *user, h2_gizclaw_client_t *client,
    const h2_gizclaw_cancel_token_t *cancel_token);

/**
 * Apply one terminal result on the thread calling service dispatch.
 *
 * The operation, result view, and caller-owned `user` remain valid through
 * callback return. This callback may release its operation handle.
 */
typedef void (*h2_gizclaw_operation_completion_fn)(
    void *user, h2_gizclaw_operation_t *operation,
    const h2_gizclaw_operation_result_t *result);

/* Sole network-owner scheduling internals, not application request handles. */
bool h2_gizclaw_cancel_requested(const h2_gizclaw_cancel_token_t *cancel_token);
h2_pal_result_t h2_gizclaw_operation_cancel(h2_gizclaw_operation_t *operation);
void h2_gizclaw_operation_release(h2_gizclaw_operation_t *operation);

typedef struct h2_gizclaw_req_vtable {
  h2_pal_result_t (*do_request)(h2_gizclaw_req_t *request,
                                void *user,
                                h2_gizclaw_req_input_read_fn input_read,
                                h2_gizclaw_req_output_write_fn output_write,
                                h2_gizclaw_req_complete_fn on_complete);
  h2_pal_result_t (*wait)(h2_gizclaw_req_t *request, uint32_t timeout_ms);
  h2_pal_result_t (*cancel)(h2_gizclaw_req_t *request);
  void (*release)(h2_gizclaw_req_t *request);
} h2_gizclaw_req_vtable_t;

struct h2_gizclaw_req {
  const h2_gizclaw_req_vtable_t *vtable;
};

/* One owner-task submission, with no pending poll or retry. Even WOULD_BLOCK
 * is terminal. Context ownership transfers only on successful construction. */
h2_pal_result_t h2_gizclaw_req_create_send_internal(
    h2_gizclaw_service_t *service, uint64_t identity, uint32_t timeout_ms,
    const void *tag, h2_gizclaw_operation_run_fn send,
    void (*destroy)(void *context), void *context,
    h2_gizclaw_req_t **out_request);
h2_pal_result_t h2_gizclaw_req_context_internal(const h2_gizclaw_req_t *request,
                                                const void *tag,
                                                const void **out_context);
/* Unary RPC with owned parser metadata. Context ownership transfers only on
 * successful construction. */
h2_pal_result_t h2_gizclaw_req_create_rpc_context_internal(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_rpc_method_t method, const void *tag,
    h2_gizclaw_rpc_bytes_t payload, uint32_t timeout_ms,
    void (*destroy)(void *), void *context, h2_gizclaw_req_t **out_request);

/* Fixed-length benchmark source; protocol I/O stays on the network owner.
 * Frame consumers run on their independent direction task, never under SDK.
 * Context ownership transfers only when construction succeeds. */
h2_pal_result_t h2_gizclaw_req_create_stream_internal(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_rpc_method_t method, const void *tag,
    h2_gizclaw_rpc_bytes_t payload, uint32_t timeout_ms, size_t input_bytes,
    h2_gizclaw_rpc_stream_fn on_frame, void (*destroy)(void *), void *context,
    h2_gizclaw_req_t **out_request);

/* Select direct worker-side consumption for streams whose payload is only
 * counted/validated internally and does not need caller delivery. */
void h2_gizclaw_req_output_optional_internal(h2_gizclaw_req_t *request);
typedef enum h2_gizclaw_stream_lane {
  H2_GIZCLAW_STREAM_AUDIO_UPLINK,
  H2_GIZCLAW_STREAM_AUDIO_DOWNLINK,
  H2_GIZCLAW_STREAM_DATA_UPLINK,
  H2_GIZCLAW_STREAM_DATA_DOWNLINK,
  H2_GIZCLAW_STREAM_LANE_COUNT,
} h2_gizclaw_stream_lane_t;

enum {
  H2_GIZCLAW_STREAM_FRAME_BYTES = 4096u,
  /* The C SDK adds a four-byte frame header. Keep the resulting DataChannel
   * message within the 16 KiB non-interleaved SCTP limit. */
  H2_GIZCLAW_STREAM_INPUT_BYTES = 16u * 1024u - 4u,
  H2_GIZCLAW_STREAM_RING_SLOTS = 8u,
};

typedef struct h2_gizclaw_stream_slot {
  h2_gizclaw_rpc_stream_event_t event;
  size_t bytes;
  size_t output_offset;
  uint8_t payload[H2_GIZCLAW_STREAM_FRAME_BYTES];
} h2_gizclaw_stream_slot_t;

typedef struct h2_gizclaw_stream_ring {
  h2_gizclaw_stream_slot_t slots[H2_GIZCLAW_STREAM_RING_SLOTS];
  size_t read_pos;
  size_t write_pos;
  size_t queued_frames;
  bool dispatch_ready;
} h2_gizclaw_stream_ring_t;

/* The caller must hold service->mutex while inspecting a lane-ready flag. */
bool h2_gizclaw_req_data_ready_internal(h2_gizclaw_service_t *service,
                                        h2_gizclaw_stream_lane_t lane);
bool h2_gizclaw_req_data_step_internal(h2_gizclaw_service_t *service,
                                       h2_gizclaw_stream_lane_t lane);

/* A download sink is admitted once by do. received is a one-shot lane-task
 * notification after successful transport completion and frame validation.
 * It must eventually publish sink_done (possibly from the downlink task).
 * detach quiesces the sink before request/context destruction. */
h2_pal_result_t h2_gizclaw_req_create_sink_stream_internal(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_rpc_method_t method, const void *tag,
    h2_gizclaw_rpc_bytes_t payload, uint32_t timeout_ms,
    h2_gizclaw_rpc_stream_fn on_frame,
    void (*received)(void *, h2_gizclaw_req_t *),
    h2_pal_result_t (*admit)(void *), void (*detach)(void *),
    void (*destroy)(void *), void *context, bool audio_sink,
    h2_gizclaw_req_t **out_request);
void h2_gizclaw_req_sink_done_internal(h2_gizclaw_req_t *request,
                                       h2_pal_result_t result);

/* PCM is supplied only by the Service uplink task, not application code.
 * admit reserves the route; detach waits for in-flight uplink references.
 * Closing input queues protocol EOS after the last accepted slot drains. */
h2_pal_result_t h2_gizclaw_req_create_pcm_stream_internal(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_rpc_method_t method, const void *tag,
    h2_gizclaw_rpc_bytes_t payload, uint32_t timeout_ms,
    h2_gizclaw_rpc_stream_fn on_frame, h2_pal_result_t (*admit)(void *),
    void (*detach)(void *), void (*destroy)(void *), void *context,
    h2_gizclaw_req_t **out_request);
bool h2_gizclaw_req_pcm_ready_internal(h2_gizclaw_req_t *request);
h2_pal_result_t h2_gizclaw_req_pcm_write_internal(h2_gizclaw_req_t *request,
                                                  const uint8_t *data,
                                                  size_t len);
void h2_gizclaw_req_pcm_end_internal(h2_gizclaw_req_t *request,
                                     h2_pal_result_t result);

typedef enum h2_gizclaw_operation_state {
  H2_GIZCLAW_OPERATION_QUEUED = 0,
  H2_GIZCLAW_OPERATION_RUNNING,
  H2_GIZCLAW_OPERATION_PENDING,
  H2_GIZCLAW_OPERATION_COMPLETION_PENDING,
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
  /* Internal publication only; called with the service mutex held. */
  h2_gizclaw_operation_completion_fn settle;
  /* Drop the execution/callback reference outside the service mutex. */
  void (*release_user)(void *user);
  void (*finish)(void *user);
  void *user;
  h2_gizclaw_operation_result_t result;
  atomic_bool terminal;
  h2_gizclaw_operation_state_t state;
  bool cancel_requested;
  bool caller_reference;
  bool internal_reference;
  bool dispatch_queued;
  bool notification_driven;
  bool ready;
  h2_gizclaw_cancel_token_t cancel_token;
  h2_gizclaw_operation_t *next_pending;
  uint64_t trace_sequence;
  uint64_t queued_at_ms;
  uint64_t started_at_ms;
  uint64_t completed_at_ms;
  uint32_t poll_count;
};

typedef enum h2_gizclaw_dispatch_kind {
  H2_GIZCLAW_DISPATCH_OPERATION = 0,
  H2_GIZCLAW_DISPATCH_CLIENT_EVENT,
  H2_GIZCLAW_DISPATCH_NOTIFICATION,
} h2_gizclaw_dispatch_kind_t;

typedef struct h2_gizclaw_dispatch_item {
  h2_gizclaw_dispatch_kind_t kind;
  h2_gizclaw_operation_t *operation;
  h2_gizclaw_client_event_t event;
  char *event_workspace_name;
  void (*notify)(void *user);
  void *notify_user;
} h2_gizclaw_dispatch_item_t;

struct h2_gizclaw_service {
  h2_gizclaw_service_config_t config;
  h2_gizclaw_config_t client_config;
  h2_gizclaw_cancel_fn original_cancel;
  void *original_cancel_user;
  h2_pal_queue_t *request_queue;
  h2_pal_queue_t *dispatch_queue;
  h2_pal_mutex_t *mutex;
  h2_pal_mutex_t *audio_mutex;
  h2_gizclaw_conversation_t *audio_conversation;
  bool audio_ended; /* audio_mutex; repeated button release is harmless. */
  h2_pal_cond_t *progress_cond;
  h2_pal_task_t *net_task;
  h2_pal_task_t *uplink_task;
  h2_pal_task_t *downlink_task;
  h2_pal_task_t *data_uplink_task;
  h2_pal_task_t *data_downlink_task;
  /* One active target per independent source/direction; never a request queue.
   */
  struct h2_gizclaw_managed_request *audio_uplink_stream;
  struct h2_gizclaw_managed_request *audio_downlink_stream;
  struct h2_gizclaw_managed_request *data_uplink_stream;
  struct h2_gizclaw_managed_request *data_downlink_stream;
  /* Allocated once with Service. A lane ring is reset only while its active
   * target slot is empty, so steady-state chunk transport never allocates. */
  h2_gizclaw_stream_ring_t stream_rings[H2_GIZCLAW_STREAM_LANE_COUNT];
  struct h2_gizclaw_audio_play *audio_play; /* Protected by mutex. */
  _Atomic(h2_gizclaw_conversation_request_t *) media_request;
  _Atomic(struct h2_gizclaw_speech_context *) speech_request;
  _Atomic(h2_gizclaw_track_t *) pcm_track;
  /* Protected by mutex. Unset closes admission before waiting for callbacks. */
  size_t pcm_track_refs;
  bool pcm_track_unsetting;
  h2_pal_webrtc_track_vtable_t webrtc_track_vtable;
  h2_pal_webrtc_track_t webrtc_track;
  atomic_uint media_callback_refs;
  atomic_int media_holder_tag; /* source line of the last acquirer */
  h2_gizclaw_client_t *client;
  h2_gizclaw_operation_t *current;
  h2_gizclaw_operation_t *pending;
  size_t active_count;
  size_t caller_reference_count;
  size_t request_reference_count;
  size_t queued_event_count;
  size_t dispatch_item_count;
  uint64_t next_trace_sequence;
  atomic_bool runtime_event_armed;
  bool started;
  bool stopping;
  bool stopped;
  bool dispatching;
  bool terminal_pending;
  bool terminal_dispatched;
  h2_pal_result_t terminal_result;
};

/* Coalesced level-to-edge bridge for the optional Runtime event queue. */
void h2_gizclaw_service_wake_dispatch_internal(
    h2_gizclaw_service_t *service);

#ifdef H2_GIZCLAW_TESTING
typedef h2_pal_result_t (*h2_gizclaw_runtime_post_test_fn)(
    h2_runtime_t *runtime, const h2_runtime_custom_event_t *event);
void h2_gizclaw_service_test_set_runtime_post(
    h2_gizclaw_runtime_post_test_fn post);
#endif

/* Dispatch one pending data-down write on the app poll task. */
bool h2_gizclaw_req_dispatch_output_internal(h2_gizclaw_service_t *service);

h2_pal_result_t
h2_gizclaw_service_pcm_read_internal(h2_gizclaw_service_t *service,
                                     uint8_t *pcm, size_t capacity,
                                     size_t *out_len);
h2_pal_result_t
h2_gizclaw_service_pcm_write_internal(h2_gizclaw_service_t *service,
                                      const uint8_t *pcm, size_t len);
/* A new audio request, or a cancel, discards downlink PCM still queued from
 * the previous request so its tail cannot play over the next turn. */
void h2_gizclaw_service_pcm_discard_downlink_internal(
    h2_gizclaw_service_t *service);
bool h2_gizclaw_service_pcm_downlink_stats_internal(
    h2_gizclaw_service_t *service, size_t *out_used, size_t *out_capacity);
bool h2_gizclaw_service_pcm_readable_internal(h2_gizclaw_service_t *service);
typedef enum h2_gizclaw_pcm_input_action {
  H2_GIZCLAW_PCM_INPUT_START,
  H2_GIZCLAW_PCM_INPUT_END,
  H2_GIZCLAW_PCM_INPUT_PREPARE,
  H2_GIZCLAW_PCM_INPUT_READ,
} h2_gizclaw_pcm_input_action_t;
h2_pal_result_t h2_gizclaw_service_pcm_input_internal(
    h2_gizclaw_service_t *service, h2_gizclaw_pcm_input_t *input,
    h2_gizclaw_pcm_input_action_t action, uint8_t *pcm, size_t len,
    size_t *out_len);
h2_pal_result_t h2_gizclaw_speech_audio_start_internal(void *context);
h2_pal_result_t h2_gizclaw_speech_audio_end_internal(void *context);
void h2_gizclaw_speech_uplink_step_internal(h2_gizclaw_service_t *service);
void h2_gizclaw_conversation_uplink_step_internal(
    h2_gizclaw_service_t *service);
void h2_gizclaw_conversation_downlink_step_internal(
    h2_gizclaw_service_t *service);
void h2_gizclaw_audio_play_downlink_step_internal(
    h2_gizclaw_service_t *service);
h2_pal_result_t h2_gizclaw_audio_play_create_internal(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t workspace, h2_gizclaw_str_t history,
    h2_gizclaw_rpc_bytes_t payload, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request);

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

/* Bounded, nonblocking FIFO delivery on service_poll(). The payload owner must
 * retain user until notify returns, including after stop. An operation can
 * safely retain it until its later completion, on this same FIFO. WOULD_BLOCK
 * means nothing was queued; retry without replacing the retained payload. */
h2_pal_result_t h2_gizclaw_service_post_internal(h2_gizclaw_service_t *service,
                                                 void (*notify)(void *user),
                                                 void *user);

/* NULL poll selects one-shot delivery: start's result is terminal, including
 * WOULD_BLOCK. A non-NULL poll retains the existing pending-request contract.
 */
h2_pal_result_t h2_gizclaw_service_submit_request_internal(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_operation_run_fn start, h2_gizclaw_operation_run_fn poll,
    h2_gizclaw_operation_completion_fn settle,
    h2_gizclaw_operation_completion_fn completion,
    void (*release_user)(void *user), void (*finish)(void *user), void *user,
    bool notification_driven,
    h2_gizclaw_operation_t **out_operation);

/* Each distinct public parser uses a distinct static tag, even when two
 * requests share the same wire method (e.g. the two profile updates). */
h2_pal_result_t h2_gizclaw_req_create_rpc_internal(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_rpc_method_t method, const void *tag,
    h2_gizclaw_rpc_bytes_t payload, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request);
h2_pal_result_t h2_gizclaw_req_response_internal(
    const h2_gizclaw_req_t *request, const void *tag,
    const h2_gizclaw_rpc_response_t **out_response);
h2_pal_result_t h2_gizclaw_req_elapsed_internal(const h2_gizclaw_req_t *request,
                                                const void *tag,
                                                uint64_t *out_elapsed_ms);
h2_pal_result_t
h2_gizclaw_req_input_internal(const h2_gizclaw_req_t *request, const void *tag,
                              h2_gizclaw_rpc_bytes_t *out_input);

int h2_gizclaw_rpc_start_internal(h2_gizclaw_client_t *client,
                                  h2_gizclaw_rpc_method_t method,
                                  h2_gizclaw_rpc_bytes_t payload,
                                  uint32_t timeout_ms,
                                  h2_gizclaw_rpc_request_t **out_request);
int h2_gizclaw_rpc_result_internal(h2_gizclaw_rpc_request_t *request,
                                   h2_gizclaw_rpc_response_t *out_response);
bool h2_gizclaw_rpc_set_complete_internal(
    h2_gizclaw_rpc_request_t *request, h2_gizclaw_rpc_complete_fn on_complete,
    void *user);
int h2_gizclaw_rpc_start_stream_internal(
    h2_gizclaw_client_t *client, h2_gizclaw_rpc_method_t method,
    h2_gizclaw_rpc_bytes_t payload, uint32_t timeout_ms,
    h2_gizclaw_rpc_stream_fn on_event, void *user,
    h2_gizclaw_rpc_request_t **out_request);
int h2_gizclaw_rpc_finish_write_internal(h2_gizclaw_rpc_request_t *request);
int h2_gizclaw_rpc_write_internal(h2_gizclaw_rpc_request_t *request,
                                  const uint8_t *data, size_t len);
void h2_gizclaw_rpc_cancel_internal(h2_gizclaw_rpc_request_t *request);
void h2_gizclaw_rpc_destroy_internal(h2_gizclaw_rpc_request_t *request);

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
                                       h2_gizclaw_client_event_sink_fn on_event,
                                       void *event_user);
  h2_pal_result_t (*dispatch_event)(h2_gizclaw_client_t *client);
  h2_pal_result_t (*close)(h2_gizclaw_client_t *client);
  void (*deinit)(h2_gizclaw_client_t *client);
} h2_gizclaw_service_client_ops_t;

void h2_gizclaw_service_test_set_client_ops(
    const h2_gizclaw_service_client_ops_t *ops);

typedef struct h2_gizclaw_async_rpc_ops {
  int (*start_stream)(h2_gizclaw_client_t *client,
                      h2_gizclaw_rpc_method_t method,
                      h2_gizclaw_rpc_bytes_t payload, uint32_t timeout_ms,
                      h2_gizclaw_rpc_stream_fn on_event, void *user,
                      h2_gizclaw_rpc_request_t **out_request);
  int (*finish_write)(h2_gizclaw_rpc_request_t *request);
  int (*write)(h2_gizclaw_rpc_request_t *request, const uint8_t *data,
               size_t len);
  int (*start)(h2_gizclaw_client_t *client, h2_gizclaw_rpc_method_t method,
               h2_gizclaw_rpc_bytes_t params_payload, uint32_t timeout_ms,
               h2_gizclaw_rpc_request_t **out_request);
  int (*result)(h2_gizclaw_rpc_request_t *request,
                h2_gizclaw_rpc_response_t *out_response);
  void (*cancel)(h2_gizclaw_rpc_request_t *request);
  void (*destroy)(h2_gizclaw_rpc_request_t *request);
  void (*set_complete)(h2_gizclaw_rpc_request_t *request,
                       h2_gizclaw_rpc_complete_fn on_complete, void *user);
} h2_gizclaw_async_rpc_ops_t;

void h2_gizclaw_async_rpc_test_set_ops(const h2_gizclaw_async_rpc_ops_t *ops);

#endif

#endif
