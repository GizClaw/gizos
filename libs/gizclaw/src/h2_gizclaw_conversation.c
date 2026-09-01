#include "h2_gizclaw_conversation.h"

#include "h2_gizclaw_client.h"
#include "h2_gizclaw_internal.h"
#include "h2_gizclaw_service_internal.h"
#include "h2_gizclaw_workspace.h"

#include "events/peer_event.pb.h"
#include "gzc_buffer.h"
#include "gzc_client.h"
#include "gzc_common.h"
#include "gzc_event.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define H2_GIZCLAW_CONVERSATION_REQUEST_QUEUE_ITEMS 8u

typedef struct h2_gizclaw_conversation_request_message {
  size_t len;
  uint64_t timestamp_ms;
  uint8_t opus[H2_GIZCLAW_CONVERSATION_OPUS_MAX_BYTES];
} h2_gizclaw_conversation_request_message_t;

struct h2_gizclaw_conversation_request {
  h2_gizclaw_service_t *service;
  h2_gizclaw_operation_t *operation;
  h2_gizclaw_conversation_t *conversation;
  h2_pal_queue_t *queue;
  h2_gizclaw_conversation_request_event_fn on_event;
  h2_gizclaw_conversation_request_completion_fn completion;
  void *user;
  char workspace_name[H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES + 1u];
  size_t workspace_name_len;
  uint64_t generation;
  int timeout_ms;
  h2_gizclaw_conversation_request_message_t pending_message;
  h2_gizclaw_conversation_event_t dispatch_event;
  h2_gizclaw_operation_result_t operation_result;
  bool has_pending_message;
  atomic_bool committed;
  atomic_bool terminal;
};

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
  return conversation != NULL &&
         conversation_event_matches_routes(
             event, conversation->stream_id, conversation->response_stream_id,
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

static h2_pal_result_t conversation_request_dispatch_event(void *user) {
  h2_gizclaw_conversation_request_t *request = user;
  return request->on_event(request->user, request, &request->dispatch_event);
}

static void
conversation_request_close(h2_gizclaw_conversation_request_t *request) {
  if (request->conversation == NULL)
    return;
  h2_gizclaw_conversation_deinit(request->conversation);
  request->conversation = NULL;
}

static h2_pal_result_t
conversation_request_poll(void *user, h2_gizclaw_client_t *client,
                          const h2_gizclaw_cancel_token_t *cancel_token) {
  (void)client;
  h2_gizclaw_conversation_request_t *request = user;
  if (h2_gizclaw_cancel_requested(cancel_token)) {
    conversation_request_close(request);
    return H2_PAL_ERR_CLOSED;
  }
  if (!request->has_pending_message) {
    const int queue_rc =
        h2_pal_queue_recv(request->service->config.queue, request->queue,
                          &request->pending_message, H2_PAL_QUEUE_NO_WAIT);
    if (queue_rc == H2_PAL_QUEUE_OK) {
      request->has_pending_message = true;
    } else if (queue_rc != H2_PAL_QUEUE_ERR_TIMEOUT &&
               queue_rc != H2_PAL_ERR_WOULD_BLOCK) {
      conversation_request_close(request);
      return queue_rc == H2_PAL_QUEUE_ERR_CLOSED ? H2_PAL_ERR_CLOSED
                                                 : H2_PAL_ERR_IO;
    }
  }
  if (request->has_pending_message) {
    h2_pal_result_t rc;
    if (request->pending_message.len == 0u) {
      rc = h2_gizclaw_conversation_commit(
          request->conversation, request->pending_message.timestamp_ms);
    } else {
      rc = h2_gizclaw_conversation_write_opus(
          request->conversation, request->pending_message.opus,
          request->pending_message.len, request->pending_message.timestamp_ms);
    }
    if (rc == H2_PAL_ERR_WOULD_BLOCK)
      return rc;
    if (rc != H2_PAL_OK) {
      conversation_request_close(request);
      return rc;
    }
    request->has_pending_message = false;
    memset(&request->pending_message, 0, sizeof(request->pending_message));
  }

  h2_gizclaw_conversation_event_t event = {0};
  h2_pal_result_t rc =
      h2_gizclaw_conversation_poll(request->conversation, 0, &event);
  if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK)
    return H2_PAL_ERR_WOULD_BLOCK;
  if (rc != H2_PAL_OK) {
    conversation_request_close(request);
    return rc;
  }
  if (event.kind == H2_GIZCLAW_CONVERSATION_EVENT_NONE)
    return H2_PAL_ERR_WOULD_BLOCK;
  request->dispatch_event = event;
  rc = h2_gizclaw_operation_dispatch_call(
      cancel_token, conversation_request_dispatch_event, request);
  const bool terminal =
      event.kind == H2_GIZCLAW_CONVERSATION_EVENT_REPLY_DONE ||
      event.kind == H2_GIZCLAW_CONVERSATION_EVENT_ERROR;
  if (rc != H2_PAL_OK || terminal)
    conversation_request_close(request);
  if (rc != H2_PAL_OK)
    return rc;
  return terminal ? H2_PAL_OK : H2_PAL_ERR_WOULD_BLOCK;
}

static h2_pal_result_t
conversation_request_start(void *user, h2_gizclaw_client_t *client,
                           const h2_gizclaw_cancel_token_t *cancel_token) {
  h2_gizclaw_conversation_request_t *request = user;
  if (h2_gizclaw_cancel_requested(cancel_token))
    return H2_PAL_ERR_CLOSED;
  h2_pal_result_t rc = h2_gizclaw_conversation_open(
      client,
      (h2_gizclaw_str_t){.data = request->workspace_name,
                         .len = request->workspace_name_len},
      request->generation, request->timeout_ms, &request->conversation);
  if (rc != H2_PAL_OK)
    return rc;
  return conversation_request_poll(user, client, cancel_token);
}

static void
conversation_request_complete(void *user, h2_gizclaw_operation_t *operation,
                              const h2_gizclaw_operation_result_t *result) {
  (void)operation;
  h2_gizclaw_conversation_request_t *request = user;
  request->operation_result = *result;
  atomic_store_explicit(&request->terminal, true, memory_order_release);
  request->completion(request->user, request);
}

h2_pal_result_t h2_gizclaw_service_conversation_create(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t workspace_name, uint64_t generation, int timeout_ms,
    h2_gizclaw_conversation_request_event_fn on_event,
    h2_gizclaw_conversation_request_completion_fn completion, void *user,
    h2_gizclaw_conversation_request_t **out_request) {
  if (service == NULL || !valid_workspace(workspace_name) || timeout_ms <= 0 ||
      on_event == NULL || completion == NULL || out_request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out_request = NULL;
  const h2_pal_mem_api_t *allocator = service->config.client_config->allocator;
  h2_gizclaw_conversation_request_t *request =
      h2_pal_mem_alloc(allocator, sizeof(*request));
  if (request == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(request, 0, sizeof(*request));
  request->service = service;
  request->on_event = on_event;
  request->completion = completion;
  request->user = user;
  request->generation = generation;
  request->timeout_ms = timeout_ms;
  request->workspace_name_len = workspace_name.len;
  memcpy(request->workspace_name, workspace_name.data, workspace_name.len);
  request->workspace_name[workspace_name.len] = '\0';
  const h2_pal_queue_config_t queue_config = {
      .name = "$gizclaw/conversation-request",
      .item_size = sizeof(h2_gizclaw_conversation_request_message_t),
      .item_count = H2_GIZCLAW_CONVERSATION_REQUEST_QUEUE_ITEMS,
      .allocator = allocator,
  };
  h2_pal_result_t rc = (h2_pal_result_t)h2_pal_queue_create(
      service->config.queue, &queue_config, &request->queue);
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_service_submit_async_internal(
        service, identity, conversation_request_start,
        conversation_request_poll, conversation_request_complete, request,
        &request->operation);
  }
  if (rc != H2_PAL_OK) {
    if (request->queue != NULL)
      h2_pal_queue_destroy(service->config.queue, request->queue);
    h2_pal_mem_free(allocator, request);
    return rc;
  }
  *out_request = request;
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_conversation_request_write_opus(
    h2_gizclaw_conversation_request_t *request, const uint8_t *opus,
    size_t opus_len, uint64_t timestamp_ms) {
  if (request == NULL || opus == NULL || opus_len == 0u ||
      opus_len > H2_GIZCLAW_CONVERSATION_OPUS_MAX_BYTES)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc =
      h2_pal_mutex_lock(request->service->config.sync, request->service->mutex);
  if (rc != H2_PAL_OK)
    return rc;
  if (atomic_load_explicit(&request->committed, memory_order_acquire) ||
      atomic_load_explicit(&request->terminal, memory_order_acquire)) {
    rc = H2_PAL_ERR_CLOSED;
  } else {
    h2_gizclaw_conversation_request_message_t message = {
        .len = opus_len, .timestamp_ms = timestamp_ms};
    memcpy(message.opus, opus, opus_len);
    rc = (h2_pal_result_t)h2_pal_queue_send(request->service->config.queue,
                                            request->queue, &message,
                                            H2_PAL_QUEUE_NO_WAIT);
  }
  (void)h2_pal_mutex_unlock(request->service->config.sync,
                            request->service->mutex);
  return rc;
}

h2_pal_result_t h2_gizclaw_conversation_request_commit(
    h2_gizclaw_conversation_request_t *request, uint64_t timestamp_ms) {
  if (request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc =
      h2_pal_mutex_lock(request->service->config.sync, request->service->mutex);
  if (rc != H2_PAL_OK)
    return rc;
  if (atomic_load_explicit(&request->committed, memory_order_acquire) ||
      atomic_load_explicit(&request->terminal, memory_order_acquire)) {
    rc = H2_PAL_ERR_CLOSED;
  } else {
    const h2_gizclaw_conversation_request_message_t message = {
        .timestamp_ms = timestamp_ms};
    rc = (h2_pal_result_t)h2_pal_queue_send(request->service->config.queue,
                                            request->queue, &message,
                                            H2_PAL_QUEUE_NO_WAIT);
    if (rc == H2_PAL_OK)
      atomic_store_explicit(&request->committed, true, memory_order_release);
  }
  (void)h2_pal_mutex_unlock(request->service->config.sync,
                            request->service->mutex);
  return rc;
}

h2_pal_result_t h2_gizclaw_conversation_request_cancel(
    h2_gizclaw_conversation_request_t *request) {
  if (request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  return h2_gizclaw_operation_cancel(request->operation);
}

h2_pal_result_t h2_gizclaw_conversation_request_wait(
    h2_gizclaw_conversation_request_t *request, uint32_t timeout_ms) {
  return request == NULL ? H2_PAL_ERR_INVALID_ARG
                         : h2_gizclaw_operation_wait(request->operation,
                                                     timeout_ms);
}

const h2_gizclaw_operation_result_t *
h2_gizclaw_conversation_request_operation_result(
    const h2_gizclaw_conversation_request_t *request) {
  if (request == NULL ||
      !atomic_load_explicit(&request->terminal, memory_order_acquire))
    return NULL;
  return &request->operation_result;
}

void h2_gizclaw_conversation_request_release(
    h2_gizclaw_conversation_request_t *request) {
  if (request == NULL ||
      !atomic_load_explicit(&request->terminal, memory_order_acquire))
    return;
  h2_gizclaw_operation_release(request->operation);
  h2_pal_queue_destroy(request->service->config.queue, request->queue);
  h2_pal_mem_free(request->service->config.client_config->allocator, request);
}
