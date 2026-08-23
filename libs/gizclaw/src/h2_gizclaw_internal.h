#ifndef H2_GIZCLAW_INTERNAL_H
#define H2_GIZCLAW_INTERNAL_H

#include "h2_gizclaw_config.h"
#include "h2_gizclaw_rpc.h"
#include "h2_gizclaw_types.h"
#include "h2/pal/os/h2_pal_mem.h"

#include "gzc_buffer.h"
#include "gzc_event.h"

typedef struct gzc_client gzc_client_t;
typedef struct h2_gizclaw_conversation h2_gizclaw_conversation_t;

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
gzc_client_t *h2_gizclaw_client_gzc_internal(h2_gizclaw_client_t *client);
void h2_gizclaw_client_log_rpc_error_internal(
    h2_gizclaw_client_t *client, h2_gizclaw_rpc_method_t method,
    int error_code, const char *error_message, size_t error_message_len);
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

typedef int (*h2_gizclaw_test_rpc_call_fn)(
    void *user, h2_gizclaw_client_t *client, h2_gizclaw_rpc_method_t method,
    h2_gizclaw_rpc_bytes_t params_payload,
    h2_gizclaw_rpc_response_t *out_response);

void h2_gizclaw_test_set_rpc_call(h2_gizclaw_test_rpc_call_fn call, void *user);

typedef int (*h2_gizclaw_test_rpc_call_stream_fn)(
    void *user, h2_gizclaw_client_t *client, h2_gizclaw_rpc_method_t method,
    h2_gizclaw_rpc_bytes_t params_payload, h2_gizclaw_rpc_stream_fn on_event,
    void *event_user);

void h2_gizclaw_test_set_rpc_call_stream(
    h2_gizclaw_test_rpc_call_stream_fn call, void *user);

typedef struct h2_gizclaw_test_speed_test_request {
  int64_t up_content_length;
  int64_t down_content_length;
} h2_gizclaw_test_speed_test_request_t;

typedef struct h2_gizclaw_test_speed_test_result {
  int64_t up_bytes;
  int64_t down_bytes;
  int64_t up_duration_ms;
  int64_t down_duration_ms;
} h2_gizclaw_test_speed_test_result_t;

typedef int (*h2_gizclaw_test_speed_test_fn)(
    void *user, h2_gizclaw_client_t *client,
    const h2_gizclaw_test_speed_test_request_t *request,
    h2_gizclaw_test_speed_test_result_t *out_result);
void h2_gizclaw_test_set_speed_test(h2_gizclaw_test_speed_test_fn speed_test,
                                    void *user);

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
typedef int (*h2_gizclaw_test_conversation_packet_send_fn)(
    void *user, gzc_client_t *client, uint8_t protocol, const uint8_t *payload,
    size_t payload_len);
typedef int (*h2_gizclaw_test_conversation_encode_fn)(void *user, void *encoder,
                                                      const int16_t *pcm,
                                                      int frame_samples,
                                                      uint8_t *opus,
                                                      int opus_capacity);
void h2_gizclaw_test_set_event_ops(h2_gizclaw_test_event_send_fn send,
                                   h2_gizclaw_test_event_read_fn read,
                                   h2_gizclaw_test_event_close_fn close,
                                   void *user);
void h2_gizclaw_test_set_packet_read(h2_gizclaw_test_packet_read_fn read,
                                     void *user);
void h2_gizclaw_test_set_client_poll(h2_gizclaw_test_client_poll_fn poll,
                                     void *user);
void h2_gizclaw_test_set_conversation_ops(
    h2_gizclaw_test_conversation_packet_send_fn packet_send,
    h2_gizclaw_test_conversation_encode_fn encode, void *user);

gzc_event_stream_t *
h2_gizclaw_test_replace_event_stream(h2_gizclaw_client_t *client,
                                     gzc_event_stream_t *events);
bool h2_gizclaw_test_client_terminal_closed(const h2_gizclaw_client_t *client);
bool h2_gizclaw_test_peer_event_matches_stream(const gzc_peer_event_t *event,
                                               const char *stream_id);
bool h2_gizclaw_test_peer_event_matches_conversation(
    const gzc_peer_event_t *event, const char *input_stream_id,
    char *response_stream_id, char *transcript_stream_id,
    char *assistant_stream_id, size_t response_stream_id_capacity);

typedef int (*h2_gizclaw_test_telemetry_send_fn)(
    void *user, const gzc_telemetry_frame_t *frame);
void h2_gizclaw_test_set_telemetry_send(h2_gizclaw_test_telemetry_send_fn send,
                                        void *user);

bool h2_gizclaw_test_media_registered(h2_gizclaw_client_t *client);
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
