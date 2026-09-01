#include "h2_gizclaw_client.h"
#include "h2_gizclaw_internal.h"

#include "gzc.h"
#include "payload/system.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define H2_GIZCLAW_ASSERT_RPC_METHOD(h2_name, gzc_name)                        \
  _Static_assert((int)(h2_name) == (int)(gzc_name),                            \
                 "GizClaw RPC method registry drift: " #h2_name)
#define H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(name)                                \
  H2_GIZCLAW_ASSERT_RPC_METHOD(H2_GIZCLAW_RPC_##name,                          \
                               gizclaw_rpc_v1_RpcMethod_RPC_METHOD_##name)

H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(CLIENT_INFO_GET);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(ALL_PING);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(ALL_SPEED_TEST_RUN);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(CLIENT_IDENTIFIERS_GET);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_INFO_GET);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_INFO_PUT);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_RUN_WORKSPACE_GET);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_RUN_WORKSPACE_SET);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_RUN_WORKSPACE_RELOAD);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_FIRMWARE_GET);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_WORKSPACE_LIST);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_WORKSPACE_GET);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_WORKSPACE_CREATE);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_WORKSPACE_PUT);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_WORKSPACE_DELETE);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_WORKSPACE_HISTORY_LIST);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_WORKSPACE_HISTORY_GET);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_WORKSPACE_HISTORY_AUDIO_DOWNLOAD);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_WORKFLOW_LIST);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_WORKFLOW_GET);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_CONTACT_LIST);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_CONTACT_GET);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_CONTACT_CREATE);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_CONTACT_PUT);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_CONTACT_DELETE);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_FRIEND_INVITE_TOKEN_GET);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_FRIEND_INVITE_TOKEN_CREATE);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_FRIEND_INVITE_TOKEN_CLEAR);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_FRIEND_ADD);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_FRIEND_LIST);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_FRIEND_DELETE);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_FRIEND_GROUP_LIST);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_FRIEND_GROUP_GET);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_FRIEND_GROUP_CREATE);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_FRIEND_GROUP_PUT);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_FRIEND_GROUP_DELETE);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_FRIEND_GROUP_INVITE_TOKEN_GET);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_FRIEND_GROUP_INVITE_TOKEN_CREATE);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_FRIEND_GROUP_INVITE_TOKEN_CLEAR);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_FRIEND_GROUP_JOIN);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_FRIEND_GROUP_MEMBERS_LIST);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_FRIEND_GROUP_MEMBERS_ADD);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_FRIEND_GROUP_MEMBERS_PUT);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_FRIEND_GROUP_MEMBERS_DELETE);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_FRIEND_GROUP_MESSAGES_LIST);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_FRIEND_GROUP_MESSAGES_GET);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_PET_LIST);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_PET_GET);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(RUNTIME_ADOPT);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_PET_PUT);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_PET_DELETE);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_PET_DRIVE);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_POINTS_GET);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_POINTS_TRANSACTIONS_LIST);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_POINTS_TRANSACTIONS_GET);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(CLIENT_TOOL_INVOKE);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_PET_ACTIONS_GET);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_PET_PIXA_DOWNLOAD);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_FRIEND_INFO_GET);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_REGISTER);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_SPEECH_TRANSCRIBE);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_SPEECH_SYNTHESIZE);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_PEER_DELETE);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_SPEECH_EXTRACT);
H2_GIZCLAW_ASSERT_RPC_METHOD_SAME(SERVER_FRIEND_GROUP_MESSAGES_AUDIO_DOWNLOAD);
#undef H2_GIZCLAW_ASSERT_RPC_METHOD_SAME
#undef H2_GIZCLAW_ASSERT_RPC_METHOD

#define H2_GIZCLAW_LOCAL_CHANNEL_LABEL_CAPACITY 64u
#define H2_GIZCLAW_WRITE_POLL_BACKOFF_MS 10u
#define H2_GIZCLAW_WRITE_POLL_SLICE_MS 50u

typedef struct h2_gizclaw_local_channel_state {
  bool create_in_progress;
  gzc_str_t create_label;
  bool create_ordered;
  bool create_reliable;
  bool ready;
  h2_pal_webrtc_peer_t *peer;
  h2_pal_webrtc_channel_t *channel;
  h2_pal_webrtc_channel_info_t info;
  h2_pal_webrtc_channel_state_t state;
  char label[H2_GIZCLAW_LOCAL_CHANNEL_LABEL_CAPACITY];
  bool has_info;
} h2_gizclaw_local_channel_state_t;

struct h2_gizclaw_client {
  struct h2_gizclaw_client *next_client;
  h2_gizclaw_config_t config;
  gzc_platform_t platform;
  gzc_platform_crypto_t crypto;
  gzc_http_vtable_t http;
  gzc_webrtc_vtable_t webrtc;
  gzc_webrtc_media_vtable_t media;
  gzc_webrtc_callbacks_t gzc_callbacks;
  gzc_rtc_opus_frame_cb opus_frame_callback;
  void *opus_frame_callback_user;
  gzc_rtc_peer_t *opus_frame_peer;
  gzc_rtc_channel_t *local_channels[GZC_RPC_MAX_INBOUND_CHANNELS + 2u];
  gzc_rtc_channel_t *remote_service_channels[GZC_RPC_MAX_INBOUND_CHANNELS];
  h2_pal_webrtc_peer_t *webrtc_peer;
  h2_gizclaw_local_channel_state_t local_channel_state;
  gzc_client_t *gzc;
  gzc_event_stream_t *events;
  h2_gizclaw_conversation_t *active_conversation;
  h2_gizclaw_client_event_fn event_handler;
  void *event_handler_user;
  h2_gizclaw_rpc_call_interceptor_fn rpc_call_interceptor;
  h2_gizclaw_rpc_stream_interceptor_fn rpc_stream_interceptor;
  void *rpc_interceptor_user;
  uint64_t next_conversation_stream_sequence;
  bool terminal_closed;
};

struct h2_gizclaw_rpc_request {
  const h2_pal_mem_api_t *allocator;
  h2_gizclaw_client_t *client;
  gzc_rpc_request_t *gzc;
  h2_gizclaw_rpc_stream_fn on_stream_event;
  void *stream_user;
  bool saw_stream_response;
  bool saw_stream_eos;
};

static h2_gizclaw_client_t *s_clients;

#if defined(H2_GIZCLAW_TESTING)
static h2_gizclaw_client_t *s_test_webrtc_client;
#endif

static h2_pal_result_t h2_gizclaw_result_from_gzc(int result);

static h2_gizclaw_client_t *h2_gizclaw_client_for_peer(gzc_rtc_peer_t *peer) {
  if (peer == NULL) {
    return NULL;
  }
  for (h2_gizclaw_client_t *client = s_clients; client != NULL;
       client = client->next_client) {
    if (client->webrtc_peer == (h2_pal_webrtc_peer_t *)peer) {
      return client;
    }
  }
#if defined(H2_GIZCLAW_TESTING)
  return s_test_webrtc_client;
#else
  return NULL;
#endif
}

#if defined(H2_GIZCLAW_TESTING)
static h2_gizclaw_test_event_send_fn s_test_event_send;
static h2_gizclaw_test_event_read_fn s_test_event_read;
static h2_gizclaw_test_event_close_fn s_test_event_close;
static void *s_test_event_user;
static h2_gizclaw_test_packet_read_fn s_test_packet_read;
static void *s_test_packet_read_user;
static h2_gizclaw_test_client_poll_fn s_test_client_poll;
static void *s_test_client_poll_user;
#endif

const h2_pal_mem_api_t *
h2_gizclaw_client_allocator_internal(h2_gizclaw_client_t *client) {
  return client != NULL ? client->config.allocator : NULL;
}

int h2_gizclaw_client_monotonic_ms_internal(h2_gizclaw_client_t *client,
                                            uint64_t *out_ms) {
  if (client == NULL || out_ms == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  return h2_pal_time_get_monotonic_ms(client->config.time, out_ms);
}

gzc_client_t *h2_gizclaw_client_gzc_internal(h2_gizclaw_client_t *client) {
  return client != NULL ? client->gzc : NULL;
}

int h2_gizclaw_event_stream_send_internal(gzc_event_stream_t *stream,
                                          const gzc_peer_event_t *event) {
#if defined(H2_GIZCLAW_TESTING)
  if (s_test_event_send != NULL) {
    return s_test_event_send(s_test_event_user, stream, event);
  }
#endif
  return gzc_event_stream_send(stream, event);
}

int h2_gizclaw_event_stream_read_internal(gzc_event_stream_t *stream,
                                          int timeout_ms,
                                          gzc_peer_event_t *out_event) {
#if defined(H2_GIZCLAW_TESTING)
  if (s_test_event_read != NULL) {
    return s_test_event_read(s_test_event_user, stream, timeout_ms, out_event);
  }
#endif
  return gzc_event_stream_read(stream, timeout_ms, out_event);
}

int h2_gizclaw_client_read_packet_internal(gzc_client_t *client, int timeout_ms,
                                           uint8_t *out_protocol,
                                           gzc_buf_t *out_payload) {
#if defined(H2_GIZCLAW_TESTING)
  if (s_test_packet_read != NULL) {
    return s_test_packet_read(s_test_packet_read_user, client, timeout_ms,
                              out_protocol, out_payload);
  }
#endif
  return gzc_client_read_packet(client, timeout_ms, out_protocol, out_payload);
}

static void h2_gizclaw_event_stream_close_internal(gzc_event_stream_t *stream) {
  if (stream == NULL) {
    return;
  }
#if defined(H2_GIZCLAW_TESTING)
  if (s_test_event_close != NULL) {
    s_test_event_close(s_test_event_user, stream);
    return;
  }
#endif
  gzc_event_stream_close(stream);
}

int h2_gizclaw_client_conversation_acquire_internal(
    h2_gizclaw_client_t *client, h2_gizclaw_conversation_t *conversation,
    gzc_event_stream_t **out_events, uint64_t *out_stream_sequence) {
  if (client == NULL || conversation == NULL || out_events == NULL ||
      out_stream_sequence == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_events = NULL;
  *out_stream_sequence = 0u;
  if (client->gzc == NULL || client->events == NULL) {
    return H2_PAL_ERR_CLOSED;
  }
  if (client->active_conversation != NULL ||
      client->next_conversation_stream_sequence == 0u) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  client->active_conversation = conversation;
  *out_events = client->events;
  *out_stream_sequence = client->next_conversation_stream_sequence++;
  return H2_PAL_OK;
}

bool h2_gizclaw_client_conversation_active_internal(
    const h2_gizclaw_client_t *client,
    const h2_gizclaw_conversation_t *conversation) {
  return client != NULL && conversation != NULL && client->events != NULL &&
         client->active_conversation == conversation;
}

void h2_gizclaw_client_conversation_release_internal(
    h2_gizclaw_client_t *client, h2_gizclaw_conversation_t *conversation) {
  if (client != NULL && client->active_conversation == conversation) {
    client->active_conversation = NULL;
  }
}

static void h2_gizclaw_release_event_handle(h2_gizclaw_client_t *client) {
  if (client == NULL) {
    return;
  }
  if (client->active_conversation != NULL) {
    h2_gizclaw_conversation_invalidate_internal(client->active_conversation);
  }
  client->active_conversation = NULL;
  if (client->events != NULL) {
    h2_gizclaw_event_stream_close_internal(client->events);
    client->events = NULL;
  }
}

void h2_gizclaw_client_event_failure_internal(
    h2_gizclaw_client_t *client, h2_gizclaw_conversation_t *conversation) {
  if (client == NULL || client->active_conversation != conversation) {
    return;
  }
  client->terminal_closed = true;
  h2_gizclaw_release_event_handle(client);
}

void h2_gizclaw_client_event_stream_failure_internal(
    h2_gizclaw_client_t *client) {
  if (client == NULL)
    return;
  client->terminal_closed = true;
  h2_gizclaw_release_event_handle(client);
}

int h2_gizclaw_client_dispatch_event(h2_gizclaw_client_t *client,
                                     int timeout_ms,
                                     h2_gizclaw_client_event_fn on_event,
                                     void *event_user) {
  if (client == NULL || timeout_ms < 0)
    return H2_PAL_ERR_INVALID_ARG;
  if (client->terminal_closed || client->events == NULL)
    return H2_PAL_ERR_CLOSED;
  if (client->active_conversation != NULL &&
      h2_gizclaw_conversation_has_pending_peer_event_internal(
          client->active_conversation)) {
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  gzc_peer_event_t peer_event = gizclaw_events_v1_PeerEvent_init_zero;
  const int rc = h2_gizclaw_event_stream_read_internal(client->events,
                                                        timeout_ms, &peer_event);
  if (rc != GZC_OK) {
    if (rc != GZC_ERR_TIMEOUT && rc != GZC_ERR_WOULD_BLOCK)
      h2_gizclaw_client_event_stream_failure_internal(client);
    return h2_gizclaw_result_from_gzc(rc);
  }
  if (peer_event.type ==
      gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_WORKSPACE_HISTORY_UPDATED) {
    const h2_gizclaw_client_event_fn handler =
        on_event != NULL ? on_event : client->event_handler;
    if (handler != NULL) {
      const char *workspace_name =
          peer_event.payload.workspace_history_updated.workspace_name;
      handler(on_event != NULL ? event_user : client->event_handler_user,
              &(h2_gizclaw_client_event_t){
          .kind = H2_GIZCLAW_CLIENT_EVENT_WORKSPACE_HISTORY_UPDATED,
          .workspace_name = {.data = workspace_name,
                             .len = strlen(workspace_name)},
          .last_updated_at_unix_ms =
                      peer_event.payload.workspace_history_updated
                          .last_updated_at_unix_ms,
      });
    }
    return H2_PAL_OK;
  }
  if (client->active_conversation != NULL &&
      h2_gizclaw_conversation_accepts_peer_event_internal(
          client->active_conversation, &peer_event)) {
    h2_gizclaw_conversation_enqueue_peer_event_internal(
        client->active_conversation, &peer_event);
  }
  return H2_PAL_OK;
}

int h2_gizclaw_client_set_event_handler(h2_gizclaw_client_t *client,
                                        h2_gizclaw_client_event_fn on_event,
    void *event_user) {
  if (client == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  client->event_handler = on_event;
  client->event_handler_user = event_user;
  return H2_PAL_OK;
}

static bool h2_gizclaw_is_canceled(const h2_gizclaw_client_t *client) {
  return client != NULL && client->config.cancel_requested != NULL &&
         client->config.cancel_requested(client->config.cancel_user);
}

static int h2_gzc_http_canceled(void *user) {
  return h2_gizclaw_is_canceled((const h2_gizclaw_client_t *)user) ? 1 : 0;
}

static uint64_t h2_gizclaw_bits_per_second(uint64_t bytes,
                                           uint64_t elapsed_ms) {
  if (elapsed_ms == 0u)
    return 0u;
  uint64_t whole = bytes / elapsed_ms;
  if (whole > UINT64_MAX / 8000u) {
    return UINT64_MAX;
  }
  return whole * 8000u + ((bytes % elapsed_ms) * 8000u) / elapsed_ms;
}

static gzc_str_t gzc_str_from_h2(h2_gizclaw_str_t str) {
  return gzc_str_from_parts(str.data, str.len);
}

static h2_pal_result_t h2_gizclaw_result_from_gzc(int result) {
  switch (result) {
  case GZC_OK:
    return H2_PAL_OK;
  case GZC_ERR_INVALID_ARGUMENT:
    return H2_PAL_ERR_INVALID_ARG;
  case GZC_ERR_NO_MEMORY:
    return H2_PAL_ERR_NO_MEMORY;
  case GZC_ERR_TIMEOUT:
    return H2_PAL_ERR_TIMEOUT;
  case GZC_ERR_CLOSED:
    return H2_PAL_ERR_CLOSED;
  case GZC_ERR_UNSUPPORTED:
    return H2_PAL_ERR_UNSUPPORTED;
  case GZC_ERR_WOULD_BLOCK:
    return H2_PAL_ERR_WOULD_BLOCK;
  case GZC_ERR_CHANNEL_LIMIT:
    return H2_PAL_ERR_BUSY;
  default:
    return H2_PAL_ERR_IO;
  }
}

int h2_gizclaw_provider_result_to_gzc(int result) {
  return result == H2_PAL_ERR_NOT_FOUND || result == H2_PAL_ERR_UNSUPPORTED
             ? GZC_ERR_UNSUPPORTED
             : GZC_ERR_RPC;
}

static int h2_gizclaw_rpc_provider_bridge(void *userdata, int method,
                                          gzc_str_t request_payload,
                                          gzc_rpc_provider_respond_fn respond,
                                          void *respond_userdata) {
  h2_gizclaw_client_t *client = userdata;
  if (client == NULL || client->config.rpc_provider == NULL ||
      respond == NULL) {
    return GZC_ERR_UNSUPPORTED;
  }
  h2_gizclaw_rpc_provider_response_t response;
  memset(&response, 0, sizeof(response));
  int rc = client->config.rpc_provider(
      client->config.rpc_provider_user, (h2_gizclaw_rpc_method_t)method,
      (h2_gizclaw_rpc_bytes_t){
          .data = (const uint8_t *)request_payload.data,
          .len = request_payload.len,
      },
      &response);
  if (rc != H2_PAL_OK) {
    return h2_gizclaw_provider_result_to_gzc(rc);
  }
  const gzc_rpc_provider_response_t gzc_response = {
      .payload = response.payload.data,
      .payload_len = response.payload.len,
      .has_error = response.has_error,
      .error_code = response.error_code,
      .error_message =
          gzc_str_from_parts((const char *)response.error_message.data,
                             response.error_message.len),
  };
  return respond(respond_userdata, &gzc_response);
}

static bool h2_gizclaw_gzc_str_has_prefix_cstr(gzc_str_t value,
                                               const char *prefix) {
  size_t prefix_len = strlen(prefix);
  return value.data != NULL && value.len >= prefix_len &&
         memcmp(value.data, prefix, prefix_len) == 0;
}

static int h2_gizclaw_remote_service_index(h2_gizclaw_client_t *client,
                                           gzc_rtc_channel_t *channel) {
  if (client == NULL || channel == NULL) {
    return -1;
  }
  for (size_t i = 0u; i < GZC_RPC_MAX_INBOUND_CHANNELS; ++i) {
    if (client->remote_service_channels[i] == channel) {
      return (int)i;
    }
  }
  return -1;
}

static int h2_gizclaw_local_channel_index(h2_gizclaw_client_t *client,
                                          gzc_rtc_channel_t *channel) {
  if (client == NULL || channel == NULL) {
    return -1;
  }
  for (size_t i = 0u; i < GZC_RPC_MAX_INBOUND_CHANNELS + 2u; ++i) {
    if (client->local_channels[i] == channel) {
      return (int)i;
    }
  }
  return -1;
}

static void h2_gizclaw_mark_local_channel(h2_gizclaw_client_t *client,
                                          gzc_rtc_channel_t *channel) {
  if (client == NULL || channel == NULL ||
      h2_gizclaw_local_channel_index(client, channel) >= 0) {
    return;
  }
  for (size_t i = 0u; i < GZC_RPC_MAX_INBOUND_CHANNELS + 2u; ++i) {
    if (client->local_channels[i] == NULL) {
      client->local_channels[i] = channel;
      return;
    }
  }
}

static void h2_gizclaw_unmark_local_channel(h2_gizclaw_client_t *client,
                                            gzc_rtc_channel_t *channel) {
  int index = h2_gizclaw_local_channel_index(client, channel);
  if (index >= 0) {
    client->local_channels[index] = NULL;
  }
}

static bool h2_gizclaw_mark_remote_service(h2_gizclaw_client_t *client,
                                           gzc_rtc_channel_t *channel) {
  if (client == NULL || channel == NULL) {
    return false;
  }
  if (h2_gizclaw_remote_service_index(client, channel) >= 0) {
    return false;
  }
  for (size_t i = 0u; i < GZC_RPC_MAX_INBOUND_CHANNELS; ++i) {
    if (client->remote_service_channels[i] == NULL) {
      client->remote_service_channels[i] = channel;
      return true;
    }
  }
  return false;
}

static void h2_gizclaw_unmark_remote_service(h2_gizclaw_client_t *client,
                                             gzc_rtc_channel_t *channel) {
  int index = h2_gizclaw_remote_service_index(client, channel);
  if (index >= 0) {
    client->remote_service_channels[index] = NULL;
  }
}

static bool h2_gizclaw_channel_is_live(h2_gizclaw_client_t *client,
                                       gzc_rtc_channel_t *channel) {
  return client != NULL && channel != NULL &&
         (h2_gizclaw_local_channel_index(client, channel) >= 0 ||
          h2_gizclaw_remote_service_index(client, channel) >= 0);
}

static h2_gizclaw_client_t *
h2_gizclaw_client_for_channel(gzc_rtc_channel_t *channel) {
  for (h2_gizclaw_client_t *client = s_clients; client != NULL;
       client = client->next_client) {
    if (h2_gizclaw_channel_is_live(client, channel)) {
      return client;
    }
  }
#if defined(H2_GIZCLAW_TESTING)
  return s_test_webrtc_client;
#else
  return NULL;
#endif
}

static void h2_gizclaw_reset_local_channel_state(h2_gizclaw_client_t *client) {
  if (client != NULL) {
    memset(&client->local_channel_state, 0,
           sizeof(client->local_channel_state));
  }
}

static bool h2_gizclaw_local_channel_create_matches(
    const h2_gizclaw_local_channel_state_t *pending,
    const h2_pal_webrtc_channel_info_t *info) {
  return pending != NULL && pending->create_in_progress && info != NULL &&
         pending->create_label.data != NULL && info->label.data != NULL &&
         info->label.len == pending->create_label.len &&
         memcmp(info->label.data, pending->create_label.data,
                info->label.len) == 0 &&
         (info->ordered != 0) == pending->create_ordered &&
         (info->reliable != 0) == pending->create_reliable;
}

static bool h2_gizclaw_retain_local_channel_state(
    h2_gizclaw_local_channel_state_t *pending, h2_pal_webrtc_peer_t *peer,
    h2_pal_webrtc_channel_t *channel, const h2_pal_webrtc_channel_info_t *info,
    h2_pal_webrtc_channel_state_t state) {
  if (pending == NULL || peer == NULL || channel == NULL ||
      (pending->ready && pending->channel != channel) ||
      (info != NULL && (info->label.data == NULL ||
                        info->label.len >= sizeof(pending->label)))) {
    return false;
  }
  if (pending->ready &&
      (pending->state == H2_PAL_WEBRTC_CHANNEL_CLOSED ||
       pending->state == H2_PAL_WEBRTC_CHANNEL_ERROR) &&
      state == H2_PAL_WEBRTC_CHANNEL_OPEN) {
    return true;
  }
  pending->ready = true;
  pending->peer = peer;
  pending->channel = channel;
  pending->state = state;
  pending->has_info = info != NULL;
  memset(&pending->info, 0, sizeof(pending->info));
  if (info != NULL) {
    memcpy(pending->label, info->label.data, info->label.len);
    pending->label[info->label.len] = '\0';
    pending->info = *info;
    pending->info.label.data = pending->label;
  }
  return true;
}

int h2_gizclaw_encode_pb_message(h2_gizclaw_client_t *client,
                                 const pb_msgdesc_t *fields,
                                 const void *message,
                                 gzc_buf_t *out_payload) {
  if (client == NULL || fields == NULL || message == NULL ||
      out_payload == NULL) {
    return GZC_ERR_INVALID_ARGUMENT;
  }
  pb_ostream_t sizing = PB_OSTREAM_SIZING;
  if (!pb_encode(&sizing, fields, message)) {
    return GZC_ERR_RPC;
  }
  size_t size = sizing.bytes_written;
  uint8_t *buf = (uint8_t *)h2_pal_mem_alloc(client->config.allocator,
                                             size == 0u ? 1u : size);
  if (buf == NULL) {
    return GZC_ERR_NO_MEMORY;
  }
  pb_ostream_t stream = pb_ostream_from_buffer(buf, size);
  int rc = GZC_OK;
  if (!pb_encode(&stream, fields, message)) {
    rc = GZC_ERR_RPC;
  } else {
    rc = gzc_buf_append(out_payload, &client->platform, buf, size);
  }
  h2_pal_mem_free(client->config.allocator, buf);
  return rc;
}

int h2_gizclaw_decode_pb_message(gzc_str_t payload,
                                 const pb_msgdesc_t *fields, void *message) {
  if (fields == NULL || message == NULL ||
      (payload.data == NULL && payload.len != 0u)) {
    return GZC_ERR_INVALID_ARGUMENT;
  }
  pb_istream_t stream =
      pb_istream_from_buffer((const pb_byte_t *)payload.data, payload.len);
  return pb_decode(&stream, fields, message) ? GZC_OK : GZC_ERR_RPC;
}

static void h2_gizclaw_log_error(h2_gizclaw_client_t *client, const char *stage,
                                 int rc) {
  if (client == NULL || client->config.log == NULL) {
    return;
  }
  char message[96];
  (void)snprintf(message, sizeof(message), "stage=%s gzc_rc=%d", stage, rc);
  (void)h2_pal_log_write(client->config.log, H2_PAL_LOG_ERROR, "gizclaw",
                         message);
}

static void h2_gizclaw_log_webrtc_rc(h2_gizclaw_client_t *client,
                                     const char *stage, int rc) {
  if (client == NULL || client->config.log == NULL) {
    return;
  }
  char message[96];
  (void)snprintf(message, sizeof(message), "webrtc_stage=%s rc=%d", stage, rc);
  (void)h2_pal_log_write(client->config.log,
                         rc == H2_PAL_OK ? H2_PAL_LOG_INFO : H2_PAL_LOG_ERROR,
                         "gizclaw", message);
}

static void h2_gizclaw_log_webrtc_poll_backoff(
    h2_gizclaw_client_t *client, uint32_t backoff_ms) {
  if (client == NULL || client->config.log == NULL) {
    return;
  }
  char message[96];
  (void)snprintf(message, sizeof(message),
                 "webrtc_stage=peer_poll rc=%d backoff_ms=%u",
                 H2_PAL_ERR_WOULD_BLOCK, (unsigned int)backoff_ms);
  (void)h2_pal_log_write(client->config.log, H2_PAL_LOG_WARN, "gizclaw",
                         message);
}

static void h2_gizclaw_log_infof(h2_gizclaw_client_t *client, const char *fmt,
                                 int a, int b, size_t c) {
  if (client == NULL || client->config.log == NULL) {
    return;
  }
  char message[128];
  (void)snprintf(message, sizeof(message), fmt, a, b, c);
  (void)h2_pal_log_write(client->config.log, H2_PAL_LOG_INFO, "gizclaw",
                         message);
}

static void h2_gizclaw_log_http_status(h2_gizclaw_client_t *client,
                                       int status_code, const uint8_t *body,
                                       size_t body_len) {
  if (client == NULL || client->config.log == NULL) {
    return;
  }
  char message[192];
  char body_text[80];
  size_t copy_len =
      body_len < (sizeof(body_text) - 1u) ? body_len : (sizeof(body_text) - 1u);
  if (body != NULL && copy_len > 0u) {
    memcpy(body_text, body, copy_len);
  } else {
    copy_len = 0u;
  }
  body_text[copy_len] = '\0';
  (void)snprintf(message, sizeof(message),
                 "http_status=%d body_len=%zu body=%s", status_code, body_len,
                 body_text);
  (void)h2_pal_log_write(client->config.log, H2_PAL_LOG_ERROR, "gizclaw",
                         message);
}

static void h2_gizclaw_log_rpc_error(h2_gizclaw_client_t *client,
                                     h2_gizclaw_rpc_method_t method,
                                     int error_code, const char *error_message,
                                     size_t error_message_len) {
  if (client == NULL || client->config.log == NULL) {
    return;
  }
  char message[256];
  const int text_len = error_message_len > 160u ? 160 : (int)error_message_len;
  (void)snprintf(message, sizeof(message),
                 "rpc_error method=%d code=%d message=%.*s", (int)method,
                 error_code, text_len,
                 error_message != NULL ? error_message : "");
  (void)h2_pal_log_write(client->config.log, H2_PAL_LOG_ERROR, "gizclaw",
                         message);
}

void h2_gizclaw_client_log_rpc_error_internal(h2_gizclaw_client_t *client,
                                              h2_gizclaw_rpc_method_t method,
                                              int error_code,
                                              const char *error_message,
                                              size_t error_message_len) {
  h2_gizclaw_log_rpc_error(client, method, error_code, error_message,
                           error_message_len);
}

static void *h2_gzc_malloc(void *user, size_t size) {
  h2_gizclaw_client_t *client = (h2_gizclaw_client_t *)user;
  return h2_pal_mem_alloc(client->config.allocator, size);
}

static void *h2_gzc_realloc(void *user, void *ptr, size_t size) {
  h2_gizclaw_client_t *client = (h2_gizclaw_client_t *)user;
  return h2_pal_mem_realloc(client->config.allocator, ptr, size);
}

static void h2_gzc_free(void *user, void *ptr) {
  h2_gizclaw_client_t *client = (h2_gizclaw_client_t *)user;
  h2_pal_mem_free(client->config.allocator, ptr);
}

static int64_t h2_gzc_time_instant_ms(void *user) {
  h2_gizclaw_client_t *client = (h2_gizclaw_client_t *)user;
  const h2_pal_time_api_t *api = client->config.time;
  uint64_t monotonic_ms = 0;
  if (api == NULL ||
      h2_pal_time_get_monotonic_ms(api, &monotonic_ms) != H2_PAL_OK) {
    return 0;
  }
  return (int64_t)monotonic_ms;
}

static int64_t h2_gzc_time_unix_ms(void *user) {
  h2_gizclaw_client_t *client = (h2_gizclaw_client_t *)user;
  const h2_pal_time_api_t *api = client->config.time;
  uint64_t wall_ms = 0;
  if (api == NULL || h2_pal_time_get_wall_ms(api, &wall_ms) != H2_PAL_OK) {
    return 0;
  }
  return (int64_t)wall_ms;
}

static int h2_gzc_random(void *user, uint8_t *out, size_t len) {
  h2_gizclaw_client_t *client = (h2_gizclaw_client_t *)user;
  if (client->config.crypto == NULL) {
    return GZC_ERR_UNSUPPORTED;
  }
  int rc = h2_pal_crypto_random(client->config.crypto, out, len);
  if (rc != H2_PAL_OK) {
    h2_gizclaw_log_error(client, "random", rc);
  }
  return rc == H2_PAL_OK ? GZC_OK
                         : (rc == H2_PAL_ERR_UNSUPPORTED ? GZC_ERR_UNSUPPORTED
                                                         : GZC_ERR_SIGNALING);
}

static void h2_gzc_log(void *user, gzc_log_level_t level, gzc_str_t message) {
  h2_gizclaw_client_t *client = (h2_gizclaw_client_t *)user;
  if (client->config.log == NULL || message.data == NULL) {
    return;
  }
  h2_pal_log_level_t h2_level = H2_PAL_LOG_INFO;
  if (level == GZC_LOG_DEBUG) {
    h2_level = H2_PAL_LOG_DEBUG;
  } else if (level == GZC_LOG_WARN) {
    h2_level = H2_PAL_LOG_WARN;
  } else if (level == GZC_LOG_ERROR) {
    h2_level = H2_PAL_LOG_ERROR;
  }
  char stack_buf[160];
  const char *text = message.data;
  if (message.len >= sizeof(stack_buf)) {
    memcpy(stack_buf, message.data, sizeof(stack_buf) - 1u);
    stack_buf[sizeof(stack_buf) - 1u] = '\0';
    text = stack_buf;
  }
  (void)h2_pal_log_write(client->config.log, h2_level, "gizclaw", text);
}

static int h2_gzc_keypair_from_private(void *user, const gzc_key_t *private_key,
                                       gzc_keypair_t *out_keypair) {
  h2_gizclaw_client_t *client = (h2_gizclaw_client_t *)user;
  if (client->config.crypto == NULL || private_key == NULL ||
      out_keypair == NULL) {
    return GZC_ERR_INVALID_ARGUMENT;
  }
  h2_pal_x25519_private_key_t h2_private;
  h2_pal_x25519_public_key_t h2_public;
  memcpy(h2_private.bytes, private_key->bytes, sizeof(h2_private.bytes));
  int rc = h2_pal_crypto_x25519_public_key_from_private(
      client->config.crypto, &h2_private, &h2_public);
  if (rc != H2_PAL_OK) {
    h2_gizclaw_log_error(client, "keypair_from_private", rc);
    return rc == H2_PAL_ERR_UNSUPPORTED ? GZC_ERR_UNSUPPORTED
                                        : GZC_ERR_SIGNALING;
  }
  memcpy(out_keypair->private_key.bytes, private_key->bytes,
         sizeof(out_keypair->private_key.bytes));
  memcpy(out_keypair->public_key.bytes, h2_public.bytes,
         sizeof(out_keypair->public_key.bytes));
  return GZC_OK;
}

static int h2_gzc_dh(void *user, const gzc_keypair_t *local,
                     const gzc_public_key_t *remote, gzc_key_t *out_shared) {
  h2_gizclaw_client_t *client = (h2_gizclaw_client_t *)user;
  if (client->config.crypto == NULL || local == NULL || remote == NULL ||
      out_shared == NULL) {
    return GZC_ERR_INVALID_ARGUMENT;
  }
  h2_pal_x25519_private_key_t h2_private;
  h2_pal_x25519_public_key_t h2_remote;
  h2_pal_x25519_shared_secret_t h2_shared;
  memcpy(h2_private.bytes, local->private_key.bytes, sizeof(h2_private.bytes));
  memcpy(h2_remote.bytes, remote->bytes, sizeof(h2_remote.bytes));
  int rc = h2_pal_crypto_x25519_shared_secret(
      client->config.crypto, &h2_private, &h2_remote, &h2_shared);
  if (rc != H2_PAL_OK) {
    h2_gizclaw_log_error(client, "dh", rc);
    return rc == H2_PAL_ERR_UNSUPPORTED ? GZC_ERR_UNSUPPORTED
                                        : GZC_ERR_SIGNALING;
  }
  memcpy(out_shared->bytes, h2_shared.bytes, sizeof(out_shared->bytes));
  return GZC_OK;
}

static int h2_gzc_hkdf_sha256(void *user, const uint8_t *secret,
                              size_t secret_len, const uint8_t *salt,
                              size_t salt_len, gzc_str_t info, uint8_t *out,
                              size_t out_len) {
  h2_gizclaw_client_t *client = (h2_gizclaw_client_t *)user;
  if (client->config.crypto == NULL) {
    return GZC_ERR_UNSUPPORTED;
  }
  int rc = h2_pal_crypto_hkdf_sha256(client->config.crypto, secret, secret_len,
                                     salt, salt_len, (const uint8_t *)info.data,
                                     info.len, out, out_len);
  if (rc != H2_PAL_OK) {
    h2_gizclaw_log_error(client, "hkdf_sha256", rc);
  }
  return rc == H2_PAL_OK ? GZC_OK
                         : (rc == H2_PAL_ERR_UNSUPPORTED ? GZC_ERR_UNSUPPORTED
                                                         : GZC_ERR_SIGNALING);
}

static int h2_gzc_aead(h2_gizclaw_client_t *client, bool seal,
                       gzc_cipher_mode_t mode, const uint8_t *key,
                       size_t key_len, const uint8_t *nonce, size_t nonce_len,
                       const uint8_t *input, size_t input_len,
                       const uint8_t *aad, size_t aad_len, gzc_buf_t *out) {
  if (client == NULL || client->config.crypto == NULL || out == NULL) {
    return GZC_ERR_INVALID_ARGUMENT;
  }
  if (mode != GZC_CIPHER_PLAINTEXT && mode != GZC_CIPHER_CHACHA20_POLY1305 &&
      mode != GZC_CIPHER_AES_256_GCM) {
    return GZC_ERR_UNSUPPORTED;
  }
  size_t needed = input_len;
  if (seal && mode != GZC_CIPHER_PLAINTEXT) {
    if (input_len > ((size_t)-1) - 16u) {
      return GZC_ERR_NO_MEMORY;
    }
    needed = input_len + 16u;
  } else if (!seal && mode != GZC_CIPHER_PLAINTEXT) {
    if (input_len < 16u) {
      return GZC_ERR_INVALID_ARGUMENT;
    }
    needed = input_len - 16u;
  }
  int reserve_rc = gzc_buf_reserve(out, &client->platform, needed);
  if (reserve_rc != GZC_OK) {
    return reserve_rc;
  }
  if (mode == GZC_CIPHER_PLAINTEXT) {
    if (needed != 0u && (input == NULL || out->data == NULL)) {
      return GZC_ERR_INVALID_ARGUMENT;
    }
    if (needed != 0u) {
      memmove(out->data, input, needed);
    }
    out->len = needed;
    return GZC_OK;
  }
  h2_pal_crypto_aead_algorithm_t algorithm =
      mode == GZC_CIPHER_CHACHA20_POLY1305
          ? H2_PAL_CRYPTO_AEAD_CHACHA20_POLY1305
          : H2_PAL_CRYPTO_AEAD_AES_256_GCM;
  h2_pal_crypto_buf_t h2_out = {
      .data = out->data,
      .len = out->len,
      .cap = out->cap,
  };
  int rc;
  if (seal) {
    rc = h2_pal_crypto_aead_seal(client->config.crypto, algorithm, key, key_len,
                                 nonce, nonce_len, input, input_len, aad,
                                 aad_len, &h2_out);
  } else {
    rc = h2_pal_crypto_aead_open(client->config.crypto, algorithm, key, key_len,
                                 nonce, nonce_len, input, input_len, aad,
                                 aad_len, &h2_out);
  }
  if (rc != H2_PAL_OK) {
    h2_gizclaw_log_error(client, seal ? "aead_seal" : "aead_open", rc);
    return rc == H2_PAL_ERR_UNSUPPORTED ? GZC_ERR_UNSUPPORTED
                                        : GZC_ERR_SIGNALING;
  }
  out->data = h2_out.data;
  out->len = h2_out.len;
  out->cap = h2_out.cap;
  return GZC_OK;
}

static int h2_gzc_aead_seal(void *user, gzc_cipher_mode_t mode,
                            const uint8_t *key, size_t key_len,
                            const uint8_t *nonce, size_t nonce_len,
                            const uint8_t *plaintext, size_t plaintext_len,
                            const uint8_t *aad, size_t aad_len,
                            gzc_buf_t *out_ciphertext) {
  return h2_gzc_aead((h2_gizclaw_client_t *)user, true, mode, key, key_len,
                     nonce, nonce_len, plaintext, plaintext_len, aad, aad_len,
                     out_ciphertext);
}

static int h2_gzc_aead_open(void *user, gzc_cipher_mode_t mode,
                            const uint8_t *key, size_t key_len,
                            const uint8_t *nonce, size_t nonce_len,
                            const uint8_t *ciphertext, size_t ciphertext_len,
                            const uint8_t *aad, size_t aad_len,
                            gzc_buf_t *out_plaintext) {
  return h2_gzc_aead((h2_gizclaw_client_t *)user, false, mode, key, key_len,
                     nonce, nonce_len, ciphertext, ciphertext_len, aad, aad_len,
                     out_plaintext);
}

static int h2_gzc_http_request(void *user, const gzc_http_request_t *request,
                               gzc_http_response_t *out_response) {
  h2_gizclaw_client_t *client = (h2_gizclaw_client_t *)user;
  if (client == NULL || client->config.http == NULL || request == NULL ||
      out_response == NULL) {
    return GZC_ERR_INVALID_ARGUMENT;
  }
  h2_pal_http_header_t stack_headers[8];
  h2_pal_http_header_t *headers = stack_headers;
  if (request->header_count >
      (sizeof(stack_headers) / sizeof(stack_headers[0]))) {
    headers = (h2_pal_http_header_t *)h2_pal_mem_alloc(
        client->config.allocator, request->header_count * sizeof(headers[0]));
    if (headers == NULL) {
      return GZC_ERR_NO_MEMORY;
    }
  }
  for (size_t i = 0u; i < request->header_count; ++i) {
    headers[i].name.data = request->headers[i].name.data;
    headers[i].name.len = request->headers[i].name.len;
    headers[i].value.data = request->headers[i].value.data;
    headers[i].value.len = request->headers[i].value.len;
  }

  h2_pal_http_request_t h2_request = {
      .method = (h2_pal_http_method_t)request->method,
      .url = {.data = request->url.data, .len = request->url.len},
      .headers = headers,
      .header_count = request->header_count,
      .body = request->body,
      .body_len = request->body_len,
      /* Cancellation is orthogonal to the caller's service deadline. */
      .timeout_ms = request->timeout_ms,
      .retry_count = request->retry_count,
      .response_allocator = client->config.allocator,
      .allocator = client->config.allocator,
      .cancel_cb = h2_gzc_http_canceled,
      .cancel_user = client,
  };
  h2_pal_http_response_t h2_response;
  h2_pal_http_response_reset(&h2_response);
  int rc = h2_pal_http_request(client->config.http, &h2_request, &h2_response);
  if (headers != stack_headers) {
    h2_pal_mem_free(client->config.allocator, headers);
  }
  h2_gizclaw_log_infof(client, "http_result rc=%d status=%d body_len=%zu", rc,
                       h2_response.status_code, h2_response.body_len);
  if (rc != H2_PAL_OK) {
    h2_pal_http_response_free(client->config.http, &h2_response);
    return GZC_ERR_HTTP;
  }
  if (h2_response.status_code < 200 || h2_response.status_code >= 300) {
    h2_gizclaw_log_http_status(client, h2_response.status_code,
                               h2_response.body, h2_response.body_len);
  }
  out_response->status_code = h2_response.status_code;
  out_response->content_length = h2_response.content_length;
  out_response->body.data = h2_response.body;
  out_response->body.len = h2_response.body_len;
  out_response->body.cap = h2_response.body_len;
  return GZC_OK;
}

static void h2_gzc_http_response_free(void *user,
                                      gzc_http_response_t *response) {
  h2_gizclaw_client_t *client = (h2_gizclaw_client_t *)user;
  if (client == NULL || response == NULL) {
    return;
  }
  h2_pal_http_response_t h2_response = {
      .status_code = response->status_code,
      .content_length = response->content_length,
      .body = response->body.data,
      .body_len = response->body.len,
      .allocator = client->config.allocator,
  };
  h2_pal_http_response_free(client->config.http, &h2_response);
  response->body.data = NULL;
  response->body.len = 0;
  response->body.cap = 0;
}

static void h2_gzc_peer_state(void *user, h2_pal_webrtc_peer_t *peer,
                              h2_pal_webrtc_peer_state_t state) {
  h2_gizclaw_client_t *client = (h2_gizclaw_client_t *)user;
  h2_gizclaw_log_infof(client, "webrtc_peer_state=%d", (int)state, 0, 0u);
  if (client != NULL && client->gzc_callbacks.on_peer_state != NULL) {
    client->gzc_callbacks.on_peer_state(client->gzc_callbacks.userdata,
                                        (gzc_rtc_peer_t *)peer,
                                        (gzc_rtc_peer_state_t)state);
  }
}

static void h2_gzc_local_sdp(void *user, h2_pal_webrtc_peer_t *peer,
                             h2_pal_webrtc_sdp_type_t type,
                             h2_pal_webrtc_str_t sdp) {
  h2_gizclaw_client_t *client = (h2_gizclaw_client_t *)user;
  if (client != NULL && client->gzc_callbacks.on_local_sdp != NULL) {
    client->gzc_callbacks.on_local_sdp(
        client->gzc_callbacks.userdata, (gzc_rtc_peer_t *)peer,
        (gzc_rtc_sdp_type_t)type, gzc_str_from_parts(sdp.data, sdp.len));
  }
}

static void h2_gizclaw_dispatch_channel_state(
    h2_gizclaw_client_t *client, h2_pal_webrtc_peer_t *peer,
    h2_pal_webrtc_channel_t *channel, const h2_pal_webrtc_channel_info_t *info,
    h2_pal_webrtc_channel_state_t state) {
  if (client != NULL && client->gzc_callbacks.on_channel_state != NULL) {
    gzc_rtc_channel_info_t gzc_info;
    memset(&gzc_info, 0, sizeof(gzc_info));
    if (info != NULL) {
      gzc_info.label = gzc_str_from_parts(info->label.data, info->label.len);
      gzc_info.stream_id = info->stream_id;
      gzc_info.ordered = info->ordered != 0;
      gzc_info.reliable = info->reliable != 0;
    }
    if (state == H2_PAL_WEBRTC_CHANNEL_OPEN && info != NULL &&
        h2_gizclaw_gzc_str_has_prefix_cstr(gzc_info.label,
                                           "giznet/v1/service/") &&
        gzc_info.ordered && gzc_info.reliable &&
        h2_gizclaw_local_channel_index(client, (gzc_rtc_channel_t *)channel) <
            0 &&
        h2_gizclaw_mark_remote_service(client, (gzc_rtc_channel_t *)channel) &&
        client->gzc_callbacks.on_remote_channel != NULL) {
      client->gzc_callbacks.on_remote_channel(
          client->gzc_callbacks.userdata, (gzc_rtc_peer_t *)peer,
          (gzc_rtc_channel_t *)channel, &gzc_info);
    } else if (state == H2_PAL_WEBRTC_CHANNEL_CLOSED ||
               state == H2_PAL_WEBRTC_CHANNEL_ERROR) {
      h2_gizclaw_unmark_remote_service(client, (gzc_rtc_channel_t *)channel);
      h2_gizclaw_unmark_local_channel(client, (gzc_rtc_channel_t *)channel);
    }
    client->gzc_callbacks.on_channel_state(
        client->gzc_callbacks.userdata, (gzc_rtc_peer_t *)peer,
        (gzc_rtc_channel_t *)channel, info == NULL ? NULL : &gzc_info,
        (gzc_rtc_channel_state_t)state);
  }
}

static void h2_gzc_channel_state(void *user, h2_pal_webrtc_peer_t *peer,
                                 h2_pal_webrtc_channel_t *channel,
                                 const h2_pal_webrtc_channel_info_t *info,
                                 h2_pal_webrtc_channel_state_t state) {
  h2_gizclaw_client_t *client = (h2_gizclaw_client_t *)user;
  if (client == NULL) {
    return;
  }
  if (state == H2_PAL_WEBRTC_CHANNEL_CLOSED ||
      state == H2_PAL_WEBRTC_CHANNEL_ERROR) {
    /* PAL terminal callbacks revoke the opaque channel alias immediately.
     * GZC can issue a final close/send while dispatching a retained terminal
     * state; those adapter calls must not re-enter PAL with the stale alias. */
    h2_gizclaw_unmark_remote_service(client, (gzc_rtc_channel_t *)channel);
    h2_gizclaw_unmark_local_channel(client, (gzc_rtc_channel_t *)channel);
  }
  h2_gizclaw_local_channel_state_t *pending = &client->local_channel_state;
  if ((h2_gizclaw_local_channel_create_matches(pending, info) ||
       (pending->ready && pending->channel == channel)) &&
      h2_gizclaw_retain_local_channel_state(pending, peer, channel, info,
                                            state)) {
    return;
  }
  h2_gizclaw_dispatch_channel_state(client, peer, channel, info, state);
}

static void
h2_gizclaw_dispatch_retained_local_channel_state(h2_gizclaw_client_t *client,
                                                 h2_pal_webrtc_peer_t *peer) {
  if (client == NULL || !client->local_channel_state.ready ||
      client->local_channel_state.create_in_progress ||
      client->local_channel_state.peer != peer) {
    return;
  }
  h2_gizclaw_local_channel_state_t pending = client->local_channel_state;
  h2_gizclaw_reset_local_channel_state(client);
  if (pending.has_info) {
    pending.info.label.data = pending.label;
  }
  h2_gizclaw_dispatch_channel_state(client, pending.peer, pending.channel,
                                    pending.has_info ? &pending.info : NULL,
                                    pending.state);
}

static void h2_gzc_channel_message(void *user, h2_pal_webrtc_peer_t *peer,
                                   h2_pal_webrtc_channel_t *channel,
                                   const h2_pal_webrtc_channel_info_t *info,
                                   const uint8_t *data, size_t len,
                                   int is_text) {
  (void)info;
  h2_gizclaw_client_t *client = (h2_gizclaw_client_t *)user;
  if (client != NULL && client->gzc_callbacks.on_channel_message != NULL) {
    client->gzc_callbacks.on_channel_message(
        client->gzc_callbacks.userdata, (gzc_rtc_peer_t *)peer,
        (gzc_rtc_channel_t *)channel, NULL, data, len, is_text != 0);
  }
}

static void h2_gzc_opus_frame(void *user, h2_pal_webrtc_peer_t *peer,
                              const uint8_t *opus, size_t opus_len) {
  h2_gizclaw_client_t *client = (h2_gizclaw_client_t *)user;
  if (client == NULL || client->opus_frame_callback == NULL ||
      client->opus_frame_peer != (gzc_rtc_peer_t *)peer) {
    return;
  }
  client->opus_frame_callback(client->opus_frame_callback_user,
                              (gzc_rtc_peer_t *)peer, opus, opus_len);
}

static int h2_gzc_peer_create(void *user,
                              const gzc_webrtc_callbacks_t *callbacks,
                              gzc_rtc_peer_t **out_peer) {
  h2_gizclaw_client_t *client = (h2_gizclaw_client_t *)user;
  if (client == NULL || client->config.webrtc == NULL || callbacks == NULL ||
      out_peer == NULL) {
    return GZC_ERR_INVALID_ARGUMENT;
  }
  client->gzc_callbacks = *callbacks;
  h2_pal_webrtc_callbacks_t h2_callbacks = {
      .user = client,
      .on_peer_state = h2_gzc_peer_state,
      .on_local_sdp = h2_gzc_local_sdp,
      .on_channel_state = h2_gzc_channel_state,
      .on_channel_message = h2_gzc_channel_message,
      .on_opus_frame =
          client->config.webrtc_media_track == NULL ? h2_gzc_opus_frame : NULL,
  };
  h2_pal_webrtc_peer_t *peer = NULL;
  int rc =
      h2_pal_webrtc_peer_create(client->config.webrtc, &h2_callbacks, &peer);
  h2_gizclaw_log_webrtc_rc(client, "peer_create", rc);
  if (rc != H2_PAL_OK) {
    return GZC_ERR_WEBRTC;
  }
  if (client->config.webrtc_media_track != NULL) {
    rc = h2_pal_webrtc_peer_set_media_track(client->config.webrtc, peer,
                                            client->config.webrtc_media_track);
    h2_gizclaw_log_webrtc_rc(client, "peer_set_media_track", rc);
    if (rc != H2_PAL_OK) {
      h2_pal_webrtc_peer_close(client->config.webrtc, peer);
      return GZC_ERR_WEBRTC;
    }
  }
  client->webrtc_peer = peer;
  *out_peer = (gzc_rtc_peer_t *)peer;
  return GZC_OK;
}

#if defined(H2_GIZCLAW_TESTING)
static h2_gizclaw_test_rpc_call_fn s_test_rpc_call;
static void *s_test_rpc_call_user;
static h2_gizclaw_test_rpc_call_stream_fn s_test_rpc_call_stream;
static void *s_test_rpc_call_stream_user;
static h2_gizclaw_test_speed_test_fn s_test_speed_test;
static void *s_test_speed_test_user;

void h2_gizclaw_test_set_rpc_call(h2_gizclaw_test_rpc_call_fn call,
                                  void *user) {
  s_test_rpc_call = call;
  s_test_rpc_call_user = user;
}

void h2_gizclaw_test_set_rpc_call_stream(
    h2_gizclaw_test_rpc_call_stream_fn call, void *user) {
  s_test_rpc_call_stream = call;
  s_test_rpc_call_stream_user = user;
}

void h2_gizclaw_test_set_speed_test(h2_gizclaw_test_speed_test_fn speed_test,
                                    void *user) {
  s_test_speed_test = speed_test;
  s_test_speed_test_user = user;
}

void h2_gizclaw_test_set_event_ops(h2_gizclaw_test_event_send_fn send,
                                   h2_gizclaw_test_event_read_fn read,
                                   h2_gizclaw_test_event_close_fn close,
                                   void *user) {
  s_test_event_send = send;
  s_test_event_read = read;
  s_test_event_close = close;
  s_test_event_user = user;
}

void h2_gizclaw_test_set_packet_read(h2_gizclaw_test_packet_read_fn read,
                                     void *user) {
  s_test_packet_read = read;
  s_test_packet_read_user = user;
}

void h2_gizclaw_test_set_client_poll(h2_gizclaw_test_client_poll_fn poll,
                                     void *user) {
  s_test_client_poll = poll;
  s_test_client_poll_user = user;
}

gzc_event_stream_t *
h2_gizclaw_test_replace_event_stream(h2_gizclaw_client_t *client,
                                     gzc_event_stream_t *events) {
  if (client == NULL) {
    return NULL;
  }
  gzc_event_stream_t *previous = client->events;
  client->events = events;
  return previous;
}

bool h2_gizclaw_test_client_terminal_closed(const h2_gizclaw_client_t *client) {
  return client != NULL && client->terminal_closed;
}

int gzc_client_try_write_bytes_internal(gzc_client_t *client,
                                        gzc_rtc_channel_t *channel,
                                        const uint8_t *data, size_t len,
                                        size_t *offset, bool *blocked,
                                        size_t max_chunks);
#endif

static int h2_gzc_peer_start_offer_active(gzc_rtc_peer_t *peer) {
  h2_gizclaw_client_t *client = h2_gizclaw_client_for_peer(peer);
  if (client == NULL || client->config.webrtc == NULL) {
    return GZC_ERR_INVALID_ARGUMENT;
  }
  int rc = h2_pal_webrtc_peer_start_offer(client->config.webrtc,
                                          (h2_pal_webrtc_peer_t *)peer);
  h2_gizclaw_log_webrtc_rc(client, "peer_start_offer", rc);
  return rc == H2_PAL_OK ? GZC_OK : GZC_ERR_WEBRTC;
}

static int h2_gzc_media_result(int rc) {
  switch (rc) {
  case H2_PAL_OK:
    return GZC_OK;
  case H2_PAL_ERR_INVALID_ARG:
    return GZC_ERR_INVALID_ARGUMENT;
  case H2_PAL_ERR_UNSUPPORTED:
    return GZC_ERR_UNSUPPORTED;
  case H2_PAL_ERR_CLOSED:
    return GZC_ERR_CLOSED;
  case H2_PAL_ERR_TIMEOUT:
    return GZC_ERR_TIMEOUT;
  case H2_PAL_ERR_WOULD_BLOCK:
    return GZC_ERR_WOULD_BLOCK;
  default:
    return GZC_ERR_WEBRTC;
  }
}

static int h2_gzc_peer_set_opus_frame_callback(gzc_rtc_peer_t *peer,
                                               gzc_rtc_opus_frame_cb callback,
                                               void *callback_user) {
  h2_gizclaw_client_t *client = h2_gizclaw_client_for_peer(peer);
  if (client == NULL || client->config.webrtc == NULL || peer == NULL ||
      (callback == NULL && callback_user != NULL)) {
    return GZC_ERR_INVALID_ARGUMENT;
  }
  client->opus_frame_peer = callback == NULL ? NULL : peer;
  client->opus_frame_callback = callback;
  client->opus_frame_callback_user = callback == NULL ? NULL : callback_user;
  return GZC_OK;
}

static int h2_gzc_peer_send_opus(gzc_rtc_peer_t *peer, const uint8_t *opus,
                                 size_t opus_len) {
  h2_gizclaw_client_t *client = h2_gizclaw_client_for_peer(peer);
  if (client == NULL || client->config.webrtc == NULL) {
    return GZC_ERR_INVALID_ARGUMENT;
  }
  if (client->config.webrtc_media_track != NULL)
    return GZC_ERR_UNSUPPORTED;
  const int rc = h2_pal_webrtc_peer_send_opus(
      client->config.webrtc, (h2_pal_webrtc_peer_t *)peer, opus, opus_len);
  if (rc != H2_PAL_OK && rc != H2_PAL_ERR_WOULD_BLOCK) {
    h2_gizclaw_log_webrtc_rc(client, "peer_send_opus", rc);
  }
  return h2_gzc_media_result(rc);
}

static int h2_gzc_peer_add_ice_server(gzc_rtc_peer_t *peer, gzc_str_t url,
                                      gzc_str_t username,
                                      gzc_str_t credential) {
  h2_gizclaw_client_t *client = h2_gizclaw_client_for_peer(peer);
  if (client == NULL || client->config.webrtc == NULL) {
    return GZC_ERR_INVALID_ARGUMENT;
  }
  const h2_pal_webrtc_ice_server_t server = {
      .url = {.data = url.data, .len = url.len},
      .username = {.data = username.data, .len = username.len},
      .credential = {.data = credential.data, .len = credential.len},
  };
  int rc = h2_pal_webrtc_peer_add_ice_server(
      client->config.webrtc, (h2_pal_webrtc_peer_t *)peer, &server);
  h2_gizclaw_log_webrtc_rc(client, "peer_add_ice_server", rc);
  return rc == H2_PAL_OK ? GZC_OK
                         : (rc == H2_PAL_ERR_UNSUPPORTED ? GZC_ERR_UNSUPPORTED
                                                         : GZC_ERR_WEBRTC);
}

static int h2_gzc_peer_set_remote_sdp(gzc_rtc_peer_t *peer,
                                      gzc_rtc_sdp_type_t type, gzc_str_t sdp) {
  h2_gizclaw_client_t *client = h2_gizclaw_client_for_peer(peer);
  if (client == NULL || client->config.webrtc == NULL) {
    return GZC_ERR_INVALID_ARGUMENT;
  }
  h2_pal_webrtc_str_t h2_sdp = {
      .data = sdp.data,
      .len = sdp.len,
  };
  int rc = h2_pal_webrtc_peer_set_remote_sdp(
      client->config.webrtc, (h2_pal_webrtc_peer_t *)peer,
      (h2_pal_webrtc_sdp_type_t)type, h2_sdp);
  h2_gizclaw_log_webrtc_rc(client, "peer_set_remote_sdp", rc);
  return rc == H2_PAL_OK ? GZC_OK : GZC_ERR_WEBRTC;
}

static int
h2_gzc_peer_create_data_channel(gzc_rtc_peer_t *peer,
                                const gzc_rtc_channel_config_t *config,
                                gzc_rtc_channel_t **out_channel) {
  h2_gizclaw_client_t *client = h2_gizclaw_client_for_peer(peer);
  if (client == NULL || client->config.webrtc == NULL || config == NULL ||
      out_channel == NULL) {
    return GZC_ERR_INVALID_ARGUMENT;
  }
  *out_channel = NULL;
  h2_gizclaw_local_channel_state_t *pending = &client->local_channel_state;
  if (pending->create_in_progress || pending->ready) {
    return GZC_ERR_WEBRTC;
  }
  h2_pal_webrtc_channel_config_t h2_config = {
      .label = {.data = config->label.data, .len = config->label.len},
      .ordered = config->ordered ? 1 : 0,
      .reliable = config->reliable ? 1 : 0,
  };
  pending->create_in_progress = true;
  pending->create_label = config->label;
  pending->create_ordered = config->ordered;
  pending->create_reliable = config->reliable;
  h2_pal_webrtc_channel_t *channel = NULL;
  int rc = h2_pal_webrtc_peer_create_data_channel(client->config.webrtc,
                                                  (h2_pal_webrtc_peer_t *)peer,
                                                  &h2_config, &channel);
  pending->create_in_progress = false;
  pending->create_label = (gzc_str_t){0};
  pending->create_ordered = false;
  pending->create_reliable = false;
  h2_gizclaw_log_webrtc_rc(client, "peer_create_data_channel", rc);
  if (rc != H2_PAL_OK) {
    h2_gizclaw_reset_local_channel_state(client);
    return GZC_ERR_WEBRTC;
  }
  if (pending->ready && pending->channel != channel) {
    h2_gizclaw_dispatch_retained_local_channel_state(client, pending->peer);
  }
  h2_gizclaw_mark_local_channel(client, (gzc_rtc_channel_t *)channel);
  *out_channel = (gzc_rtc_channel_t *)channel;
  return GZC_OK;
}

static int h2_gzc_peer_poll(gzc_rtc_peer_t *peer, int timeout_ms) {
  h2_gizclaw_client_t *client = h2_gizclaw_client_for_peer(peer);
  if (client == NULL || client->config.webrtc == NULL) {
    return GZC_ERR_INVALID_ARGUMENT;
  }
  if (h2_gizclaw_is_canceled(client)) {
    h2_gizclaw_reset_local_channel_state(client);
    return GZC_ERR_CLOSED;
  }
  h2_gizclaw_dispatch_retained_local_channel_state(
      client, (h2_pal_webrtc_peer_t *)peer);
  int rc = h2_pal_webrtc_peer_poll(client->config.webrtc,
                                   (h2_pal_webrtc_peer_t *)peer, timeout_ms);
  if (rc != H2_PAL_OK) {
    h2_gizclaw_log_webrtc_rc(client, "peer_poll", rc);
  }
  return rc == H2_PAL_OK ? GZC_OK : GZC_ERR_WEBRTC;
}

static int h2_gzc_channel_send(gzc_rtc_channel_t *channel, const uint8_t *data,
                               size_t len, bool is_text) {
  h2_gizclaw_client_t *client = h2_gizclaw_client_for_channel(channel);
  if (client == NULL || client->config.webrtc == NULL) {
    return GZC_ERR_INVALID_ARGUMENT;
  }
  uint64_t started_ms = 0u;
  int rc = h2_pal_time_get_monotonic_ms(client->config.time, &started_ms);
  if (rc != H2_PAL_OK) {
    return GZC_ERR_WEBRTC;
  }
  const int write_timeout_ms = client->config.write_timeout_ms == 0
                                   ? client->config.connect_timeout_ms
                                   : client->config.write_timeout_ms;
  for (;;) {
    rc = h2_pal_webrtc_channel_send(client->config.webrtc,
                                    (h2_pal_webrtc_channel_t *)channel, data,
                                    len, is_text ? 1 : 0);
    if (rc != H2_PAL_ERR_WOULD_BLOCK) {
      if (rc != H2_PAL_OK) {
        h2_gizclaw_log_webrtc_rc(client, "channel_send", rc);
      }
      return h2_gzc_media_result(rc);
    }
    if (h2_gizclaw_is_canceled(client)) {
      return GZC_ERR_CLOSED;
    }
    uint64_t now_ms = 0u;
    if (h2_pal_time_get_monotonic_ms(client->config.time, &now_ms) !=
        H2_PAL_OK) {
      return GZC_ERR_WEBRTC;
    }
    uint64_t elapsed_ms =
        now_ms >= started_ms ? now_ms - started_ms : UINT64_MAX;
    if (elapsed_ms >= (uint64_t)write_timeout_ms) {
      return GZC_ERR_TIMEOUT;
    }
    if (client->webrtc_peer == NULL) {
      return GZC_ERR_WEBRTC;
    }
    const uint64_t remaining_ms = (uint64_t)write_timeout_ms - elapsed_ms;
    const int poll_timeout_ms =
        (int)(remaining_ms < H2_GIZCLAW_WRITE_POLL_SLICE_MS
                  ? remaining_ms
                  : H2_GIZCLAW_WRITE_POLL_SLICE_MS);
    rc = h2_pal_webrtc_peer_poll(client->config.webrtc, client->webrtc_peer,
                                 poll_timeout_ms);
    if (rc == H2_PAL_ERR_WOULD_BLOCK) {
      const uint32_t backoff_ms =
          remaining_ms < H2_GIZCLAW_WRITE_POLL_BACKOFF_MS
              ? (uint32_t)remaining_ms
              : H2_GIZCLAW_WRITE_POLL_BACKOFF_MS;
      h2_gizclaw_log_webrtc_poll_backoff(client, backoff_ms);
      rc = h2_pal_time_sleep_ms(client->config.time, backoff_ms);
      if (rc != H2_PAL_OK) {
        h2_gizclaw_log_webrtc_rc(client, "peer_poll_backoff", rc);
        return GZC_ERR_WEBRTC;
      }
    } else if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT) {
      return h2_gzc_media_result(rc);
    }
    if (!h2_gizclaw_channel_is_live(client, channel)) {
      return GZC_ERR_CLOSED;
    }
  }
}

static int h2_gzc_channel_buffered_amount(gzc_rtc_channel_t *channel,
                                          uint64_t *out_bytes) {
  h2_gizclaw_client_t *client = h2_gizclaw_client_for_channel(channel);
  if (client == NULL || client->config.webrtc == NULL || out_bytes == NULL ||
      !h2_gizclaw_channel_is_live(client, channel)) {
    return GZC_ERR_INVALID_ARGUMENT;
  }
  /*
   * PAL backends expose backpressure through channel_send(WOULD_BLOCK).
   * h2_gzc_channel_send synchronously waits for peer progress until the
   * bounded backend queue accepts the chunk, so the adapter itself never owns
   * queued bytes.
   */
  *out_bytes = 0u;
  return GZC_OK;
}

static int
h2_gzc_channel_set_buffered_amount_low_threshold(gzc_rtc_channel_t *channel,
                                                 uint64_t bytes) {
  (void)bytes;
  h2_gizclaw_client_t *client = h2_gizclaw_client_for_channel(channel);
  return client != NULL && client->config.webrtc != NULL &&
                 h2_gizclaw_channel_is_live(client, channel)
             ? GZC_OK
             : GZC_ERR_INVALID_ARGUMENT;
}

static void h2_gzc_channel_close(gzc_rtc_channel_t *channel) {
  h2_gizclaw_client_t *client = h2_gizclaw_client_for_channel(channel);
  if (client != NULL && client->config.webrtc != NULL) {
    if (client->local_channel_state.channel ==
        (h2_pal_webrtc_channel_t *)channel) {
      h2_gizclaw_reset_local_channel_state(client);
    }
    h2_gizclaw_unmark_local_channel(client, channel);
    h2_pal_webrtc_channel_close(client->config.webrtc,
                                (h2_pal_webrtc_channel_t *)channel);
  }
}

static void h2_gzc_peer_close(gzc_rtc_peer_t *peer) {
  h2_gizclaw_client_t *client = h2_gizclaw_client_for_peer(peer);
  if (client != NULL && client->config.webrtc != NULL) {
    h2_gizclaw_reset_local_channel_state(client);
    client->opus_frame_callback = NULL;
    client->opus_frame_callback_user = NULL;
    client->opus_frame_peer = NULL;
    h2_pal_webrtc_peer_close(client->config.webrtc,
                             (h2_pal_webrtc_peer_t *)peer);
    if (client->webrtc_peer == (h2_pal_webrtc_peer_t *)peer) {
      client->webrtc_peer = NULL;
    }
  }
}

static int h2_gizclaw_config_valid(const h2_gizclaw_config_t *config) {
  return config != NULL && config->connect_timeout_ms > 0 &&
         config->write_timeout_ms >= 0 && config->allocator != NULL &&
         config->http != NULL && config->webrtc != NULL &&
         config->crypto != NULL && config->time != NULL &&
         config->time->vtable != NULL &&
         config->time->vtable->get_monotonic_ms != NULL &&
         config->server_endpoint.data != NULL &&
         config->private_key.data != NULL;
}

int h2_gizclaw_client_init(const h2_gizclaw_config_t *config,
                           h2_gizclaw_client_t **out_client) {
  if (!h2_gizclaw_config_valid(config) || out_client == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_client = NULL;
  uint64_t monotonic_ms = 0;
  int time_rc = h2_pal_time_get_monotonic_ms(config->time, &monotonic_ms);
  if (time_rc != H2_PAL_OK) {
    return time_rc;
  }
  h2_gizclaw_client_t *client = (h2_gizclaw_client_t *)h2_pal_mem_alloc(
      config->allocator, sizeof(*client));
  if (client == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  memset(client, 0, sizeof(*client));
  client->config = *config;
  client->platform.userdata = client;
  client->platform.malloc = h2_gzc_malloc;
  client->platform.realloc = h2_gzc_realloc;
  client->platform.free = h2_gzc_free;
  client->platform.time_instant_ms = h2_gzc_time_instant_ms;
  client->platform.time_unix_ms = h2_gzc_time_unix_ms;
  client->platform.random = h2_gzc_random;
  client->platform.log = h2_gzc_log;
  client->crypto.userdata = client;
  client->crypto.keypair_from_private = h2_gzc_keypair_from_private;
  client->crypto.dh = h2_gzc_dh;
  client->crypto.hkdf_sha256 = h2_gzc_hkdf_sha256;
  client->crypto.aead_seal = h2_gzc_aead_seal;
  client->crypto.aead_open = h2_gzc_aead_open;
  client->http.userdata = client;
  client->http.request = h2_gzc_http_request;
  client->http.response_free = h2_gzc_http_response_free;
  client->webrtc.userdata = client;
  client->webrtc.peer_create = h2_gzc_peer_create;
  client->webrtc.peer_start_offer = h2_gzc_peer_start_offer_active;
  client->webrtc.peer_set_remote_sdp = h2_gzc_peer_set_remote_sdp;
  client->webrtc.peer_create_data_channel = h2_gzc_peer_create_data_channel;
  client->webrtc.peer_poll = h2_gzc_peer_poll;
  client->webrtc.channel_send = h2_gzc_channel_send;
  client->webrtc.channel_buffered_amount = h2_gzc_channel_buffered_amount;
  client->webrtc.channel_set_buffered_amount_low_threshold =
      h2_gzc_channel_set_buffered_amount_low_threshold;
  client->webrtc.channel_close = h2_gzc_channel_close;
  client->webrtc.peer_close = h2_gzc_peer_close;
  client->media.struct_size = sizeof(client->media);
  client->media.peer_set_opus_frame_callback =
      h2_gzc_peer_set_opus_frame_callback;
  client->media.peer_send_opus = h2_gzc_peer_send_opus;

  gzc_client_config_t gzc_config;
  memset(&gzc_config, 0, sizeof(gzc_config));
  gzc_config.server_endpoint = gzc_str_from_h2(config->server_endpoint);
  gzc_config.private_key = gzc_str_from_h2(config->private_key);
  gzc_config.platform = &client->platform;
  gzc_config.crypto = &client->crypto;
  gzc_config.http = &client->http;
  gzc_config.webrtc = &client->webrtc;
  gzc_config.cipher_mode = (gzc_cipher_mode_t)config->cipher_mode;
  gzc_config.connect_timeout_ms = config->connect_timeout_ms;
  gzc_config.write_timeout_ms = config->write_timeout_ms == 0
                                    ? config->connect_timeout_ms
                                    : config->write_timeout_ms;
  if (config->rpc_provider != NULL) {
    gzc_config.rpc_provider = h2_gizclaw_rpc_provider_bridge;
    gzc_config.rpc_provider_userdata = client;
  }
  int rc = gzc_client_create(&gzc_config, &client->gzc);
  if (rc != GZC_OK) {
    h2_pal_mem_free(config->allocator, client);
    return H2_PAL_ERR_INVALID_ARG;
  }
  rc = gzc_client_set_webrtc_media(client->gzc, &client->media);
  if (rc != GZC_OK) {
    gzc_client_destroy(client->gzc);
    h2_pal_mem_free(config->allocator, client);
    return H2_PAL_ERR_INVALID_STATE;
  }
  rc = gzc_client_set_peer_add_ice_server(client->gzc,
                                          h2_gzc_peer_add_ice_server);
  if (rc != GZC_OK) {
    gzc_client_destroy(client->gzc);
    h2_pal_mem_free(config->allocator, client);
    return H2_PAL_ERR_INVALID_STATE;
  }
  client->next_conversation_stream_sequence = 1u;
  client->next_client = s_clients;
  s_clients = client;
  *out_client = client;
  return H2_PAL_OK;
}

int h2_gizclaw_client_connect(h2_gizclaw_client_t *client) {
  if (client == NULL || client->gzc == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (client->terminal_closed) {
    return H2_PAL_ERR_CLOSED;
  }
  int rc = gzc_client_connect(client->gzc);
  if (rc == GZC_OK) {
    rc = gzc_event_stream_open(client->gzc, client->config.connect_timeout_ms,
                               &client->events);
    if (rc != GZC_OK) {
      (void)gzc_client_close(client->gzc);
    }
  }
  if (rc != GZC_OK) {
    h2_gizclaw_log_error(client, "connect", rc);
    h2_gizclaw_release_event_handle(client);
  }
  return h2_gizclaw_result_from_gzc(rc);
}

int h2_gizclaw_client_poll(h2_gizclaw_client_t *client, int timeout_ms) {
  if (client == NULL || client->gzc == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (client->terminal_closed) {
    return H2_PAL_ERR_CLOSED;
  }
  if (client->event_handler != NULL) {
    const int event_rc =
        h2_gizclaw_client_dispatch_event(client, 0, NULL, NULL);
    if (event_rc != H2_PAL_OK && event_rc != H2_PAL_ERR_TIMEOUT &&
        event_rc != H2_PAL_ERR_WOULD_BLOCK) {
      return event_rc;
    }
  }
  int rc;
#if defined(H2_GIZCLAW_TESTING)
  if (s_test_client_poll != NULL) {
    rc = s_test_client_poll(s_test_client_poll_user, client->gzc, timeout_ms);
  } else
#endif
  {
    rc = gzc_client_poll(client->gzc, timeout_ms);
  }
  if (rc != GZC_OK) {
    h2_gizclaw_log_error(client, "poll", rc);
    if (rc == GZC_ERR_CLOSED) {
      client->terminal_closed = true;
      h2_gizclaw_release_event_handle(client);
    }
  }
  return h2_gizclaw_result_from_gzc(rc);
}

void h2_gizclaw_rpc_response_deinit(h2_gizclaw_client_t *client,
                                    h2_gizclaw_rpc_response_t *response) {
  if (client == NULL || response == NULL) {
    return;
  }
  h2_pal_mem_free(client->config.allocator, response->result_payload);
  h2_pal_mem_free(client->config.allocator, response->error_message);
  memset(response, 0, sizeof(*response));
}

int h2_gizclaw_client_rpc_call(h2_gizclaw_client_t *client,
                               h2_gizclaw_rpc_method_t method,
                               h2_gizclaw_rpc_bytes_t params_payload,
                               h2_gizclaw_rpc_response_t *out_response) {
  if (client == NULL || client->gzc == NULL || method <= 0 ||
      out_response == NULL ||
      (params_payload.len > 0u && params_payload.data == NULL)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_response, 0, sizeof(*out_response));
  if (client->rpc_call_interceptor != NULL) {
    return client->rpc_call_interceptor(client->rpc_interceptor_user, client,
                                        method, params_payload, out_response);
  }
#if defined(H2_GIZCLAW_TESTING)
  if (s_test_rpc_call != NULL) {
    return s_test_rpc_call(s_test_rpc_call_user, client, method, params_payload,
                           out_response);
  }
#endif
  h2_gizclaw_rpc_request_t *request = NULL;
  int rc = h2_gizclaw_client_rpc_request_start(
      client, method, params_payload, 5000u, &request);
  while (rc == H2_PAL_OK &&
         (rc = h2_gizclaw_rpc_request_result(request, out_response)) ==
             H2_PAL_ERR_WOULD_BLOCK) {
    rc = h2_gizclaw_client_poll(client, 10);
    if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK)
      rc = H2_PAL_OK;
  }
  h2_gizclaw_rpc_request_destroy(request);
  if (rc != H2_PAL_OK) {
    h2_gizclaw_rpc_response_deinit(client, out_response);
    return rc;
  }
  if (out_response->has_error) {
    h2_gizclaw_log_rpc_error(client, method, out_response->error_code,
                             out_response->error_message,
                             out_response->error_message_len);
  }
  return H2_PAL_OK;
}

int h2_gizclaw_client_rpc_request_start(
    h2_gizclaw_client_t *client, h2_gizclaw_rpc_method_t method,
    h2_gizclaw_rpc_bytes_t params_payload, uint32_t timeout_ms,
    h2_gizclaw_rpc_request_t **out_request) {
  if (out_request != NULL) {
    *out_request = NULL;
  }
  if (client == NULL || client->gzc == NULL || method <= 0 ||
      timeout_ms == 0u || timeout_ms > INT32_MAX || out_request == NULL ||
      (params_payload.len > 0u && params_payload.data == NULL)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_gizclaw_rpc_request_t *request =
      h2_pal_mem_alloc(client->config.allocator, sizeof(*request));
  if (request == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  memset(request, 0, sizeof(*request));
  request->allocator = client->config.allocator;
  request->client = client;
  const int rc = gzc_rpc_request_start(
      client->gzc, 0u, (gizclaw_rpc_v1_RpcMethod)method,
      gzc_str_from_parts((const char *)params_payload.data, params_payload.len),
      (int)timeout_ms, &request->gzc);
  if (rc != GZC_OK) {
    h2_pal_mem_free(request->allocator, request);
    return h2_gizclaw_result_from_gzc(rc);
  }
  *out_request = request;
  return H2_PAL_OK;
}

int h2_gizclaw_rpc_request_result(h2_gizclaw_rpc_request_t *request,
                                  h2_gizclaw_rpc_response_t *out_response) {
  if (request == NULL || request->gzc == NULL || out_response == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_response, 0, sizeof(*out_response));
  gzc_rpc_response_t response;
  memset(&response, 0, sizeof(response));
  int rc = gzc_rpc_request_result(request->gzc, &response);
  if (rc != GZC_OK) {
    return h2_gizclaw_result_from_gzc(rc);
  }
  out_response->has_error = response.has_error;
  out_response->error_code = response.error.code;
  out_response->result_payload_len = response.result_payload.len;
  out_response->error_message_len = response.error.message.len;
  if (response.result_payload.len > 0u) {
    out_response->result_payload =
        h2_pal_mem_alloc(request->allocator, response.result_payload.len);
    if (out_response->result_payload == NULL) {
      memset(out_response, 0, sizeof(*out_response));
      return H2_PAL_ERR_NO_MEMORY;
    }
    memcpy(out_response->result_payload, response.result_payload.data,
           response.result_payload.len);
  }
  if (response.error.message.len > 0u) {
    out_response->error_message =
        h2_pal_mem_alloc(request->allocator, response.error.message.len);
    if (out_response->error_message == NULL) {
      h2_pal_mem_free(request->allocator, out_response->result_payload);
      memset(out_response, 0, sizeof(*out_response));
      return H2_PAL_ERR_NO_MEMORY;
    }
    memcpy(out_response->error_message, response.error.message.data,
           response.error.message.len);
  }
  return H2_PAL_OK;
}

void h2_gizclaw_rpc_request_cancel(h2_gizclaw_rpc_request_t *request) {
  if (request != NULL) {
    gzc_rpc_request_cancel(request->gzc);
  }
}

void h2_gizclaw_rpc_request_destroy(h2_gizclaw_rpc_request_t *request) {
  if (request == NULL) {
    return;
  }
  gzc_rpc_request_destroy(request->gzc);
  h2_pal_mem_free(request->allocator, request);
}

static int h2_gizclaw_stream_frame(void *user, const gzc_rpc_frame_t *frame) {
  h2_gizclaw_rpc_request_t *request = user;
  if (request == NULL || frame == NULL || request->on_stream_event == NULL) {
    return GZC_ERR_INVALID_ARGUMENT;
  }
  if (frame->type == GZC_RPC_FRAME_EOS) {
    h2_gizclaw_rpc_stream_event_t event = {
        .kind = H2_GIZCLAW_RPC_STREAM_EOS,
    };
    const int rc = request->on_stream_event(request->stream_user, &event);
    if (rc == GZC_OK) {
      request->saw_stream_eos = true;
    }
    return rc;
  }
  h2_gizclaw_rpc_stream_event_t event;
  memset(&event, 0, sizeof(event));
  if (!request->saw_stream_response) {
    gzc_rpc_response_t response;
    memset(&response, 0, sizeof(response));
    int rc = gzc_rpc_decode_response_envelope(
        gzc_str_from_parts((const char *)frame->data, frame->len), &response);
    if (rc != GZC_OK) {
      return rc;
    }
    event.kind = H2_GIZCLAW_RPC_STREAM_RESPONSE;
    event.result_payload = (h2_gizclaw_rpc_bytes_t){
        .data = (const uint8_t *)response.result_payload.data,
        .len = response.result_payload.len,
    };
    event.has_error = response.has_error;
    event.error_code = response.error.code;
    event.error_message = (h2_gizclaw_rpc_bytes_t){
        .data = (const uint8_t *)response.error.message.data,
        .len = response.error.message.len,
    };
    request->saw_stream_response = true;
  } else {
    event.kind = H2_GIZCLAW_RPC_STREAM_DATA;
    event.data = (h2_gizclaw_rpc_bytes_t){
        .data = frame->data,
        .len = frame->len,
    };
  }
  return request->on_stream_event(request->stream_user, &event);
}

int h2_gizclaw_client_rpc_request_start_stream(
    h2_gizclaw_client_t *client, h2_gizclaw_rpc_method_t method,
    h2_gizclaw_rpc_bytes_t params_payload, uint32_t timeout_ms,
    h2_gizclaw_rpc_stream_fn on_event, void *user,
    h2_gizclaw_rpc_request_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (client == NULL || client->gzc == NULL || method <= 0 ||
      timeout_ms == 0u || timeout_ms > INT32_MAX || on_event == NULL ||
      out_request == NULL ||
      (params_payload.len > 0u && params_payload.data == NULL)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_gizclaw_rpc_request_t *request =
      h2_pal_mem_alloc(client->config.allocator, sizeof(*request));
  if (request == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(request, 0, sizeof(*request));
  request->allocator = client->config.allocator;
  request->client = client;
  request->on_stream_event = on_event;
  request->stream_user = user;
  const int rc = gzc_rpc_request_start_stream(
      client->gzc, 0u, (gizclaw_rpc_v1_RpcMethod)method,
      gzc_str_from_parts((const char *)params_payload.data, params_payload.len),
      (int)timeout_ms, h2_gizclaw_stream_frame, request, &request->gzc);
  if (rc != GZC_OK) {
    h2_pal_mem_free(request->allocator, request);
    return h2_gizclaw_result_from_gzc(rc);
  }
  *out_request = request;
  return H2_PAL_OK;
}

int h2_gizclaw_rpc_request_write(h2_gizclaw_rpc_request_t *request,
                                 const uint8_t *data, size_t len) {
  if (request == NULL || request->gzc == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  return h2_gizclaw_result_from_gzc(
      gzc_rpc_request_write(request->gzc, data, len));
}

int h2_gizclaw_rpc_request_finish_write(
    h2_gizclaw_rpc_request_t *request) {
  if (request == NULL || request->gzc == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  return h2_gizclaw_result_from_gzc(
      gzc_rpc_request_finish_write(request->gzc));
}

int h2_gizclaw_client_rpc_call_stream(h2_gizclaw_client_t *client,
                                      h2_gizclaw_rpc_method_t method,
                                      h2_gizclaw_rpc_bytes_t params_payload,
                                      h2_gizclaw_rpc_stream_fn on_event,
                                      void *user) {
  if (client == NULL || client->gzc == NULL || method <= 0 ||
      on_event == NULL ||
      (params_payload.len > 0u && params_payload.data == NULL)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (client->rpc_stream_interceptor != NULL) {
    return client->rpc_stream_interceptor(
        client->rpc_interceptor_user, client, method, params_payload, on_event,
        user);
  }
#if defined(H2_GIZCLAW_TESTING)
  if (s_test_rpc_call_stream != NULL) {
    return s_test_rpc_call_stream(s_test_rpc_call_stream_user, client, method,
                                  params_payload, on_event, user);
  }
#endif
  h2_gizclaw_rpc_request_t *request = NULL;
  int rc = h2_gizclaw_client_rpc_request_start_stream(
      client, method, params_payload, 5000u, on_event, user, &request);
  while (rc == H2_PAL_OK &&
         (rc = h2_gizclaw_rpc_request_finish_write(request)) ==
             H2_PAL_ERR_WOULD_BLOCK) {
    rc = h2_gizclaw_client_poll(client, 10);
    if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK)
      rc = H2_PAL_OK;
  }
  h2_gizclaw_rpc_response_t response = {0};
  while (rc == H2_PAL_OK &&
         (rc = h2_gizclaw_rpc_request_result(request, &response)) ==
             H2_PAL_ERR_WOULD_BLOCK) {
    rc = h2_gizclaw_client_poll(client, 10);
    if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK)
      rc = H2_PAL_OK;
  }
  if (rc == H2_PAL_OK &&
      (!request->saw_stream_response || !request->saw_stream_eos))
    rc = H2_PAL_ERR_IO;
  h2_gizclaw_rpc_response_deinit(client, &response);
  h2_gizclaw_rpc_request_destroy(request);
  return rc;
}

void h2_gizclaw_client_set_rpc_interceptor_internal(
    h2_gizclaw_client_t *client, h2_gizclaw_rpc_call_interceptor_fn call,
    h2_gizclaw_rpc_stream_interceptor_fn stream, void *user) {
  if (client == NULL)
    return;
  client->rpc_call_interceptor = call;
  client->rpc_stream_interceptor = stream;
  client->rpc_interceptor_user = user;
}

int h2_gizclaw_client_ping_measure(h2_gizclaw_client_t *client,
                                   h2_gizclaw_ping_result_t *out_result) {
  if (out_result == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_result = (h2_gizclaw_ping_result_t){0};
  if (client == NULL || client->gzc == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  uint64_t started_ms = 0u;
  int rc = h2_pal_time_get_monotonic_ms(client->config.time, &started_ms);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  gizclaw_rpc_v1_PingRequest ping = gizclaw_rpc_v1_PingRequest_init_zero;
  ping.client_send_time = client->platform.time_unix_ms(client);
  gzc_buf_t params;
  gzc_buf_init(&params);
  rc = h2_gizclaw_encode_pb_message(client, gizclaw_rpc_v1_PingRequest_fields,
                                    &ping, &params);
  if (rc == GZC_OK) {
    h2_gizclaw_rpc_response_t response = {0};
    rc = h2_gizclaw_client_rpc_call(
        client, H2_GIZCLAW_RPC_ALL_PING,
        (h2_gizclaw_rpc_bytes_t){.data = params.data, .len = params.len},
        &response);
    if (rc == H2_PAL_OK && response.has_error) {
      rc = H2_PAL_ERR_IO;
    }
    if (rc == H2_PAL_OK) {
      gizclaw_rpc_v1_PingResponse decoded =
          gizclaw_rpc_v1_PingResponse_init_zero;
      rc = h2_gizclaw_decode_pb_message(
          gzc_str_from_parts((const char *)response.result_payload,
                             response.result_payload_len),
                                        gizclaw_rpc_v1_PingResponse_fields,
                                        &decoded);
      out_result->server_time_ms = decoded.server_time;
    }
    h2_gizclaw_rpc_response_deinit(client, &response);
  }
  gzc_buf_free(&params, &client->platform);
  if (rc != GZC_OK) {
    h2_gizclaw_log_error(client, "ping", rc);
  }
  uint64_t completed_ms = 0u;
  if (rc == GZC_OK) {
    rc = h2_pal_time_get_monotonic_ms(client->config.time, &completed_ms);
  }
  if (rc == H2_PAL_OK) {
    out_result->round_trip_ms =
        h2_pal_time_elapsed_ms(started_ms, completed_ms);
  }
  return rc == H2_PAL_OK || rc == GZC_OK ? H2_PAL_OK : H2_PAL_ERR_IO;
}

int h2_gizclaw_client_ping(h2_gizclaw_client_t *client) {
  h2_gizclaw_ping_result_t result = {0};
  return h2_gizclaw_client_ping_measure(client, &result);
}

#define H2_GIZCLAW_SPEEDTEST_FRAME_BYTES 4096u

typedef struct h2_gizclaw_speedtest_state {
  h2_gizclaw_client_t *client;
  uint64_t expected_upload_bytes;
  uint64_t expected_download_bytes;
  uint64_t download_bytes;
  uint64_t download_started_ms;
  uint64_t download_completed_ms;
  bool saw_response;
  bool saw_eos;
} h2_gizclaw_speedtest_state_t;

static int h2_gizclaw_speedtest_frame(
    void *user, const h2_gizclaw_rpc_stream_event_t *event) {
  h2_gizclaw_speedtest_state_t *state = user;
  if (state == NULL || event == NULL)
    return GZC_ERR_INVALID_ARGUMENT;
  if (event->kind == H2_GIZCLAW_RPC_STREAM_RESPONSE) {
    if (state->saw_response || event->has_error)
      return GZC_ERR_RPC;
    gizclaw_rpc_v1_SpeedTestResponse response =
        gizclaw_rpc_v1_SpeedTestResponse_init_zero;
    const int rc = h2_gizclaw_decode_pb_message(
        gzc_str_from_parts((const char *)event->result_payload.data,
                           event->result_payload.len),
        gizclaw_rpc_v1_SpeedTestResponse_fields, &response);
    if (rc != GZC_OK || response.up_content_length < 0 ||
        response.down_content_length < 0 ||
        (uint64_t)response.up_content_length != state->expected_upload_bytes ||
        (uint64_t)response.down_content_length !=
            state->expected_download_bytes) {
      return GZC_ERR_RPC;
    }
    state->saw_response = true;
    if (state->expected_download_bytes > 0u &&
        h2_pal_time_get_monotonic_ms(state->client->config.time,
                                     &state->download_started_ms) !=
            H2_PAL_OK) {
      return GZC_ERR_RPC;
    }
    return GZC_OK;
  }
  if (event->kind == H2_GIZCLAW_RPC_STREAM_DATA) {
    if (!state->saw_response || state->saw_eos ||
        event->data.len > state->expected_download_bytes ||
        state->download_bytes >
            state->expected_download_bytes - event->data.len) {
      return GZC_ERR_RPC;
    }
    state->download_bytes += event->data.len;
    return GZC_OK;
  }
  if (event->kind != H2_GIZCLAW_RPC_STREAM_EOS || !state->saw_response ||
      state->saw_eos ||
      state->download_bytes != state->expected_download_bytes) {
    return GZC_ERR_RPC;
  }
  state->saw_eos = true;
  return h2_pal_time_get_monotonic_ms(state->client->config.time,
                                      &state->download_completed_ms) ==
                 H2_PAL_OK
             ? GZC_OK
             : GZC_ERR_RPC;
}

int h2_gizclaw_client_speedtest_measure(
    h2_gizclaw_client_t *client, size_t upload_bytes, size_t download_bytes,
    h2_gizclaw_speedtest_result_t *out_result) {
  if (out_result == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_result = (h2_gizclaw_speedtest_result_t){0};
  if (client == NULL || client->gzc == NULL ||
      (upload_bytes == 0u && download_bytes == 0u) ||
      upload_bytes > H2_GIZCLAW_SPEEDTEST_MAX_BYTES ||
      download_bytes > H2_GIZCLAW_SPEEDTEST_MAX_BYTES) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  gizclaw_rpc_v1_SpeedTestRequest speed =
      gizclaw_rpc_v1_SpeedTestRequest_init_zero;
  speed.down_content_length = (int64_t)download_bytes;
  speed.up_content_length = (int64_t)upload_bytes;
#if defined(H2_GIZCLAW_TESTING)
  if (s_test_speed_test != NULL) {
    const h2_gizclaw_test_speed_test_request_t test_request = {
        .up_content_length = speed.up_content_length,
        .down_content_length = speed.down_content_length,
    };
    h2_gizclaw_test_speed_test_result_t test_result = {0};
    const int test_rc = s_test_speed_test(s_test_speed_test_user, client,
                                          &test_request, &test_result);
    if (test_rc != GZC_OK || test_result.up_bytes != speed.up_content_length ||
        test_result.down_bytes != speed.down_content_length ||
        test_result.up_duration_ms < 0 || test_result.down_duration_ms < 0) {
      return H2_PAL_ERR_IO;
    }
    out_result->upload_bytes = (uint64_t)test_result.up_bytes;
    out_result->download_bytes = (uint64_t)test_result.down_bytes;
    out_result->upload_elapsed_ms =
        test_result.up_duration_ms > 0 ? (uint64_t)test_result.up_duration_ms
                                      : (upload_bytes > 0u ? 1u : 0u);
    out_result->elapsed_ms =
        test_result.down_duration_ms > 0
            ? (uint64_t)test_result.down_duration_ms
            : (download_bytes > 0u ? 1u : 0u);
    out_result->upload_bits_per_second = h2_gizclaw_bits_per_second(
        out_result->upload_bytes, out_result->upload_elapsed_ms);
    out_result->download_bits_per_second = h2_gizclaw_bits_per_second(
        out_result->download_bytes, out_result->elapsed_ms);
    return H2_PAL_OK;
  }
#endif
  gzc_buf_t params;
  gzc_buf_init(&params);
  int rc = h2_gizclaw_encode_pb_message(
      client, gizclaw_rpc_v1_SpeedTestRequest_fields, &speed, &params);
  h2_gizclaw_speedtest_state_t state = {
      .client = client,
      .expected_upload_bytes = upload_bytes,
      .expected_download_bytes = download_bytes,
  };
  h2_gizclaw_rpc_request_t *request = NULL;
  if (rc == GZC_OK) {
    rc = h2_gizclaw_client_rpc_request_start_stream(
        client, H2_GIZCLAW_RPC_ALL_SPEED_TEST_RUN,
        (h2_gizclaw_rpc_bytes_t){.data = params.data, .len = params.len},
        30000u, h2_gizclaw_speedtest_frame, &state, &request);
  }
  gzc_buf_free(&params, &client->platform);
  static const uint8_t upload_frame[H2_GIZCLAW_SPEEDTEST_FRAME_BYTES];
  uint64_t upload_started_ms = 0u;
  uint64_t upload_completed_ms = 0u;
  size_t uploaded = 0u;
  if (rc == H2_PAL_OK && upload_bytes > 0u)
    rc = h2_pal_time_get_monotonic_ms(client->config.time,
                                      &upload_started_ms);
  while (rc == H2_PAL_OK && uploaded < upload_bytes) {
    size_t count = upload_bytes - uploaded;
    if (count > sizeof(upload_frame))
      count = sizeof(upload_frame);
    rc = h2_gizclaw_rpc_request_write(request, upload_frame, count);
    if (rc == H2_PAL_OK) {
      uploaded += count;
    } else if (rc == H2_PAL_ERR_WOULD_BLOCK) {
      rc = h2_gizclaw_client_poll(client, 10);
      if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK)
        rc = H2_PAL_OK;
    }
  }
  while (rc == H2_PAL_OK &&
         (rc = h2_gizclaw_rpc_request_finish_write(request)) ==
             H2_PAL_ERR_WOULD_BLOCK) {
    rc = h2_gizclaw_client_poll(client, 10);
    if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK)
      rc = H2_PAL_OK;
  }
  if (rc == H2_PAL_OK && upload_bytes > 0u)
    rc = h2_pal_time_get_monotonic_ms(client->config.time,
                                      &upload_completed_ms);
  h2_gizclaw_rpc_response_t response = {0};
  while (rc == H2_PAL_OK &&
         (rc = h2_gizclaw_rpc_request_result(request, &response)) ==
             H2_PAL_ERR_WOULD_BLOCK) {
    rc = h2_gizclaw_client_poll(client, 10);
    if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK)
      rc = H2_PAL_OK;
  }
  if (rc == H2_PAL_OK &&
      (response.has_error || !state.saw_response || !state.saw_eos ||
       uploaded != upload_bytes || state.download_bytes != download_bytes)) {
    rc = H2_PAL_ERR_IO;
  }
  h2_gizclaw_rpc_response_deinit(client, &response);
  h2_gizclaw_rpc_request_destroy(request);
  if (rc != H2_PAL_OK)
    return rc;
  out_result->upload_bytes = uploaded;
  out_result->download_bytes = state.download_bytes;
  if (uploaded > 0u) {
    out_result->upload_elapsed_ms = h2_pal_time_elapsed_ms(
        upload_started_ms, upload_completed_ms);
    if (out_result->upload_elapsed_ms == 0u)
      out_result->upload_elapsed_ms = 1u;
    out_result->upload_bits_per_second = h2_gizclaw_bits_per_second(
        out_result->upload_bytes, out_result->upload_elapsed_ms);
  }
  if (state.download_bytes > 0u) {
    out_result->elapsed_ms = h2_pal_time_elapsed_ms(
        state.download_started_ms, state.download_completed_ms);
    if (out_result->elapsed_ms == 0u)
      out_result->elapsed_ms = 1u;
    out_result->download_bits_per_second = h2_gizclaw_bits_per_second(
        out_result->download_bytes, out_result->elapsed_ms);
  }
  return H2_PAL_OK;
}

int h2_gizclaw_client_speedtest_download(
    h2_gizclaw_client_t *client, size_t download_bytes,
    h2_gizclaw_speedtest_result_t *out_result) {
  return h2_gizclaw_client_speedtest_measure(client, 0u, download_bytes,
                                             out_result);
}

int h2_gizclaw_client_speedtest(h2_gizclaw_client_t *client) {
  h2_gizclaw_speedtest_result_t result = {0};
  return h2_gizclaw_client_speedtest_measure(client, 1024u * 1024u,
                                             1024u * 1024u, &result);
}

int h2_gizclaw_client_delete_peer(h2_gizclaw_client_t *client) {
  h2_gizclaw_rpc_response_t response = {0};
  int rc = h2_gizclaw_client_rpc_call(client, H2_GIZCLAW_RPC_SERVER_PEER_DELETE,
                                      (h2_gizclaw_rpc_bytes_t){0}, &response);
  if (rc == H2_PAL_OK && response.has_error) {
    rc = H2_PAL_ERR_IO;
  }
  h2_gizclaw_rpc_response_deinit(client, &response);
  return rc;
}

int h2_gizclaw_client_close(h2_gizclaw_client_t *client) {
  if (client == NULL || client->gzc == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  client->terminal_closed = true;
  h2_gizclaw_reset_local_channel_state(client);
  h2_gizclaw_release_event_handle(client);
  int rc = gzc_client_close(client->gzc);
  return rc == GZC_OK ? H2_PAL_OK : H2_PAL_ERR_IO;
}

void h2_gizclaw_client_deinit(h2_gizclaw_client_t *client) {
  if (client == NULL) {
    return;
  }
  const h2_pal_mem_api_t *allocator = client->config.allocator;
  if (client->gzc != NULL) {
    h2_gizclaw_reset_local_channel_state(client);
    h2_gizclaw_release_event_handle(client);
    gzc_client_destroy(client->gzc);
    client->gzc = NULL;
  }
  h2_gizclaw_client_t **cursor = &s_clients;
  while (*cursor != NULL && *cursor != client) {
    cursor = &(*cursor)->next_client;
  }
  if (*cursor == client) {
    *cursor = client->next_client;
  }
#if defined(H2_GIZCLAW_TESTING)
  if (s_test_webrtc_client == client) {
    s_test_webrtc_client = NULL;
  }
#endif
  h2_pal_mem_free(allocator, client);
}

#if defined(H2_GIZCLAW_TESTING)
bool h2_gizclaw_test_media_registered(h2_gizclaw_client_t *client) {
  return client != NULL && client->media.struct_size == sizeof(client->media) &&
         client->media.peer_set_opus_frame_callback != NULL &&
         client->media.peer_send_opus != NULL;
}

int h2_gizclaw_test_peer_create(h2_gizclaw_client_t *client,
                                h2_pal_webrtc_peer_t **out_peer) {
  if (client == NULL || out_peer == NULL)
    return GZC_ERR_INVALID_ARGUMENT;
  const gzc_webrtc_callbacks_t callbacks = {0};
  return h2_gzc_peer_create(client, &callbacks, (gzc_rtc_peer_t **)out_peer);
}

int h2_gizclaw_test_media_send_opus(h2_gizclaw_client_t *client,
                                    gzc_rtc_peer_t *peer, const uint8_t *opus,
                                    size_t opus_len) {
  if (client == NULL) {
    return GZC_ERR_INVALID_ARGUMENT;
  }
  s_test_webrtc_client = client;
  return h2_gzc_peer_send_opus(peer, opus, opus_len);
}

int h2_gizclaw_test_media_set_opus_frame_callback(
    h2_gizclaw_client_t *client, gzc_rtc_peer_t *peer,
    gzc_rtc_opus_frame_cb callback, void *callback_user) {
  if (client == NULL) {
    return GZC_ERR_INVALID_ARGUMENT;
  }
  s_test_webrtc_client = client;
  return h2_gzc_peer_set_opus_frame_callback(peer, callback, callback_user);
}

void h2_gizclaw_test_media_emit_opus(h2_gizclaw_client_t *client,
                                     gzc_rtc_peer_t *peer, const uint8_t *opus,
                                     size_t opus_len) {
  h2_gzc_opus_frame(client, (h2_pal_webrtc_peer_t *)peer, opus, opus_len);
}

void h2_gizclaw_test_media_close_peer(h2_gizclaw_client_t *client,
                                      gzc_rtc_peer_t *peer) {
  if (client == NULL) {
    return;
  }
  s_test_webrtc_client = client;
  h2_gzc_peer_close(peer);
}

int h2_gizclaw_test_try_write_bytes(h2_gizclaw_client_t *client,
                                    h2_pal_webrtc_channel_t *channel,
                                    const uint8_t *data, size_t len,
                                    size_t *offset, bool *blocked) {
  if (client == NULL) {
    return GZC_ERR_INVALID_ARGUMENT;
  }
  s_test_webrtc_client = client;
  client->webrtc_peer = (h2_pal_webrtc_peer_t *)(uintptr_t)1u;
  h2_gizclaw_mark_local_channel(client, (gzc_rtc_channel_t *)channel);
  int rc = gzc_client_try_write_bytes_internal(client->gzc,
                                               (gzc_rtc_channel_t *)channel,
                                               data, len, offset, blocked, 1u);
  h2_gizclaw_unmark_local_channel(client, (gzc_rtc_channel_t *)channel);
  return rc;
}
#endif
