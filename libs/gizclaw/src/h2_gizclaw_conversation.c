#include "h2_gizclaw_conversation.h"

#include "h2_gizclaw_client.h"
#include "h2_gizclaw_internal.h"
#include "h2_gizclaw_workspace.h"

#include "events/peer_event.pb.h"
#include "gzc_buffer.h"
#include "gzc_client.h"
#include "gzc_common.h"
#include "gzc_event.h"

#include <stdio.h>
#include <string.h>

struct h2_gizclaw_conversation {
  h2_gizclaw_client_t *client;
  const h2_pal_mem_api_t *allocator;
  gzc_client_t *gzc;
  gzc_event_stream_t *events;
  uint64_t generation;
  char workspace_name[H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES + 1u];
  char stream_id[H2_GIZCLAW_CONVERSATION_STREAM_ID_MAX_BYTES + 1u];
  char response_stream_id[H2_GIZCLAW_CONVERSATION_STREAM_ID_MAX_BYTES + 1u];
  char transcript_stream_id[H2_GIZCLAW_CONVERSATION_STREAM_ID_MAX_BYTES + 1u];
  char assistant_stream_id[H2_GIZCLAW_CONVERSATION_STREAM_ID_MAX_BYTES + 1u];
  uint64_t sequence;
  bool input_ready;
  bool committed;
  bool canceled;
  bool terminal_pending;
  bool terminal_has_error;
  bool terminal_retryable;
  bool pending_peer_event;
  gzc_peer_event_t peer_event;
  uint8_t audio[H2_GIZCLAW_CONVERSATION_OPUS_MAX_BYTES];
  char text[H2_GIZCLAW_CONVERSATION_TEXT_MAX_BYTES + 1u];
  char error_code[65];
};

#if defined(H2_GIZCLAW_TESTING)
static h2_gizclaw_test_conversation_packet_send_fn s_test_packet_send;
static void *s_test_conversation_ops_user;

void h2_gizclaw_test_set_conversation_ops(
    h2_gizclaw_test_conversation_packet_send_fn packet_send, void *user) {
  s_test_packet_send = packet_send;
  s_test_conversation_ops_user = user;
}
#endif

static int gzc_to_pal(int rc) {
  switch (rc) {
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
  default:
    return H2_PAL_ERR_IO;
  }
}

static bool event_transport_failed(int rc) {
  return rc != GZC_OK && rc != GZC_ERR_TIMEOUT && rc != GZC_ERR_WOULD_BLOCK;
}

static int conversation_send_packet(h2_gizclaw_conversation_t *conversation,
                                    uint8_t protocol, const uint8_t *payload,
                                    size_t payload_len) {
#if defined(H2_GIZCLAW_TESTING)
  if (s_test_packet_send != NULL) {
    return s_test_packet_send(s_test_conversation_ops_user, conversation->gzc,
                              protocol, payload, payload_len);
  }
#endif
  return gzc_client_send_packet(conversation->gzc, protocol, payload,
                                payload_len);
}

static bool valid_workspace(h2_gizclaw_str_t workspace_name) {
  return workspace_name.data != NULL && workspace_name.len > 0u &&
         workspace_name.len <= H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES &&
         memchr(workspace_name.data, '\0', workspace_name.len) == NULL;
}

static const char *peer_event_stream_id(const gzc_peer_event_t *event) {
  if (event == NULL)
    return NULL;
  switch (event->type) {
  case gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_TEXT_DELTA:
    return event->payload.text_delta.stream_id;
  case gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_TEXT_DONE:
    return event->payload.text_done.stream_id;
  case gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_EOS:
    return event->payload.eos.stream_id;
  default:
    return NULL;
  }
}

static const char *peer_event_label(const gzc_peer_event_t *event) {
  if (event == NULL)
    return NULL;
  switch (event->type) {
  case gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_TEXT_DELTA:
    return event->payload.text_delta.label;
  case gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_TEXT_DONE:
    return event->payload.text_done.label;
  case gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_EOS:
    return event->payload.eos.label;
  default:
    return NULL;
  }
}

static bool stream_id_matches(const char *actual, const char *expected) {
  if (actual == NULL || expected == NULL)
    return false;
  const size_t expected_len = strlen(expected);
  return strcmp(actual, expected) == 0 ||
         (expected_len > 0u && strncmp(actual, expected, expected_len) == 0 &&
          actual[expected_len] == ':');
}

static bool peer_event_matches_stream(const gzc_peer_event_t *event,
                                      const char *input_stream_id,
                                      char *response_stream_id,
                                      size_t response_stream_id_capacity) {
  if (event == NULL || input_stream_id == NULL)
    return false;
  const char *event_stream_id = peer_event_stream_id(event);
  if (event_stream_id == NULL ||
      stream_id_matches(event_stream_id, input_stream_id))
    return true;
  if (response_stream_id == NULL || response_stream_id_capacity == 0u)
    return false;
  if (response_stream_id[0] == '\0') {
    const size_t event_stream_id_len = strlen(event_stream_id);
    if (event_stream_id_len >= response_stream_id_capacity)
      return false;
    memcpy(response_stream_id, event_stream_id, event_stream_id_len + 1u);
    return true;
  }
  return stream_id_matches(event_stream_id, response_stream_id);
}

static bool conversation_event_matches_routes(
    const gzc_peer_event_t *event, const char *input_stream_id,
    char *response_stream_id, char *transcript_stream_id,
    char *assistant_stream_id, size_t response_stream_id_capacity) {
  char *selected_stream_id = response_stream_id;
  const char *label = peer_event_label(event);
  if (label != NULL && strcmp(label, "transcript") == 0)
    selected_stream_id = transcript_stream_id;
  else if (label != NULL && strcmp(label, "assistant") == 0)
    selected_stream_id = assistant_stream_id;
  return peer_event_matches_stream(event, input_stream_id, selected_stream_id,
                                   response_stream_id_capacity);
}

bool h2_gizclaw_conversation_accepts_peer_event_internal(
    h2_gizclaw_conversation_t *conversation, const gzc_peer_event_t *event) {
  return conversation != NULL && conversation_event_matches_routes(
                                     event, conversation->stream_id,
                                     conversation->response_stream_id,
                                     conversation->transcript_stream_id,
                                     conversation->assistant_stream_id,
                                     sizeof(conversation->response_stream_id));
}

bool h2_gizclaw_conversation_has_pending_peer_event_internal(
    const h2_gizclaw_conversation_t *conversation) {
  return conversation != NULL && conversation->pending_peer_event;
}

void h2_gizclaw_conversation_enqueue_peer_event_internal(
    h2_gizclaw_conversation_t *conversation, const gzc_peer_event_t *event) {
  if (conversation == NULL || event == NULL || conversation->pending_peer_event)
    return;
  conversation->peer_event = *event;
  conversation->pending_peer_event = true;
}

#if defined(H2_GIZCLAW_TESTING)
bool h2_gizclaw_test_peer_event_matches_stream(const gzc_peer_event_t *event,
                                               const char *stream_id) {
  return peer_event_matches_stream(event, stream_id, NULL, 0u);
}

bool h2_gizclaw_test_peer_event_matches_conversation(
    const gzc_peer_event_t *event, const char *input_stream_id,
    char *response_stream_id, char *transcript_stream_id,
    char *assistant_stream_id, size_t response_stream_id_capacity) {
  return conversation_event_matches_routes(
      event, input_stream_id, response_stream_id, transcript_stream_id,
      assistant_stream_id, response_stream_id_capacity);
}
#endif

static int send_boundary(h2_gizclaw_conversation_t *conversation, bool end,
                         uint64_t timestamp_ms, const char *error_code) {
  if (conversation == NULL || conversation->events == NULL ||
      !h2_gizclaw_client_conversation_active_internal(conversation->client,
                                                      conversation))
    return H2_PAL_ERR_INVALID_STATE;
  gzc_peer_event_t event = gizclaw_events_v1_PeerEvent_init_zero;
  event.version = GZC_PEER_EVENT_VERSION;
  if (!end) {
    event.type = gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_BOS;
    event.which_payload = gizclaw_events_v1_PeerEvent_bos_tag;
    (void)snprintf(event.payload.bos.stream_id,
                   sizeof(event.payload.bos.stream_id), "%s",
                   conversation->stream_id);
    event.payload.bos.sequence = conversation->sequence++;
    event.payload.bos.timestamp_unix_ms = (int64_t)timestamp_ms;
    event.payload.bos.kind = gizclaw_events_v1_StreamKind_STREAM_KIND_AUDIO;
    (void)snprintf(event.payload.bos.label, sizeof(event.payload.bos.label),
                   "%s", "demo-home");
    (void)snprintf(event.payload.bos.mime_type,
                   sizeof(event.payload.bos.mime_type), "%s", "audio/opus");
  } else {
    event.type = gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_EOS;
    event.which_payload = gizclaw_events_v1_PeerEvent_eos_tag;
    (void)snprintf(event.payload.eos.stream_id,
                   sizeof(event.payload.eos.stream_id), "%s",
                   conversation->stream_id);
    event.payload.eos.sequence = conversation->sequence++;
    event.payload.eos.timestamp_unix_ms = (int64_t)timestamp_ms;
    event.payload.eos.kind = gizclaw_events_v1_StreamKind_STREAM_KIND_AUDIO;
    (void)snprintf(event.payload.eos.label, sizeof(event.payload.eos.label),
                   "%s", "demo-home");
    (void)snprintf(event.payload.eos.mime_type,
                   sizeof(event.payload.eos.mime_type), "%s", "audio/opus");
    if (error_code != NULL) {
      event.payload.eos.has_error = true;
      (void)snprintf(event.payload.eos.error.code,
                     sizeof(event.payload.eos.error.code), "%s", error_code);
      event.payload.eos.error.retryable = true;
    }
  }
  const int gzc_rc =
      h2_gizclaw_event_stream_send_internal(conversation->events, &event);
  if (event_transport_failed(gzc_rc))
    h2_gizclaw_client_event_failure_internal(conversation->client,
                                             conversation);
  return gzc_to_pal(gzc_rc);
}

int h2_gizclaw_conversation_open(h2_gizclaw_client_t *client,
                                 h2_gizclaw_str_t workspace_name,
                                 uint64_t generation, int timeout_ms,
                                 h2_gizclaw_conversation_t **out_conversation) {
  if (client == NULL || out_conversation == NULL ||
      !valid_workspace(workspace_name) || timeout_ms <= 0) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_conversation = NULL;
  const h2_pal_mem_api_t *allocator =
      h2_gizclaw_client_allocator_internal(client);
  gzc_client_t *gzc = h2_gizclaw_client_gzc_internal(client);
  if (allocator == NULL || gzc == NULL)
    return H2_PAL_ERR_INVALID_STATE;
  h2_gizclaw_conversation_t *conversation =
      h2_pal_mem_alloc(allocator, sizeof(*conversation));
  if (conversation == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(conversation, 0, sizeof(*conversation));
  conversation->client = client;
  conversation->allocator = allocator;
  conversation->gzc = gzc;
  conversation->generation = generation;
  memcpy(conversation->workspace_name, workspace_name.data, workspace_name.len);
  conversation->workspace_name[workspace_name.len] = '\0';
  uint64_t stream_sequence = 0u;
  int rc = h2_gizclaw_client_conversation_acquire_internal(
      client, conversation, &conversation->events, &stream_sequence);
  if (rc != H2_PAL_OK) {
    h2_pal_mem_free(allocator, conversation);
    return rc;
  }
  const int stream_len =
      snprintf(conversation->stream_id, sizeof(conversation->stream_id),
               "demo-%llu", (unsigned long long)stream_sequence);
  if (stream_len <= 0 ||
      (size_t)stream_len >= sizeof(conversation->stream_id)) {
    h2_gizclaw_client_conversation_release_internal(client, conversation);
    h2_pal_mem_free(allocator, conversation);
    return H2_PAL_ERR_INVALID_ARG;
  }
  (void)timeout_ms;
  rc = send_boundary(conversation, false, 0u, NULL);
  if (rc != H2_PAL_OK) {
    h2_gizclaw_client_conversation_release_internal(client, conversation);
    h2_pal_mem_free(allocator, conversation);
    return rc;
  }
  conversation->input_ready = true;
  *out_conversation = conversation;
  return H2_PAL_OK;
}

bool h2_gizclaw_conversation_input_ready(
    const h2_gizclaw_conversation_t *conversation) {
  return conversation != NULL && conversation->input_ready &&
         !conversation->committed && !conversation->canceled &&
         h2_gizclaw_client_conversation_active_internal(conversation->client,
                                                        conversation);
}

int h2_gizclaw_conversation_write_opus(h2_gizclaw_conversation_t *conversation,
                                       const uint8_t *opus, size_t opus_len,
                                       uint64_t timestamp_ms) {
  if (conversation == NULL || opus == NULL || opus_len == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  if (!h2_gizclaw_conversation_input_ready(conversation))
    return H2_PAL_ERR_INVALID_STATE;
  if (opus_len > H2_GIZCLAW_CONVERSATION_OPUS_MAX_BYTES)
    return H2_PAL_ERR_INVALID_ARG;
  (void)timestamp_ms;
  return gzc_to_pal(conversation_send_packet(
      conversation, GZC_PROTOCOL_OPUS_PACKET, opus, opus_len));
}

int h2_gizclaw_conversation_commit(h2_gizclaw_conversation_t *conversation,
                                   uint64_t timestamp_ms) {
  if (conversation == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (conversation->canceled)
    return H2_PAL_ERR_CLOSED;
  if (conversation->committed)
    return H2_PAL_OK;
  if (!conversation->input_ready)
    return H2_PAL_ERR_INVALID_STATE;
  const int rc = send_boundary(conversation, true, timestamp_ms, NULL);
  if (rc == H2_PAL_OK) {
    conversation->committed = true;
    conversation->input_ready = false;
  }
  return rc;
}

static int poll_audio(h2_gizclaw_conversation_t *conversation,
                      h2_gizclaw_conversation_event_t *out_event) {
  gzc_buf_t payload;
  gzc_buf_init(&payload);
  uint8_t protocol = 0u;
  const int gzc_rc = h2_gizclaw_client_read_packet_internal(
      conversation->gzc, 0, &protocol, &payload);
  if (gzc_rc != GZC_OK) {
    gzc_buf_free(&payload, gzc_client_platform(conversation->gzc));
    if (gzc_rc == GZC_ERR_CLOSED)
      h2_gizclaw_client_event_failure_internal(conversation->client,
                                               conversation);
    return gzc_rc;
  }
  int rc = GZC_ERR_RPC;
  if (protocol == GZC_PROTOCOL_OPUS_PACKET && payload.len > 0u &&
      payload.len <= sizeof(conversation->audio)) {
    const size_t audio_len = payload.len;
    memcpy(conversation->audio, payload.data, audio_len);
    *out_event = (h2_gizclaw_conversation_event_t){
        .kind = H2_GIZCLAW_CONVERSATION_EVENT_REPLY_AUDIO,
        .generation = conversation->generation,
        .audio = conversation->audio,
        .audio_len = audio_len,
    };
    rc = GZC_OK;
  }
  gzc_buf_free(&payload, gzc_client_platform(conversation->gzc));
  return rc;
}

static void copy_text(char *out, size_t capacity, const char *text,
                      size_t *out_len) {
  size_t len = 0u;
  while (len + 1u < capacity && text[len] != '\0')
    ++len;
  memcpy(out, text, len);
  out[len] = '\0';
  *out_len = len;
}

int h2_gizclaw_conversation_poll(h2_gizclaw_conversation_t *conversation,
                                 int timeout_ms,
                                 h2_gizclaw_conversation_event_t *out_event) {
  if (conversation == NULL || out_event == NULL || timeout_ms < 0)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_event, 0, sizeof(*out_event));
  if (conversation->canceled)
    return H2_PAL_ERR_CLOSED;
  if (!h2_gizclaw_client_conversation_active_internal(conversation->client,
                                                      conversation))
    return H2_PAL_ERR_CLOSED;
  int rc = poll_audio(conversation, out_event);
  if (rc == GZC_OK)
    return H2_PAL_OK;
  if (rc != GZC_ERR_TIMEOUT && rc != GZC_ERR_WOULD_BLOCK)
    return gzc_to_pal(rc);
  if (conversation->terminal_pending) {
    conversation->terminal_pending = false;
    if (conversation->terminal_has_error) {
      out_event->kind = H2_GIZCLAW_CONVERSATION_EVENT_ERROR;
      out_event->generation = conversation->generation;
      out_event->error_code = conversation->error_code;
      out_event->retryable = conversation->terminal_retryable;
    } else {
      out_event->kind = H2_GIZCLAW_CONVERSATION_EVENT_REPLY_DONE;
      out_event->generation = conversation->generation;
    }
    return H2_PAL_OK;
  }

  if (!conversation->pending_peer_event) {
    const h2_pal_result_t dispatch_rc =
        (h2_pal_result_t)h2_gizclaw_client_dispatch_event(
            conversation->client, timeout_ms, NULL, NULL);
    if (dispatch_rc != H2_PAL_OK)
      return dispatch_rc;
  }
  if (!conversation->pending_peer_event)
    return H2_PAL_ERR_WOULD_BLOCK;
  const gzc_peer_event_t event = conversation->peer_event;
  conversation->pending_peer_event = false;
  size_t text_len = 0u;
  *out_event = (h2_gizclaw_conversation_event_t){
      .generation = conversation->generation,
  };
  switch (event.type) {
  case gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_TEXT_DELTA:
    copy_text(conversation->text, sizeof(conversation->text),
              event.payload.text_delta.text, &text_len);
    out_event->kind = H2_GIZCLAW_CONVERSATION_EVENT_TEXT_DELTA;
    out_event->text = conversation->text;
    out_event->text_len = text_len;
    return H2_PAL_OK;
  case gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_TEXT_DONE:
    copy_text(conversation->text, sizeof(conversation->text),
              event.payload.text_done.text, &text_len);
    out_event->kind = H2_GIZCLAW_CONVERSATION_EVENT_TEXT_DONE;
    out_event->text = conversation->text;
    out_event->text_len = text_len;
    return H2_PAL_OK;
  case gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_EOS:
    conversation->terminal_pending = true;
    if (event.payload.eos.has_error) {
      (void)snprintf(conversation->error_code, sizeof(conversation->error_code),
                     "%s", event.payload.eos.error.code);
      conversation->terminal_has_error = true;
      conversation->terminal_retryable = event.payload.eos.error.retryable;
    }
    out_event->kind = H2_GIZCLAW_CONVERSATION_EVENT_NONE;
    return H2_PAL_OK;
  default:
    out_event->kind = H2_GIZCLAW_CONVERSATION_EVENT_NONE;
    return H2_PAL_OK;
  }
}

void h2_gizclaw_conversation_cancel(h2_gizclaw_conversation_t *conversation) {
  if (conversation == NULL || conversation->canceled)
    return;
  if (!conversation->committed && conversation->events != NULL)
    (void)send_boundary(conversation, true, 0u, "canceled");
  conversation->canceled = true;
  conversation->input_ready = false;
}

void h2_gizclaw_conversation_deinit(h2_gizclaw_conversation_t *conversation) {
  if (conversation == NULL)
    return;
  h2_gizclaw_conversation_cancel(conversation);
  h2_gizclaw_client_conversation_release_internal(conversation->client,
                                                  conversation);
  conversation->client = NULL;
  conversation->gzc = NULL;
  conversation->events = NULL;
  h2_pal_mem_free(conversation->allocator, conversation);
}

void h2_gizclaw_conversation_invalidate_internal(
    h2_gizclaw_conversation_t *conversation) {
  if (conversation == NULL)
    return;
  conversation->client = NULL;
  conversation->gzc = NULL;
  conversation->events = NULL;
  conversation->input_ready = false;
  conversation->canceled = true;
}
