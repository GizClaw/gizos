#ifndef H2_GIZCLAW_INTERNAL_H
#define H2_GIZCLAW_INTERNAL_H

#include "h2/pal/os/h2_pal_mem.h"
#include "h2_gizclaw_client.h"
#include "h2_gizclaw_config.h"
#include "h2_gizclaw_conversation.h"
#include "h2_gizclaw_rpc.h"
#include "h2_gizclaw_types.h"

#include "gzc_buffer.h"
#include "gzc_event.h"
#include "pb.h"

/* Normalize only a canonical NOT_FOUND status. UNIMPLEMENTED, any other
 * status code, or a transport failure is not resource absence. */
static inline h2_pal_result_t
h2_gizclaw_rpc_error_result_internal(int error_code) {
  return error_code == H2_GIZCLAW_RPC_ERROR_NOT_FOUND ? H2_PAL_ERR_NOT_FOUND
                                                      : H2_GIZCLAW_ERR_REMOTE;
}

/** Owned unary result. The managed request frees both buffers with its PAL
 * allocator. */
typedef struct h2_gizclaw_rpc_response {
  uint8_t *result_payload;
  size_t result_payload_len;
  bool has_error;
  int error_code;
  char *error_message;
  size_t error_message_len;
} h2_gizclaw_rpc_response_t;

typedef struct h2_gizclaw_rpc_request h2_gizclaw_rpc_request_t;
typedef void (*h2_gizclaw_rpc_complete_fn)(void *user,
                                           h2_pal_result_t result);

typedef enum h2_gizclaw_rpc_stream_event_kind {
  H2_GIZCLAW_RPC_STREAM_RESPONSE = 1,
  H2_GIZCLAW_RPC_STREAM_DATA,
  H2_GIZCLAW_RPC_STREAM_EOS,
} h2_gizclaw_rpc_stream_event_kind_t;

/** Views are borrowed and valid only for the duration of the callback.
 * Return PAL results, not SDK status codes. Frames cannot be replayed;
 * WOULD_BLOCK (or a positive result) terminates the RPC with ERR_IO. */
typedef struct h2_gizclaw_rpc_stream_event {
  h2_gizclaw_rpc_stream_event_kind_t kind;
  h2_gizclaw_rpc_bytes_t result_payload;
  h2_gizclaw_rpc_bytes_t data;
  bool has_error;
  int error_code;
  h2_gizclaw_rpc_bytes_t error_message;
  /* Snapshot supplied by the managed stream at ingress, before deferral. */
  size_t input_bytes;
  bool input_finished;
} h2_gizclaw_rpc_stream_event_t;

typedef int (*h2_gizclaw_rpc_stream_fn)(
    void *user, const h2_gizclaw_rpc_stream_event_t *event);

/**
 * Start one request-owned unary RPC on the connected client's Peer service.
 *
 * Calls that start requests, poll the client, inspect results, cancel, and
 * destroy requests must be serialized by one caller. The client and its PAL
 * configuration must outlive every request. The payload is borrowed only
 * until this function returns.
 */
int h2_gizclaw_client_rpc_request_start(h2_gizclaw_client_t *client,
                                        h2_gizclaw_rpc_method_t method,
                                        h2_gizclaw_rpc_bytes_t params_payload,
                                        uint32_t timeout_ms,
                                        h2_gizclaw_rpc_request_t **out_request);

/** Start one mixed-frame RPC advanced exclusively by client poll. */
int h2_gizclaw_client_rpc_request_start_stream(
    h2_gizclaw_client_t *client, h2_gizclaw_rpc_method_t method,
    h2_gizclaw_rpc_bytes_t params_payload, uint32_t timeout_ms,
    h2_gizclaw_rpc_stream_fn on_event, void *user,
    h2_gizclaw_rpc_request_t **out_request);

/** Copy and queue one binary request frame without polling. */
int h2_gizclaw_rpc_request_write(h2_gizclaw_rpc_request_t *request,
                                 const uint8_t *data, size_t len);

/** Queue request EOS without polling. */
int h2_gizclaw_rpc_request_finish_write(h2_gizclaw_rpc_request_t *request);

/**
 * Inspect one request without polling.
 *
 * Returns H2_PAL_ERR_WOULD_BLOCK while pending. A successful response is an
 * owned copy; the managed request frees both buffers with its PAL allocator. A
 * remote RPC error remains a successful transport result with has_error set.
 */
int h2_gizclaw_rpc_request_result(h2_gizclaw_rpc_request_t *request,
                                  h2_gizclaw_rpc_response_t *out_response);

/** Install an internal terminal notification hook before the next client poll. */
void h2_gizclaw_rpc_request_set_complete_handler(
    h2_gizclaw_rpc_request_t *request, h2_gizclaw_rpc_complete_fn on_complete,
    void *user);

/** Idempotently cancel a pending request. */
void h2_gizclaw_rpc_request_cancel(h2_gizclaw_rpc_request_t *request);

/** Cancel if needed and consume the request handle. NULL is a no-op. */
void h2_gizclaw_rpc_request_destroy(h2_gizclaw_rpc_request_t *request);

typedef struct gzc_client gzc_client_t;
typedef struct h2_gizclaw_conversation h2_gizclaw_conversation_t;

int h2_gizclaw_client_init(const h2_gizclaw_config_t *config,
                           h2_gizclaw_client_t **out_client);
int h2_gizclaw_client_connect(h2_gizclaw_client_t *client);
int h2_gizclaw_client_poll(h2_gizclaw_client_t *client, int timeout_ms);
int h2_gizclaw_client_dispatch_event(h2_gizclaw_client_t *client,
                                     int timeout_ms,
                                     h2_gizclaw_client_event_fn on_event,
                                     void *event_user);
/* A sink may decline an event with WOULD_BLOCK; the client retains and retries
 * it before reading another Event frame, while transport polling continues. */
typedef h2_pal_result_t (*h2_gizclaw_client_event_sink_fn)(
    void *user, const h2_gizclaw_client_event_t *event);
int h2_gizclaw_client_set_event_handler(
    h2_gizclaw_client_t *client, h2_gizclaw_client_event_sink_fn on_event,
    void *event_user);
int h2_gizclaw_client_close(h2_gizclaw_client_t *client);
void h2_gizclaw_client_deinit(h2_gizclaw_client_t *client);

static inline bool
h2_gizclaw_runtime_alias_valid_internal(h2_gizclaw_str_t value) {
  if (value.data == NULL || value.len == 0u || value.len > 63u)
    return false;
  bool expects_alnum = true;
  for (size_t index = 0u; index < value.len; ++index) {
    const char ch = value.data[index];
    const bool alnum = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
    if (alnum) {
      expects_alnum = false;
    } else if ((ch == '-' || ch == '.') && !expects_alnum) {
      expects_alnum = true;
    } else {
      return false;
    }
  }
  return !expects_alnum;
}

int h2_gizclaw_provider_result_to_gzc(int result);

const h2_pal_mem_api_t *
h2_gizclaw_client_allocator_internal(h2_gizclaw_client_t *client);
int h2_gizclaw_client_monotonic_ms_internal(h2_gizclaw_client_t *client,
                                            uint64_t *out_ms);
gzc_client_t *h2_gizclaw_client_gzc_internal(h2_gizclaw_client_t *client);
void h2_gizclaw_client_log_rpc_error_internal(h2_gizclaw_client_t *client,
                                              h2_gizclaw_rpc_method_t method,
                                              int error_code,
                                              const char *error_message,
                                              size_t error_message_len);
/* Network-task-only Event lease and BOS/EOS protocol implementation.
 * Audio packets use the PAL Track exclusively; these are not user APIs.
 * A WOULD_BLOCK open retains its lease in out_conversation for BOS retry;
 * the network owner must destroy it on every terminal path. */
int h2_gizclaw_conversation_wire_open_internal(
    h2_gizclaw_client_t *client, h2_gizclaw_str_t workspace_name,
    uint64_t generation, int timeout_ms,
    h2_gizclaw_conversation_t **out_conversation);
bool h2_gizclaw_conversation_wire_input_ready_internal(
    const h2_gizclaw_conversation_t *conversation);
int h2_gizclaw_conversation_wire_finish_input_internal(
    h2_gizclaw_conversation_t *conversation, uint64_t timestamp_ms);
int h2_gizclaw_conversation_wire_poll_internal(
    h2_gizclaw_conversation_t *conversation, int timeout_ms,
    h2_gizclaw_conversation_event_t *out_event);
void h2_gizclaw_conversation_wire_destroy_internal(
    h2_gizclaw_conversation_t *conversation);

int h2_gizclaw_client_conversation_acquire_internal(
    h2_gizclaw_client_t *client, h2_gizclaw_conversation_t *conversation,
    gzc_event_stream_t **out_events, uint64_t *out_stream_sequence);
bool h2_gizclaw_client_conversation_active_internal(
    const h2_gizclaw_client_t *client,
    const h2_gizclaw_conversation_t *conversation);
void h2_gizclaw_client_conversation_release_internal(
    h2_gizclaw_client_t *client, h2_gizclaw_conversation_t *conversation);
void h2_gizclaw_client_event_failure_internal(
    h2_gizclaw_client_t *client, h2_gizclaw_conversation_t *conversation);
void h2_gizclaw_client_event_stream_failure_internal(
    h2_gizclaw_client_t *client);
void h2_gizclaw_conversation_invalidate_internal(
    h2_gizclaw_conversation_t *conversation);
bool h2_gizclaw_conversation_accepts_peer_event_internal(
    h2_gizclaw_conversation_t *conversation, const gzc_peer_event_t *event);
/* Formats the reply-route state a peer event lands on, for diagnostics. */
void h2_gizclaw_conversation_describe_peer_event_internal(
    const h2_gizclaw_conversation_t *conversation,
    const gzc_peer_event_t *event, char *out, size_t cap);
bool h2_gizclaw_conversation_has_pending_peer_event_internal(
    const h2_gizclaw_conversation_t *conversation);
void h2_gizclaw_conversation_enqueue_peer_event_internal(
    h2_gizclaw_conversation_t *conversation, const gzc_peer_event_t *event);
int h2_gizclaw_event_stream_send_internal(gzc_event_stream_t *stream,
                                          const gzc_peer_event_t *event);
int h2_gizclaw_event_stream_read_internal(gzc_event_stream_t *stream,
                                          int timeout_ms,
                                          gzc_peer_event_t *out_event);
int h2_gizclaw_client_read_packet_internal(gzc_client_t *client, int timeout_ms,
                                           uint8_t *out_protocol,
                                           gzc_buf_t *out_payload);
#if defined(H2_GIZCLAW_TESTING)
#include "gzc_telemetry.h"
#include "platform/gzc_platform_webrtc.h"

int h2_gizclaw_test_stream_failure_result(int pal_error, bool eos,
                                          int *out_sdk_result);
typedef int (*h2_gizclaw_test_event_send_fn)(void *user,
                                             gzc_event_stream_t *stream,
                                             const gzc_peer_event_t *event);
typedef int (*h2_gizclaw_test_event_read_fn)(void *user,
                                             gzc_event_stream_t *stream,
                                             int timeout_ms,
                                             gzc_peer_event_t *out_event);
typedef void (*h2_gizclaw_test_event_close_fn)(void *user,
                                               gzc_event_stream_t *stream);
typedef int (*h2_gizclaw_test_packet_read_fn)(void *user, gzc_client_t *client,
                                              int timeout_ms,
                                              uint8_t *out_protocol,
                                              gzc_buf_t *out_payload);
typedef int (*h2_gizclaw_test_client_poll_fn)(void *user, gzc_client_t *client,
                                              int timeout_ms);
void h2_gizclaw_test_set_event_ops(h2_gizclaw_test_event_send_fn send,
                                   h2_gizclaw_test_event_read_fn read,
                                   h2_gizclaw_test_event_close_fn close,
                                   void *user);
void h2_gizclaw_test_set_packet_read(h2_gizclaw_test_packet_read_fn read,
                                     void *user);
void h2_gizclaw_test_set_client_poll(h2_gizclaw_test_client_poll_fn poll,
                                     void *user);
bool h2_gizclaw_test_audio_rings(void);

gzc_event_stream_t *
h2_gizclaw_test_replace_event_stream(h2_gizclaw_client_t *client,
                                     gzc_event_stream_t *events);
bool h2_gizclaw_test_client_terminal_closed(const h2_gizclaw_client_t *client);

typedef int (*h2_gizclaw_test_telemetry_send_fn)(
    void *user, const gzc_telemetry_frame_t *frame);
struct h2_gizclaw_telemetry_frame;
struct h2_gizclaw_workflow_page;
struct h2_gizclaw_workspace_activation;
struct h2_gizclaw_workspace_history_page;
int h2_gizclaw_workspace_decode_activation_for_test(
    h2_gizclaw_resp_storage_t *storage, const uint8_t *data, size_t len,
    struct h2_gizclaw_workspace_activation *out_result);
int h2_gizclaw_workspace_decode_history_list_for_test(
    h2_gizclaw_resp_storage_t *storage, const uint8_t *data, size_t len,
    size_t max_count, struct h2_gizclaw_workspace_history_page *out_result);
int h2_gizclaw_workflow_decode_list_for_test(
    h2_gizclaw_resp_storage_t *storage, const uint8_t *data, size_t len,
    size_t max_count, struct h2_gizclaw_workflow_page *out_page);
int h2_gizclaw_test_telemetry_send(
    h2_gizclaw_client_t *client,
    const struct h2_gizclaw_telemetry_frame *frame);
void h2_gizclaw_test_set_telemetry_send(h2_gizclaw_test_telemetry_send_fn send,
                                        void *user);

bool h2_gizclaw_test_media_registered(h2_gizclaw_client_t *client);
int h2_gizclaw_test_peer_create(h2_gizclaw_client_t *client,
                                h2_pal_webrtc_peer_t **out_peer);
int h2_gizclaw_test_peer_poll(h2_gizclaw_client_t *client,
                              h2_pal_webrtc_peer_t *peer, int timeout_ms);
int h2_gizclaw_test_media_send_opus(h2_gizclaw_client_t *client,
                                    gzc_rtc_peer_t *peer, const uint8_t *opus,
                                    size_t opus_len);
int h2_gizclaw_test_media_set_opus_frame_callback(
    h2_gizclaw_client_t *client, gzc_rtc_peer_t *peer,
    gzc_rtc_opus_frame_cb callback, void *callback_user);
void h2_gizclaw_test_media_emit_opus(h2_gizclaw_client_t *client,
                                     gzc_rtc_peer_t *peer, const uint8_t *opus,
                                     size_t opus_len);
void h2_gizclaw_test_media_close_peer(h2_gizclaw_client_t *client,
                                      gzc_rtc_peer_t *peer);
#endif
#endif
