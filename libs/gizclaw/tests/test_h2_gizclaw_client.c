#include "h2_gizclaw_client.h"
#include "h2_gizclaw_conversation.h"
#include "h2_gizclaw_internal.h"
#include "h2_gizclaw_pet.h"
#include "h2_gizclaw_points.h"
#include "h2_gizclaw_profile.h"
#include "h2_gizclaw_profile_internal.h"
#include "h2_gizclaw_registration.h"
#include "h2_gizclaw_registration_internal.h"
#include "h2_gizclaw_social.h"
#include "h2_gizclaw_speech.h"
#include "h2_gizclaw_telemetry.h"
#include "h2_gizclaw_workspace.h"

#include "gzc_common.h"
#include "payload/gameplay.pb.h"
#include "payload/social.pb.h"
#include "payload/system.pb.h"
#include "payload/workspace.pb.h"
#include "pb_encode.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(H2_GIZCLAW_TESTING)
int h2_gizclaw_test_try_write_bytes(h2_gizclaw_client_t *client,
                                    h2_pal_webrtc_channel_t *channel,
                                    const uint8_t *data, size_t len,
                                    size_t *offset, bool *blocked);
#endif

static int test_send_calls;
static int test_poll_calls;
static int test_last_poll_timeout_ms;
static int test_sleep_calls;
static uint32_t test_last_sleep_ms;
static h2_pal_result_t test_sleep_result = H2_PAL_OK;
static int test_warn_logs;
static char test_last_log_message[H2_PAL_LOG_MESSAGE_MAX];
static int test_send_would_block_count = 1;
static h2_pal_result_t test_send_result = H2_PAL_OK;
static h2_pal_result_t test_poll_result = H2_PAL_OK;
static h2_pal_result_t test_poll_event_error = H2_PAL_OK;
static uint64_t test_monotonic_ms = 1234u;
static bool test_cancel_requested;
static int test_opus_send_calls;
static h2_pal_result_t test_opus_send_result;
static uint8_t test_opus_bytes[H2_PAL_WEBRTC_OPUS_MAX_PACKET_SIZE];
static size_t test_opus_len;
static int test_opus_receive_calls;
static int test_peer_close_calls;

int h2_gizclaw_home_tests(void);
static int expect(int condition, const char *message);

typedef struct test_event_stream {
  gzc_event_stream_t *stream;
  size_t send_count;
  size_t read_count;
  size_t close_count;
  size_t fail_on_send_count;
  int send_result;
  int read_result;
  gzc_peer_event_t read_event;
  h2_gizclaw_conversation_t *conversation;
  bool conversation_invalidated_when_closed;
} test_event_stream_t;

typedef struct test_client_poll {
  int result;
  int calls;
  int timeout_ms;
  gzc_client_t *client;
} test_client_poll_t;

static void *test_alloc(void *user, size_t len) {
  (void)user;
  return malloc(len);
}
static void *test_realloc(void *user, void *ptr, size_t len) {
  (void)user;
  return realloc(ptr, len);
}

static void test_free(void *user, void *ptr) {
  (void)user;
  free(ptr);
}

static bool test_encode_registration_response(uint8_t *buffer, size_t capacity,
                                              size_t *out_len) {
  gizclaw_rpc_v1_ServerRegisterResponse response =
      gizclaw_rpc_v1_ServerRegisterResponse_init_zero;
  (void)snprintf(response.runtime_profile_name,
                 sizeof(response.runtime_profile_name), "%s", "demo-test");
  pb_ostream_t stream = pb_ostream_from_buffer(buffer, capacity);
  if (!pb_encode(&stream, gizclaw_rpc_v1_ServerRegisterResponse_fields,
                 &response)) {
    return false;
  }
  *out_len = stream.bytes_written;
  return true;
}

static int test_event_send(void *user, gzc_event_stream_t *stream,
                           const gzc_peer_event_t *event) {
  test_event_stream_t *test = user;
  if (test == NULL || stream != test->stream || event == NULL)
    return GZC_ERR_INVALID_ARGUMENT;
  ++test->send_count;
  if (test->fail_on_send_count != 0u &&
      test->send_count >= test->fail_on_send_count)
    return test->send_result;
  return GZC_OK;
}

static int test_event_read(void *user, gzc_event_stream_t *stream,
                           int timeout_ms, gzc_peer_event_t *out_event) {
  test_event_stream_t *test = user;
  (void)timeout_ms;
  if (test == NULL || stream != test->stream || out_event == NULL)
    return GZC_ERR_INVALID_ARGUMENT;
  ++test->read_count;
  if (test->read_result == GZC_OK)
    *out_event = test->read_event;
  return test->read_result;
}

typedef struct test_workspace_history_notification {
  char workspace_name[64];
  int64_t last_updated_at_unix_ms;
  size_t calls;
} test_workspace_history_notification_t;

static void
test_workspace_history_notification(void *user,
                                    const h2_gizclaw_client_event_t *event) {
  test_workspace_history_notification_t *notification = user;
  assert(notification != NULL && event != NULL);
  assert(event->kind == H2_GIZCLAW_CLIENT_EVENT_WORKSPACE_HISTORY_UPDATED);
  assert(event->workspace_name.len < sizeof(notification->workspace_name));
  memcpy(notification->workspace_name, event->workspace_name.data,
         event->workspace_name.len);
  notification->workspace_name[event->workspace_name.len] = '\0';
  notification->last_updated_at_unix_ms = event->last_updated_at_unix_ms;
  ++notification->calls;
}

static int test_packet_read(void *user, gzc_client_t *client, int timeout_ms,
                            uint8_t *out_protocol, gzc_buf_t *out_payload) {
  (void)user;
  (void)client;
  (void)timeout_ms;
  (void)out_protocol;
  (void)out_payload;
  return GZC_ERR_WOULD_BLOCK;
}

static void test_event_close(void *user, gzc_event_stream_t *stream) {
  test_event_stream_t *test = user;
  assert(test != NULL);
  assert(stream == test->stream);
  if (test->conversation != NULL)
    test->conversation_invalidated_when_closed =
        !h2_gizclaw_conversation_wire_input_ready_internal(test->conversation);
  ++test->close_count;
}

static int test_client_poll_call(void *user, gzc_client_t *client,
                                 int timeout_ms) {
  test_client_poll_t *test = user;
  assert(test != NULL);
  ++test->calls;
  test->client = client;
  test->timeout_ms = timeout_ms;
  return test->result;
}

typedef struct test_event_sink {
  h2_pal_result_t result;
  unsigned calls, accepted;
  const char *expected_name;
  int64_t expected_timestamp;
} test_event_sink_t;

static h2_pal_result_t test_event_sink(void *user,
                                       const h2_gizclaw_client_event_t *event) {
  test_event_sink_t *sink = user;
  assert(event->workspace_name.len == strlen(sink->expected_name));
  assert(memcmp(event->workspace_name.data, sink->expected_name,
                event->workspace_name.len) == 0);
  assert(event->last_updated_at_unix_ms == sink->expected_timestamp);
  ++sink->calls;
  if (sink->result == H2_PAL_OK)
    ++sink->accepted;
  return sink->result;
}

static int test_client_event_backpressure(const h2_gizclaw_config_t *config) {
  h2_gizclaw_client_t *client = NULL;
  assert(h2_gizclaw_client_init(config, &client) == H2_PAL_OK);
  test_event_stream_t event = {.stream = (gzc_event_stream_t *)(uintptr_t)0x39u,
                               .read_result = GZC_OK};
  test_client_poll_t poll = {.result = GZC_ERR_WOULD_BLOCK};
  test_event_sink_t sink = {.result = H2_PAL_ERR_WOULD_BLOCK,
                            .expected_name = "first",
                            .expected_timestamp = 1};
  event.read_event.type =
      gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_WORKSPACE_HISTORY_UPDATED;
  snprintf(
      event.read_event.payload.workspace_history_updated.workspace_name,
      sizeof(event.read_event.payload.workspace_history_updated.workspace_name),
      "first");
  event.read_event.payload.workspace_history_updated.last_updated_at_unix_ms =
      1;
  h2_gizclaw_test_set_event_ops(NULL, test_event_read, test_event_close,
                                &event);
  h2_gizclaw_test_set_client_poll(test_client_poll_call, &poll);
  assert(h2_gizclaw_test_replace_event_stream(client, event.stream) == NULL);
  assert(h2_gizclaw_client_set_event_handler(client, test_event_sink, &sink) ==
         H2_PAL_OK);
  assert(h2_gizclaw_client_poll(client, 0) == H2_PAL_ERR_WOULD_BLOCK);
  snprintf(
      event.read_event.payload.workspace_history_updated.workspace_name,
      sizeof(event.read_event.payload.workspace_history_updated.workspace_name),
      "second");
  event.read_event.payload.workspace_history_updated.last_updated_at_unix_ms =
      2;
  for (unsigned i = 0; i < 3; ++i) {
    sink.result = i % 2 ? H2_PAL_ERR_TIMEOUT : H2_PAL_ERR_WOULD_BLOCK;
    assert(h2_gizclaw_client_poll(client, 0) == H2_PAL_ERR_WOULD_BLOCK);
  }
  assert(event.read_count == 1 && poll.calls == 4 && sink.calls == 4 &&
         sink.accepted == 0);
  sink.result = H2_PAL_OK;
  assert(h2_gizclaw_client_poll(client, 0) == H2_PAL_ERR_WOULD_BLOCK);
  assert(event.read_count == 1 && sink.accepted == 1);
  sink.expected_name = "second";
  sink.expected_timestamp = 2;
  assert(h2_gizclaw_client_poll(client, 0) == H2_PAL_ERR_WOULD_BLOCK);
  assert(event.read_count == 2 && sink.accepted == 2 && poll.calls == 6);
  sink.result = H2_PAL_ERR_IO;
  assert(h2_gizclaw_client_poll(client, 0) == H2_PAL_ERR_IO);
  assert(event.close_count == 1 && poll.calls == 6);
  h2_gizclaw_client_deinit(client);
  assert(event.close_count == 1);
  h2_gizclaw_test_set_event_ops(NULL, NULL, NULL, NULL);
  h2_gizclaw_test_set_client_poll(NULL, NULL);
  return 0;
}

static int test_closed_poll_mapping(const h2_gizclaw_config_t *config) {
  int fails = 0;
  h2_gizclaw_client_t *client = NULL;
  fails += expect(h2_gizclaw_client_init(config, &client) == H2_PAL_OK &&
                      client != NULL,
                  "closed-poll mapping test initializes an independent client");
  if (client == NULL)
    return fails;

  test_event_stream_t event = {
      .stream = (gzc_event_stream_t *)(uintptr_t)0x38u,
  };
  test_client_poll_t poll = {
      .result = GZC_ERR_WOULD_BLOCK,
  };
  h2_gizclaw_test_set_event_ops(test_event_send, NULL, test_event_close,
                                &event);
  h2_gizclaw_test_set_client_poll(test_client_poll_call, &poll);
  fails +=
      expect(h2_gizclaw_test_replace_event_stream(client, event.stream) == NULL,
             "closed-poll mapping test installs the client Event handle");
  const h2_gizclaw_str_t workspace = {
      .data = "demo-poll",
      .len = 9u,
  };
  h2_gizclaw_conversation_t *conversation = NULL;
  fails +=
      expect(h2_gizclaw_conversation_wire_open_internal(
                 client, workspace, 11u, 1000, &conversation) == H2_PAL_OK &&
                 conversation != NULL && event.send_count == 1u,
             "closed-poll mapping test opens an active conversation");
  event.conversation = conversation;
  fails += expect(
      h2_gizclaw_client_poll(client, 17) == H2_PAL_ERR_WOULD_BLOCK &&
          poll.calls == 1 && poll.client != NULL && poll.timeout_ms == 17 &&
          event.close_count == 0u &&
          h2_gizclaw_conversation_wire_input_ready_internal(conversation) &&
          !h2_gizclaw_test_client_terminal_closed(client),
      "would-block poll preserves the active client and conversation");
  poll.result = GZC_ERR_CLOSED;
  fails += expect(
      h2_gizclaw_client_poll(client, 37) == H2_PAL_ERR_CLOSED &&
          poll.calls == 2 && poll.client != NULL && poll.timeout_ms == 37 &&
          event.close_count == 1u &&
          event.conversation_invalidated_when_closed &&
          !h2_gizclaw_conversation_wire_input_ready_internal(conversation) &&
          h2_gizclaw_test_client_terminal_closed(client),
      "SDK closed poll invalidates the conversation before releasing Event");
  fails +=
      expect(h2_gizclaw_conversation_wire_poll_internal(
                 conversation, 0, &(h2_gizclaw_conversation_event_t){0}) ==
                 H2_PAL_ERR_CLOSED,
             "closed-poll mapping leaves the active conversation unusable");
  fails +=
      expect(h2_gizclaw_client_poll(client, 99) == H2_PAL_ERR_CLOSED &&
                 poll.calls == 2 && event.close_count == 1u,
             "terminal poll bypasses the SDK and does not release Event twice");
  h2_gizclaw_conversation_wire_destroy_internal(conversation);
  event.conversation = NULL;
  fails +=
      expect(event.send_count == 1u && event.close_count == 1u,
             "conversation deinit does not reuse the released Event handle");
  fails += expect(
      h2_gizclaw_client_close(client) == H2_PAL_OK && event.close_count == 1u,
      "conversation and client cleanup after SDK closed poll stay idempotent");
  h2_gizclaw_client_deinit(client);
  h2_gizclaw_test_set_client_poll(NULL, NULL);
  h2_gizclaw_test_set_event_ops(NULL, NULL, NULL, NULL);
  return fails;
}

static int
test_event_failures_poison_client(h2_gizclaw_client_t *client,
                                  const h2_gizclaw_config_t *config) {
  int fails = 0;
  test_event_stream_t test = {
      .stream = (gzc_event_stream_t *)(uintptr_t)0x40u,
      .read_result = GZC_OK,
  };
  test.read_event = (gzc_peer_event_t)gizclaw_events_v1_PeerEvent_init_zero;
  test.read_event.version = GZC_PEER_EVENT_VERSION;
  test.read_event.type =
      gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_WORKSPACE_HISTORY_UPDATED;
  test.read_event.which_payload =
      gizclaw_events_v1_PeerEvent_workspace_history_updated_tag;
  (void)snprintf(
      test.read_event.payload.workspace_history_updated.workspace_name,
      sizeof(test.read_event.payload.workspace_history_updated.workspace_name),
      "%s", "social-group-1");
  test.read_event.payload.workspace_history_updated.last_updated_at_unix_ms =
      1234;
  h2_gizclaw_test_set_event_ops(test_event_send, test_event_read,
                                test_event_close, &test);
  h2_gizclaw_test_set_packet_read(test_packet_read, NULL);
  fails +=
      expect(h2_gizclaw_test_replace_event_stream(client, test.stream) == NULL,
             "closed-poll test installs the client Event access handle");

  const h2_gizclaw_str_t workspace = {
      .data = "demo-chat",
      .len = 9u,
  };
  h2_gizclaw_conversation_t *conversation = NULL;
  fails +=
      expect(h2_gizclaw_conversation_wire_open_internal(
                 client, workspace, 9u, 1000, &conversation) == H2_PAL_OK &&
                 conversation != NULL && test.send_count == 1u,
             "closed-poll test opens an active logical conversation");
  test_workspace_history_notification_t notification = {0};
  fails +=
      expect(h2_gizclaw_client_dispatch_event(
                 client, 0, test_workspace_history_notification,
                 &notification) == H2_PAL_OK &&
                 notification.calls == 1u &&
                 strcmp(notification.workspace_name, "social-group-1") == 0 &&
                 notification.last_updated_at_unix_ms == 1234,
             "client routes workspace history updates independently of "
             "the active conversation");
  test.read_result = GZC_ERR_WOULD_BLOCK;
  fails += expect(
      h2_gizclaw_client_dispatch_event(client, 0, NULL, NULL) ==
              H2_PAL_ERR_WOULD_BLOCK &&
          test.read_count == 2u && test.close_count == 0u &&
          h2_gizclaw_conversation_wire_input_ready_internal(conversation) &&
          !h2_gizclaw_test_client_terminal_closed(client),
      "would-block client Event dispatch preserves the active "
      "conversation");
  test.read_result = GZC_ERR_RPC;
  fails += expect(
      h2_gizclaw_client_dispatch_event(client, 0, NULL, NULL) ==
              H2_PAL_ERR_IO &&
          test.read_count == 3u && test.close_count == 1u &&
          !h2_gizclaw_conversation_wire_input_ready_internal(conversation) &&
          h2_gizclaw_test_client_terminal_closed(client),
      "malformed client Event dispatch invalidates the conversation and "
      "poisons the client");
  fails += expect(
      h2_gizclaw_client_poll(client, 0) == H2_PAL_ERR_CLOSED,
      "client poll remains closed after the mandatory Event transport closes");
  fails += expect(
      h2_gizclaw_client_close(client) == H2_PAL_OK && test.close_count == 1u,
      "explicit close after closed poll does not release Event twice");
  h2_gizclaw_conversation_wire_destroy_internal(conversation);
  h2_gizclaw_test_set_packet_read(NULL, NULL);
  h2_gizclaw_test_set_event_ops(NULL, NULL, NULL, NULL);

  h2_gizclaw_client_t *send_client = NULL;
  fails += expect(h2_gizclaw_client_init(config, &send_client) == H2_PAL_OK &&
                      send_client != NULL,
                  "send-failure test initializes an independent client");
  test_event_stream_t send_test = {
      .stream = (gzc_event_stream_t *)(uintptr_t)0x50u,
      .fail_on_send_count = 2u,
      .send_result = GZC_ERR_RPC,
  };
  h2_gizclaw_test_set_event_ops(test_event_send, NULL, test_event_close,
                                &send_test);
  fails += expect(h2_gizclaw_test_replace_event_stream(
                      send_client, send_test.stream) == NULL,
                  "send-failure test installs the client Event access handle");
  h2_gizclaw_conversation_t *send_conversation = NULL;
  fails += expect(h2_gizclaw_conversation_wire_open_internal(
                      send_client, workspace, 10u, 1000, &send_conversation) ==
                          H2_PAL_OK &&
                      send_conversation != NULL && send_test.send_count == 1u,
                  "send-failure test opens an active logical conversation");
  fails += expect(
      h2_gizclaw_conversation_wire_finish_input_internal(
          send_conversation, 10u) == H2_PAL_ERR_IO &&
          send_test.send_count == 2u && send_test.close_count == 1u &&
          !h2_gizclaw_conversation_wire_input_ready_internal(
              send_conversation) &&
          h2_gizclaw_test_client_terminal_closed(send_client),
      "non-transient Event send failure invalidates the turn and client");
  fails += expect(h2_gizclaw_client_close(send_client) == H2_PAL_OK &&
                      send_test.close_count == 1u,
                  "send failure and explicit close release Event exactly once");
  h2_gizclaw_conversation_wire_destroy_internal(send_conversation);
  h2_gizclaw_client_deinit(send_client);
  h2_gizclaw_test_set_event_ops(NULL, NULL, NULL, NULL);
  return fails;
}

static gzc_peer_event_t test_reply_event(int type, const char *label,
                                         const char *id, const char *text,
                                         const char *error_code) {
  gzc_peer_event_t event =
      (gzc_peer_event_t)gizclaw_events_v1_PeerEvent_init_zero;
  event.version = GZC_PEER_EVENT_VERSION;
  event.type = type;
  if (type == gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_BOS) {
    event.which_payload = gizclaw_events_v1_PeerEvent_bos_tag;
    (void)snprintf(event.payload.bos.label, sizeof(event.payload.bos.label),
                   "%s", label);
    (void)snprintf(event.payload.bos.stream_id,
                   sizeof(event.payload.bos.stream_id), "%s", id);
    event.payload.bos.kind = gizclaw_events_v1_StreamKind_STREAM_KIND_TEXT;
  } else if (type ==
             gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_TEXT_DELTA) {
    event.which_payload = gizclaw_events_v1_PeerEvent_text_delta_tag;
    (void)snprintf(event.payload.text_delta.label,
                   sizeof(event.payload.text_delta.label), "%s", label);
    (void)snprintf(event.payload.text_delta.stream_id,
                   sizeof(event.payload.text_delta.stream_id), "%s", id);
    (void)snprintf(event.payload.text_delta.text,
                   sizeof(event.payload.text_delta.text), "%s", text);
  } else if (type ==
             gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_TEXT_DONE) {
    event.which_payload = gizclaw_events_v1_PeerEvent_text_done_tag;
    (void)snprintf(event.payload.text_done.label,
                   sizeof(event.payload.text_done.label), "%s", label);
    (void)snprintf(event.payload.text_done.stream_id,
                   sizeof(event.payload.text_done.stream_id), "%s", id);
    (void)snprintf(event.payload.text_done.text,
                   sizeof(event.payload.text_done.text), "%s", text);
  } else {
    event.which_payload = gizclaw_events_v1_PeerEvent_eos_tag;
    (void)snprintf(event.payload.eos.label, sizeof(event.payload.eos.label),
                   "%s", label);
    (void)snprintf(event.payload.eos.stream_id,
                   sizeof(event.payload.eos.stream_id), "%s", id);
    if (error_code != NULL) {
      event.payload.eos.has_error = true;
      (void)snprintf(event.payload.eos.error.code,
                     sizeof(event.payload.eos.error.code), "%s", error_code);
    }
  }
  return event;
}

/* Feeds one peer event through the client and returns the wire poll result;
 * `out` receives the projected conversation event. */
static h2_pal_result_t test_feed_reply_event(
    test_event_stream_t *stream, h2_gizclaw_conversation_t *conv,
    const gzc_peer_event_t *event, h2_gizclaw_conversation_event_t *out) {
  stream->read_event = *event;
  stream->read_result = GZC_OK;
  const size_t reads = stream->read_count;
  const h2_pal_result_t rc =
      h2_gizclaw_conversation_wire_poll_internal(conv, 0, out);
  stream->read_result = GZC_ERR_WOULD_BLOCK;
  assert(stream->read_count == reads + 1u);
  return rc;
}

/* A pending boundary is emitted without reading the wire. */
static h2_pal_result_t
test_drain_reply_event(test_event_stream_t *stream,
                       h2_gizclaw_conversation_t *conv,
                       h2_gizclaw_conversation_event_t *out) {
  stream->read_result = GZC_ERR_WOULD_BLOCK;
  const size_t reads = stream->read_count;
  const h2_pal_result_t rc =
      h2_gizclaw_conversation_wire_poll_internal(conv, 0, out);
  assert(stream->read_count ==
         reads + (rc == H2_PAL_ERR_WOULD_BLOCK ? 1u : 0u));
  return rc;
}

/* Server-side barge-in while the input is still open (realtime): the reply
 * the server cut short ends with REPLY_DONE, and the next reply's BOS, text
 * and EOS are all accepted. Once the input is committed (push-to-talk) the
 * interruption remains a conversation error. */
static int test_conversation_barge_in(const h2_gizclaw_config_t *config) {
  static const int BOS = gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_BOS;
  static const int DELTA =
      gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_TEXT_DELTA;
  static const int DONE =
      gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_TEXT_DONE;
  static const int EOS = gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_EOS;
  int fails = 0;
  const h2_gizclaw_str_t workspace = {.data = "demo-barge", .len = 10u};
  /* 0: BOS of the next reply arrives before the interrupted EOS (device log).
   * 1: the interrupted EOS arrives before the next BOS.
   * 2: committed input (push-to-talk) keeps the error semantics.
   * 3: an error other than STREAM_INTERRUPTED stays an error while open. */
  for (unsigned mode = 0; mode < 4; ++mode) {
    h2_gizclaw_client_t *client = NULL;
    fails += expect(h2_gizclaw_client_init(config, &client) == H2_PAL_OK &&
                        client != NULL,
                    "barge-in test initializes an independent client");
    if (client == NULL)
      return fails;
    test_event_stream_t stream = {
        .stream = (gzc_event_stream_t *)(uintptr_t)(0x60u + mode),
        .read_result = GZC_ERR_WOULD_BLOCK,
    };
    h2_gizclaw_test_set_event_ops(test_event_send, test_event_read,
                                  test_event_close, &stream);
    h2_gizclaw_test_set_packet_read(test_packet_read, NULL);
    fails += expect(
        h2_gizclaw_test_replace_event_stream(client, stream.stream) == NULL,
        "barge-in test installs the client Event handle");
    h2_gizclaw_conversation_t *conv = NULL;
    fails +=
        expect(h2_gizclaw_conversation_wire_open_internal(
                   client, workspace, 20u + mode, 1000, &conv) == H2_PAL_OK &&
                   conv != NULL && stream.send_count == 1u,
               "barge-in test opens an active conversation");
    if (mode == 2)
      fails += expect(h2_gizclaw_conversation_wire_finish_input_internal(
                          conv, 5u) == H2_PAL_OK &&
                          stream.send_count == 2u,
                      "push-to-talk commits the input before the reply");
    h2_gizclaw_conversation_event_t out = {0};
    gzc_peer_event_t event =
        test_reply_event(BOS, "assistant", "reply-1", "", NULL);
    fails += expect(test_feed_reply_event(&stream, conv, &event, &out) ==
                            H2_PAL_OK &&
                        out.kind == H2_GIZCLAW_CONVERSATION_EVENT_NONE,
                    "first reply BOS is accepted");
    event = test_reply_event(DELTA, "assistant", "reply-1", "hel", NULL);
    fails += expect(test_feed_reply_event(&stream, conv, &event, &out) ==
                            H2_PAL_OK &&
                        out.kind == H2_GIZCLAW_CONVERSATION_EVENT_TEXT_DELTA &&
                        out.text_len == 3u,
                    "first reply text delta is delivered");
    event = test_reply_event(DONE, "assistant", "reply-1", "hello", NULL);
    fails += expect(test_feed_reply_event(&stream, conv, &event, &out) ==
                            H2_PAL_OK &&
                        out.kind == H2_GIZCLAW_CONVERSATION_EVENT_TEXT_DONE &&
                        out.text_len == 5u,
                    "first reply text done is delivered without a boundary");
    fails += expect(test_drain_reply_event(&stream, conv, &out) ==
                        H2_PAL_ERR_WOULD_BLOCK,
                    "an open reply has no pending boundary");
    if (mode != 2) {
      event = test_reply_event(BOS, "transcript", "heard-2", "", NULL);
      fails += expect(test_feed_reply_event(&stream, conv, &event, &out) ==
                              H2_PAL_OK &&
                          out.kind == H2_GIZCLAW_CONVERSATION_EVENT_NONE,
                      "the next user turn's transcript BOS is accepted");
      event = test_reply_event(DONE, "transcript", "heard-2", "again", NULL);
      fails += expect(test_feed_reply_event(&stream, conv, &event, &out) ==
                              H2_PAL_OK &&
                          out.kind == H2_GIZCLAW_CONVERSATION_EVENT_TEXT_DONE,
                      "the next user turn's transcript is delivered");
    }
    const char *code = mode == 3 ? "MODEL_FAILED" : "STREAM_INTERRUPTED";
    if (mode == 2) {
      /* Push-to-talk: no next reply can follow committed input, so a new-id
       * BOS while the reply is still open is rejected as before. */
      event = test_reply_event(BOS, "assistant", "reply-2", "", NULL);
      fails += expect(test_feed_reply_event(&stream, conv, &event, &out) ==
                          H2_PAL_ERR_WOULD_BLOCK,
                      "a new-id BOS on an open committed reply is rejected");
    }
    if (mode == 0) {
      event = test_reply_event(BOS, "assistant", "reply-2", "", NULL);
      fails += expect(test_feed_reply_event(&stream, conv, &event, &out) ==
                              H2_PAL_OK &&
                          out.kind == H2_GIZCLAW_CONVERSATION_EVENT_NONE,
                      "the next reply's BOS supersedes the open reply");
      fails += expect(
          test_drain_reply_event(&stream, conv, &out) == H2_PAL_OK &&
              out.kind == H2_GIZCLAW_CONVERSATION_EVENT_REPLY_DONE &&
              out.generation == 20u &&
              h2_gizclaw_conversation_wire_take_reply_interrupted_internal(
                  conv) &&
              !h2_gizclaw_conversation_wire_take_reply_interrupted_internal(
                  conv),
          "the superseded reply ends with an interrupted REPLY_DONE");
      event = test_reply_event(EOS, "assistant", "reply-1", "", code);
      fails += expect(test_feed_reply_event(&stream, conv, &event, &out) ==
                          H2_PAL_ERR_WOULD_BLOCK,
                      "the late interrupted EOS of the old reply is dropped");
    } else {
      event = test_reply_event(EOS, "assistant", "reply-1", "", code);
      const h2_pal_result_t rc =
          test_feed_reply_event(&stream, conv, &event, &out);
      fails += expect(rc == H2_PAL_OK &&
                          out.kind == H2_GIZCLAW_CONVERSATION_EVENT_NONE,
                      "the interrupted EOS is accepted on the open reply");
      fails += expect(test_drain_reply_event(&stream, conv, &out) == H2_PAL_OK,
                      "the interrupted EOS stages a boundary");
      if (mode == 1) {
        fails += expect(
            out.kind == H2_GIZCLAW_CONVERSATION_EVENT_REPLY_DONE &&
                out.generation == 21u &&
                h2_gizclaw_conversation_wire_take_reply_interrupted_internal(
                    conv),
            "an interrupted reply while input is open is a REPLY_DONE");
      } else {
        fails += expect(
            out.kind == H2_GIZCLAW_CONVERSATION_EVENT_ERROR &&
                out.error_code != NULL && strcmp(out.error_code, code) == 0 &&
                !out.retryable &&
                !h2_gizclaw_conversation_wire_take_reply_interrupted_internal(
                    conv),
            mode == 2 ? "an interrupted reply after committed input stays an "
                        "error"
                      : "another EOS error while input is open stays an "
                        "error");
      }
      if (mode == 2) {
        event = test_reply_event(BOS, "assistant", "reply-2", "", NULL);
        fails += expect(test_feed_reply_event(&stream, conv, &event, &out) ==
                                H2_PAL_OK &&
                            out.kind == H2_GIZCLAW_CONVERSATION_EVENT_NONE,
                        "a later BOS after the ended reply is still routed");
      }
    }
    if (mode == 0 || mode == 1) {
      if (mode == 1) {
        event = test_reply_event(BOS, "assistant", "reply-2", "", NULL);
        fails += expect(test_feed_reply_event(&stream, conv, &event, &out) ==
                                H2_PAL_OK &&
                            out.kind == H2_GIZCLAW_CONVERSATION_EVENT_NONE,
                        "the next reply's BOS follows the interrupted EOS");
      }
      event = test_reply_event(DELTA, "assistant", "reply-2", "wor", NULL);
      fails += expect(test_feed_reply_event(&stream, conv, &event, &out) ==
                              H2_PAL_OK &&
                          out.kind == H2_GIZCLAW_CONVERSATION_EVENT_TEXT_DELTA,
                      "the next reply's text delta is delivered");
      event = test_reply_event(DONE, "assistant", "reply-2", "world", NULL);
      fails += expect(test_feed_reply_event(&stream, conv, &event, &out) ==
                              H2_PAL_OK &&
                          out.kind == H2_GIZCLAW_CONVERSATION_EVENT_TEXT_DONE,
                      "the next reply's text done is delivered");
      event = test_reply_event(EOS, "assistant", "reply-2", "", NULL);
      fails += expect(test_feed_reply_event(&stream, conv, &event, &out) ==
                              H2_PAL_OK &&
                          out.kind == H2_GIZCLAW_CONVERSATION_EVENT_NONE,
                      "the next reply's EOS is accepted");
      fails += expect(
          test_drain_reply_event(&stream, conv, &out) == H2_PAL_OK &&
              out.kind == H2_GIZCLAW_CONVERSATION_EVENT_REPLY_DONE &&
              !h2_gizclaw_conversation_wire_take_reply_interrupted_internal(
                  conv),
          "the next reply ends with a normal REPLY_DONE");
    }
    h2_gizclaw_conversation_wire_destroy_internal(conv);
    fails += expect(h2_gizclaw_client_close(client) == H2_PAL_OK,
                    "barge-in test closes the client");
    h2_gizclaw_client_deinit(client);
    h2_gizclaw_test_set_packet_read(NULL, NULL);
    h2_gizclaw_test_set_event_ops(NULL, NULL, NULL, NULL);
  }
  return fails;
}

static int test_conversation_event_lease(h2_gizclaw_client_t *client) {
  int fails = 0;
  h2_gizclaw_conversation_t *first =
      (h2_gizclaw_conversation_t *)(uintptr_t)0x10u;
  h2_gizclaw_conversation_t *second =
      (h2_gizclaw_conversation_t *)(uintptr_t)0x20u;
  gzc_event_stream_t *events = (gzc_event_stream_t *)(uintptr_t)0x30u;
  fails += expect(h2_gizclaw_test_replace_event_stream(client, events) == NULL,
                  "test installs one client-owned Event access handle");

  gzc_event_stream_t *borrowed_events = NULL;
  uint64_t stream_sequence = 0u;
  fails += expect(
      h2_gizclaw_client_conversation_acquire_internal(
          client, first, &borrowed_events, &stream_sequence) == H2_PAL_OK &&
          borrowed_events == events && stream_sequence == 1u &&
          h2_gizclaw_client_conversation_active_internal(client, first),
      "first conversation borrows the client Event handle");

  borrowed_events = events;
  stream_sequence = UINT64_MAX;
  fails += expect(h2_gizclaw_client_conversation_acquire_internal(
                      client, second, &borrowed_events, &stream_sequence) ==
                          H2_PAL_ERR_INVALID_STATE &&
                      borrowed_events == NULL && stream_sequence == 0u,
                  "second simultaneous conversation lease is rejected");

  h2_gizclaw_client_conversation_release_internal(client, second);
  fails += expect(h2_gizclaw_client_conversation_active_internal(client, first),
                  "an unrelated conversation cannot release the active lease");
  h2_gizclaw_client_conversation_release_internal(client, first);
  fails +=
      expect(!h2_gizclaw_client_conversation_active_internal(client, first),
             "the active conversation releases its logical lease");

  fails += expect(h2_gizclaw_client_conversation_acquire_internal(
                      client, second, &borrowed_events, &stream_sequence) ==
                          H2_PAL_OK &&
                      borrowed_events == events && stream_sequence == 2u,
                  "a later conversation receives a unique stream sequence");
  h2_gizclaw_client_conversation_release_internal(client, second);

  /* Route rotation and stale-event rejection exercise the actual wire owner
   * in test_conversation_public_audio_tasks, not a separate test-only matcher.
   */

  fails +=
      expect(h2_gizclaw_test_replace_event_stream(client, NULL) == events,
             "test removes the borrowed Event handle before client deinit");
  return fails;
}

static int test_stable_registration_token_reuse(h2_gizclaw_client_t *client) {
  (void)client;
  int fails = 0;
  const char token[] = "stable-product-token";
  const uint8_t request[] = {
      0x0a, 0x14, 's', 't', 'a', 'b', 'l', 'e', '-', 'p', 'r',
      'o',  'd',  'u', 'c', 't', '-', 't', 'o', 'k', 'e', 'n',
  };
  uint8_t response[128];
  size_t response_len = 0u;
  fails += expect(test_encode_registration_response(response, sizeof(response),
                                                    &response_len),
                  "registration response fixture encodes");
  for (size_t attempt = 0u; attempt < 2u; ++attempt) {
    uint8_t encoded[128];
    size_t encoded_len = 0u;
    h2_gizclaw_registration_result_t registration = {0};
    fails += expect(
        h2_gizclaw_registration_encode_request(token, encoded, sizeof(encoded),
                                               &encoded_len) == H2_PAL_OK &&
            encoded_len == sizeof(request) &&
            memcmp(encoded, request, sizeof(request)) == 0,
        "stable product token encodes for each connection snapshot");
    fails += expect(h2_gizclaw_registration_decode_response(
                        response, response_len, &registration) == H2_PAL_OK,
                    "registration response decodes");
    fails +=
        expect(strcmp(registration.runtime_profile_name, "demo-test") == 0,
               "repeated registration preserves the product binding response");
  }
  return fails;
}

typedef struct test_telemetry_send {
  int result;
  int calls;
  uint32_t sequence;
  int64_t observed_at_unix_ms;
  size_t observation_count;
  bool mapped;
} test_telemetry_send_t;

static int test_telemetry_send_call(void *user,
                                    const gzc_telemetry_frame_t *frame) {
  test_telemetry_send_t *mock = user;
  ++mock->calls;
  mock->sequence = frame->sequence;
  mock->observed_at_unix_ms = frame->observed_at_unix_ms;
  mock->observation_count = frame->observation_count;
  mock->mapped =
      frame->observation_count == 4u &&
      frame->observations[0].kind == GZC_TELEMETRY_OBSERVATION_BATTERY &&
      frame->observations[0].battery.has_percent &&
      frame->observations[0].battery.percent == 72.5 &&
      frame->observations[0].battery.has_charging &&
      frame->observations[0].battery.charging &&
      frame->observations[1].kind == GZC_TELEMETRY_OBSERVATION_NETWORK &&
      frame->observations[1].network.has_rat &&
      frame->observations[1].network.rat.len == 4u &&
      memcmp(frame->observations[1].network.rat.data, "wifi", 4u) == 0 &&
      frame->observations[1].network.has_connected &&
      frame->observations[1].network.connected &&
      frame->observations[2].kind == GZC_TELEMETRY_OBSERVATION_GNSS &&
      frame->observations[2].gnss.latitude == 31.2304 &&
      frame->observations[2].gnss.longitude == 121.4737 &&
      frame->observations[3].kind == GZC_TELEMETRY_OBSERVATION_SYSTEM &&
      frame->observations[3].system.has_hardware_version &&
      frame->observations[3].system.hardware_version.len == 13u &&
      memcmp(frame->observations[3].system.hardware_version.data,
             "esp32s3_dev_1", 13u) == 0;
  return mock->result;
}

static int test_telemetry_adapter(h2_gizclaw_client_t *client) {
  int fails = 0;
  h2_gizclaw_telemetry_observation_t observations[4] = {
      {
          .kind = H2_GIZCLAW_TELEMETRY_BATTERY,
          .value.battery =
              {
                  .has_percent = true,
                  .percent = 72.5,
                  .has_charging = true,
                  .charging = true,
              },
      },
      {
          .kind = H2_GIZCLAW_TELEMETRY_NETWORK,
          .value.network =
              {
                  .has_rat = true,
                  .rat = {.data = "wifi", .len = 4u},
                  .has_connected = true,
                  .connected = true,
              },
      },
      {
          .kind = H2_GIZCLAW_TELEMETRY_GNSS,
          .value.gnss =
              {
                  .latitude = 31.2304,
                  .longitude = 121.4737,
              },
      },
      {
          .kind = H2_GIZCLAW_TELEMETRY_SYSTEM,
          .value.system =
              {
                  .has_uptime_seconds = true,
                  .uptime_seconds = 12.5,
                  .has_hardware_version = true,
                  .hardware_version = {.data = "esp32s3_dev_1", .len = 13u},
              },
      },
  };
  const h2_gizclaw_telemetry_frame_t frame = {
      .sequence = 7u,
      .observed_at_unix_ms = 1785110400000ll,
      .observations = observations,
      .observation_count = 4u,
  };
  test_telemetry_send_t mock = {.result = GZC_OK};
  h2_gizclaw_test_set_telemetry_send(test_telemetry_send_call, &mock);
  fails += expect(h2_gizclaw_test_telemetry_send(client, &frame) == H2_PAL_OK,
                  "telemetry adapter submits a bounded frame");
  fails += expect(mock.calls == 1 && mock.sequence == 7u &&
                      mock.observed_at_unix_ms == 1785110400000ll &&
                      mock.observation_count == 4u && mock.mapped,
                  "telemetry adapter maps all four observation kinds");

  observations[0].value.battery.percent = NAN;
  fails += expect(h2_gizclaw_test_telemetry_send(client, &frame) ==
                          H2_PAL_ERR_INVALID_ARG &&
                      mock.calls == 1,
                  "telemetry rejects fabricated non-finite values before send");
  observations[0].value.battery.percent = 72.5;

  mock.result = GZC_ERR_TIMEOUT;
  fails += expect(h2_gizclaw_test_telemetry_send(client, &frame) ==
                          H2_PAL_ERR_TIMEOUT &&
                      mock.calls == 2,
                  "telemetry maps a bounded transport timeout");
  h2_gizclaw_test_set_telemetry_send(NULL, NULL);
  return fails;
}

static h2_pal_result_t test_get_monotonic_ms(void *user, uint64_t *out_ms) {
  (void)user;
  *out_ms = test_monotonic_ms;
  return H2_PAL_OK;
}

static h2_pal_result_t test_sleep_ms(void *user, uint32_t ms) {
  (void)user;
  ++test_sleep_calls;
  test_last_sleep_ms = ms;
  if (test_sleep_result == H2_PAL_OK) {
    test_monotonic_ms += ms;
  }
  return test_sleep_result;
}

static int test_log_write(void *user, h2_pal_log_level_t level,
                          const char *scope, const char *message) {
  (void)user;
  assert(strcmp(scope, "gizclaw") == 0);
  if (level == H2_PAL_LOG_WARN) {
    ++test_warn_logs;
    (void)snprintf(test_last_log_message, sizeof(test_last_log_message), "%s",
                   message);
  }
  return H2_PAL_OK;
}

static h2_pal_result_t test_get_monotonic_ms_unsupported(void *user,
                                                         uint64_t *out_ms) {
  (void)user;
  (void)out_ms;
  return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t test_channel_send(h2_pal_webrtc_channel_t *channel,
                                         const uint8_t *data, size_t len,
                                         int is_text) {
  (void)channel;
  (void)data;
  (void)len;
  (void)is_text;
  test_send_calls++;
  return test_send_calls <= test_send_would_block_count ? H2_PAL_ERR_WOULD_BLOCK
                                                        : test_send_result;
}

static h2_pal_result_t test_peer_poll(h2_pal_webrtc_peer_t *peer,
                                      int timeout_ms,
                                      h2_pal_webrtc_event_t *out_event) {
  (void)peer;
  test_poll_calls++;
  test_last_poll_timeout_ms = timeout_ms;
  if (test_poll_result != H2_PAL_ERR_WOULD_BLOCK) {
    test_monotonic_ms += timeout_ms > 0 ? (uint64_t)timeout_ms : 1u;
  }
  if (test_poll_result == H2_PAL_OK && out_event != NULL)
    *out_event = (h2_pal_webrtc_event_t){
        .kind = test_poll_event_error == H2_PAL_OK
                    ? H2_PAL_WEBRTC_EVENT_WRITABLE
                    : H2_PAL_WEBRTC_EVENT_ERROR,
        .peer = peer,
        .error = test_poll_event_error,
    };
  return test_poll_result;
}

static bool test_cancel(void *user) {
  (void)user;
  return test_cancel_requested;
}

static h2_pal_result_t test_peer_send_opus(h2_pal_webrtc_peer_t *peer,
                                           const uint8_t *opus,
                                           size_t opus_len) {
  assert(peer == (h2_pal_webrtc_peer_t *)0x2);
  assert(opus != NULL);
  assert(opus_len <= sizeof(test_opus_bytes));
  memcpy(test_opus_bytes, opus, opus_len);
  test_opus_len = opus_len;
  ++test_opus_send_calls;
  return test_opus_send_result;
}

static size_t test_media_track_bind_calls;
static h2_pal_webrtc_track_t *test_expected_track;

static h2_pal_result_t test_track_read(void *user, uint8_t *data,
                                       size_t capacity, size_t *out_len) {
  (void)user;
  (void)data;
  (void)capacity;
  *out_len = 0u;
  return H2_PAL_ERR_WOULD_BLOCK;
}

static h2_pal_result_t
test_webrtc_peer_create(void *user, h2_pal_webrtc_peer_t **out_peer) {
  (void)user;
  *out_peer = (h2_pal_webrtc_peer_t *)0x2;
  return H2_PAL_OK;
}

static h2_pal_result_t test_peer_set_track(h2_pal_webrtc_peer_t *peer,
                                                 h2_pal_webrtc_track_t *track) {
  assert(peer == (h2_pal_webrtc_peer_t *)0x2);
  assert(track == test_expected_track);
  ++test_media_track_bind_calls;
  return H2_PAL_OK;
}

static h2_pal_result_t test_peer_unset_track(h2_pal_webrtc_peer_t *peer,
                                             h2_pal_webrtc_track_t *track) {
  assert(peer == (h2_pal_webrtc_peer_t *)0x2);
  assert(track == test_expected_track);
  return H2_PAL_OK;
}

static void test_peer_close(h2_pal_webrtc_peer_t *peer) {
  assert(peer == (h2_pal_webrtc_peer_t *)0x2);
  ++test_peer_close_calls;
}

static void test_opus_receive(void *user, gzc_rtc_peer_t *peer,
                              const uint8_t *opus, size_t opus_len) {
  assert(user == (void *)0x3);
  assert(peer == (gzc_rtc_peer_t *)0x2);
  assert(opus_len == 3u);
  assert(memcmp(opus, "\xf8\xff\xfe", 3u) == 0);
  ++test_opus_receive_calls;
}

static int expect(int condition, const char *message) {
  if (condition) {
    return 0;
  }
  printf("FAIL %s\n", message);
  return 1;
}

typedef struct provider_completion_fixture {
  int calls;
  int result;
  int results[8];
} provider_completion_fixture_t;

static void provider_complete(void *user, int result) {
  provider_completion_fixture_t *fixture = user;
  fixture->results[fixture->calls++] = result;
  fixture->result = result;
}

static int completion_provider(void *user, h2_gizclaw_rpc_method_t method,
    h2_gizclaw_rpc_bytes_t request, h2_gizclaw_rpc_provider_response_t *out) {
  (void)request;
  assert(method == H2_GIZCLAW_RPC_CLIENT_DEVICE_REBOOT);
  out->on_complete = provider_complete;
  out->complete_user = user;
  return H2_PAL_OK;
}

static void test_provider_completions(h2_gizclaw_config_t config) {
  const int saved_send_calls = test_send_calls;
  const int saved_block_count = test_send_would_block_count;
  test_send_would_block_count = 0;
  provider_completion_fixture_t fixture = {0};
  config.rpc_provider = completion_provider;
  config.rpc_provider_user = &fixture;
  for (int scenario = 0; scenario < 6; ++scenario) {
    h2_gizclaw_client_t *client = NULL;
    assert(h2_gizclaw_client_init(&config, &client) == H2_PAL_OK);
    h2_pal_webrtc_channel_t *channel = (h2_pal_webrtc_channel_t *)0x121;
    fixture = (provider_completion_fixture_t){0};
    const int respond_result = scenario == 3 ? GZC_ERR_NO_MEMORY : GZC_OK;
    assert(h2_gizclaw_test_provider_response(client, channel, respond_result) == respond_result);
    assert(fixture.calls == 0);
    test_client_poll_t poll = {.result = GZC_OK};
    h2_gizclaw_test_set_client_poll(test_client_poll_call, &poll);
    if (scenario == 3) {
      assert(h2_gizclaw_client_poll(client, 0) == H2_PAL_OK);
      assert(fixture.calls == 1 && fixture.result != H2_PAL_OK);
    } else if (scenario == 5) {
      poll.result = GZC_ERR_CLOSED;
      assert(h2_gizclaw_client_poll(client, 0) == H2_PAL_ERR_CLOSED);
      assert(fixture.calls == 1 && fixture.result == H2_PAL_ERR_CLOSED);
    } else if (scenario == 4) {
      assert(h2_gizclaw_client_close(client) == H2_PAL_OK);
      assert(fixture.calls == 1 && fixture.result == H2_PAL_ERR_CLOSED);
    } else {
      /* Poll success alone is not completion while the response is pending. */
      assert(h2_gizclaw_client_poll(client, 0) == H2_PAL_OK);
      assert(h2_gizclaw_client_poll(client, 0) == H2_PAL_OK);
      assert(fixture.calls == 0);
      const int transient[] = {GZC_ERR_WOULD_BLOCK, GZC_ERR_TIMEOUT, GZC_OK};
      for (size_t i = 0u; i < sizeof(transient) / sizeof(transient[0]); ++i) {
        poll.result = transient[i];
        (void)h2_gizclaw_client_poll(client, 0);
        assert(fixture.calls == 0);
      }
      if (scenario == 0) {
        const uint8_t eos[] = {0, 0, 0, 0};
        assert(h2_gizclaw_test_provider_send(channel, eos, sizeof(eos)) == GZC_OK);
      }
      h2_gizclaw_test_provider_channel_close(client, channel, scenario == 1);
      assert(fixture.calls == 0);
      poll.result = scenario == 2 ? GZC_ERR_TIMEOUT : GZC_OK;
      (void)h2_gizclaw_client_poll(client, 0);
      assert(fixture.calls == 1);
      assert((fixture.result == H2_PAL_OK) == (scenario == 0));
    }
    h2_gizclaw_test_set_client_poll(NULL, NULL);
    h2_gizclaw_client_deinit(client);
    assert(fixture.calls == 1);
  }
  /* Successful A must survive B's timeout in the same SDK poll. Exercise
   * split headers/payloads and an EOS-shaped payload, plus blocked retries. */
  for (int failure = 0; failure < 3; ++failure) {
    h2_gizclaw_client_t *pair = NULL;
    provider_completion_fixture_t a = {0};
    config.rpc_provider_user = &a;
    assert(h2_gizclaw_client_init(&config, &pair) == H2_PAL_OK);
    h2_pal_webrtc_channel_t *ca = (h2_pal_webrtc_channel_t *)0x401;
    h2_pal_webrtc_channel_t *cb = (h2_pal_webrtc_channel_t *)0x402;
    assert(h2_gizclaw_test_provider_response(pair, ca, GZC_OK) == GZC_OK);
    const uint8_t response[] = {4, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    test_send_result = H2_PAL_ERR_WOULD_BLOCK;
    assert(h2_gizclaw_test_provider_send(ca, response, sizeof(response)) == GZC_ERR_WOULD_BLOCK);
    test_send_result = H2_PAL_OK;
    for (size_t i = 0; i < sizeof(response); ++i)
      assert(h2_gizclaw_test_provider_send(ca, response + i, 1) == GZC_OK);
    h2_gizclaw_test_provider_channel_close(pair, ca, false);
    assert(h2_gizclaw_test_provider_response(pair, cb, GZC_OK) == GZC_OK);
    if (failure == 1) {
      test_send_result = H2_PAL_ERR_IO;
      assert(h2_gizclaw_test_provider_send(cb, response, sizeof(response)) != GZC_OK);
      test_send_result = H2_PAL_OK;
    } else {
      /* No EOS: merely sending an all-zero payload cannot mean completion. */
      assert(h2_gizclaw_test_provider_send(cb, response, 8) == GZC_OK);
    }
    h2_gizclaw_test_provider_channel_close(pair, cb, false);
    test_client_poll_t poll = {.result = failure == 0 ? GZC_ERR_TIMEOUT : GZC_OK};
    h2_gizclaw_test_set_client_poll(test_client_poll_call, &poll);
    (void)h2_gizclaw_client_poll(pair, 0);
    assert(a.calls == 2);
    assert(a.result != H2_PAL_OK);
    assert(a.results[0] == H2_PAL_OK);
    assert(a.results[1] != H2_PAL_OK);
    h2_gizclaw_test_set_client_poll(NULL, NULL);
    h2_gizclaw_client_deinit(pair);
    assert(a.calls == 2);
  }
  config.rpc_provider_user = &fixture;
  h2_gizclaw_client_t *client = NULL;
  fixture = (provider_completion_fixture_t){0};
  assert(h2_gizclaw_client_init(&config, &client) == H2_PAL_OK);
  for (uintptr_t i = 0u; i < GZC_RPC_MAX_INBOUND_CHANNELS; ++i) {
    assert(h2_gizclaw_test_provider_response(client,
        (h2_pal_webrtc_channel_t *)(0x200u + i), GZC_OK) == GZC_OK);
  }
  assert(fixture.calls == 0);
  assert(h2_gizclaw_test_provider_response(client,
      (h2_pal_webrtc_channel_t *)0x300u, GZC_OK) == GZC_ERR_RPC);
  assert(fixture.calls == 1 && fixture.result == H2_PAL_ERR_INVALID_STATE);
  assert(h2_gizclaw_client_close(client) == H2_PAL_OK);
  assert(fixture.calls == 1 + GZC_RPC_MAX_INBOUND_CHANNELS);
  h2_gizclaw_client_deinit(client);
  assert(fixture.calls == 1 + GZC_RPC_MAX_INBOUND_CHANNELS);
  test_send_calls = saved_send_calls;
  test_send_would_block_count = saved_block_count;
}

int main(void) {
  int fails = 0;
  fails += expect(H2_GIZCLAW_RPC_SERVER_PET_LIST == 65,
                  "pet list wire method remains 65");
  fails += expect(H2_GIZCLAW_RPC_SERVER_PET_GET == 66,
                  "pet get wire method remains 66");
  fails += expect(H2_GIZCLAW_RPC_RUNTIME_ADOPT == 67,
                  "runtime adopt wire method remains 67");
  fails += expect(H2_GIZCLAW_RPC_SERVER_PET_PUT == 68,
                  "pet put wire method remains 68");
  fails += expect(H2_GIZCLAW_RPC_SERVER_PET_DELETE == 69,
                  "pet delete wire method remains 69");
  fails += expect(H2_GIZCLAW_RPC_SERVER_PET_DRIVE == 70,
                  "pet drive wire method remains 70");
  fails += expect(H2_GIZCLAW_RPC_SERVER_POINTS_GET == 71,
                  "points get wire method remains 71");
  fails += expect(H2_GIZCLAW_RPC_SERVER_POINTS_TRANSACTIONS_LIST == 72,
                  "points transaction list wire method remains 72");
  fails += expect(H2_GIZCLAW_RPC_SERVER_POINTS_TRANSACTIONS_GET == 73,
                  "points transaction get wire method remains 73");
  fails += expect(H2_GIZCLAW_RPC_CLIENT_TOOL_INVOKE == 82,
                  "tool invoke wire method remains 82");
  fails += expect(H2_GIZCLAW_RPC_SERVER_PET_ACTIONS_GET == 86,
                  "pet actions wire method remains 86");
  fails += expect(H2_GIZCLAW_RPC_SERVER_PET_PIXA_DOWNLOAD == 87,
                  "pet PIXA download wire method remains 87");
  fails += expect(H2_GIZCLAW_RPC_SERVER_FRIEND_INFO_GET == 89,
                  "friend info wire method remains 89");
  fails += expect(H2_GIZCLAW_RPC_SERVER_REGISTER == 90,
                  "register wire method remains 90");
  fails += expect(H2_GIZCLAW_RPC_SERVER_PEER_DELETE == 93,
                  "peer delete wire method remains 93");
  h2_gizclaw_config_t config;
  memset(&config, 0, sizeof(config));
  config.server_endpoint.data = "127.0.0.1:19820";
  config.server_endpoint.len = strlen(config.server_endpoint.data);
  config.private_key.data = "test-private-key";
  config.private_key.len = strlen(config.private_key.data);
  config.cipher_mode = H2_GIZCLAW_CIPHER_CHACHA20_POLY1305;
  config.connect_timeout_ms = 1000;

  const h2_pal_mem_vtable_t mem_vtable = {
      .alloc = test_alloc,
      .realloc = test_realloc,
      .free = test_free,
  };
  const h2_pal_mem_api_t mem = {
      .user = NULL,
      .vtable = &mem_vtable,
  };
  const h2_pal_http_api_t http = {0};
  const h2_pal_webrtc_vtable_t webrtc_vtable = {
      .peer_create = test_webrtc_peer_create,
      .peer_poll = test_peer_poll,
      .peer_set_track = test_peer_set_track,
      .peer_unset_track = test_peer_unset_track,
      .channel_send = test_channel_send,
      .peer_send_opus = test_peer_send_opus,
      .peer_close = test_peer_close,
  };
  const h2_pal_webrtc_api_t webrtc = {
      .user = NULL,
      .vtable = &webrtc_vtable,
  };
  const h2_pal_crypto_api_t crypto = {0};
  const h2_pal_time_vtable_t time_vtable = {
      .get_monotonic_ms = test_get_monotonic_ms,
      .sleep_ms = test_sleep_ms,
  };
  const h2_pal_time_api_t time = {
      .user = NULL,
      .vtable = &time_vtable,
  };
  config.allocator = &mem;
  config.http = &http;
  config.webrtc = &webrtc;
  config.crypto = &crypto;
  config.time = &time;
  const h2_pal_log_vtable_t log_vtable = {
      .write = test_log_write,
  };
  const h2_pal_log_api_t log = {
      .user = NULL,
      .vtable = &log_vtable,
  };
  config.log = &log;
  config.cancel_requested = test_cancel;
  test_provider_completions(config);

  h2_gizclaw_client_t *client = (h2_gizclaw_client_t *)0x1;
  fails +=
      expect(h2_gizclaw_client_init(NULL, &client) == H2_PAL_ERR_INVALID_ARG,
             "init rejects null config");
  fails += expect(client == (h2_gizclaw_client_t *)0x1,
                  "failed init leaves output untouched before validation");
  config.connect_timeout_ms = -1;
  fails +=
      expect(h2_gizclaw_client_init(&config, &client) == H2_PAL_ERR_INVALID_ARG,
             "init rejects an inherited negative service-write timeout");
  config.connect_timeout_ms = 1000;
  config.write_timeout_ms = -1;
  fails +=
      expect(h2_gizclaw_client_init(&config, &client) == H2_PAL_ERR_INVALID_ARG,
             "init rejects a negative service-write timeout");
  config.write_timeout_ms = 0;
  const h2_pal_time_api_t missing_monotonic_time = {0};
  config.time = &missing_monotonic_time;
  fails +=
      expect(h2_gizclaw_client_init(&config, &client) == H2_PAL_ERR_INVALID_ARG,
             "init rejects a time API without a monotonic clock");
  const h2_pal_time_vtable_t unsupported_time_vtable = {
      .get_monotonic_ms = test_get_monotonic_ms_unsupported,
  };
  const h2_pal_time_api_t unsupported_time = {
      .user = NULL,
      .vtable = &unsupported_time_vtable,
  };
  config.time = &unsupported_time;
  client = (h2_gizclaw_client_t *)0x1;
  fails +=
      expect(h2_gizclaw_client_init(&config, &client) == H2_PAL_ERR_UNSUPPORTED,
             "init rejects a monotonic clock that reports unsupported");
  fails +=
      expect(client == NULL, "failed monotonic preflight clears the output");
  config.time = &time;
  config.write_timeout_ms = 2000;
  client = NULL;
  fails += expect(h2_gizclaw_client_init(&config, &client) == H2_PAL_OK,
                  "init accepts the current SDK platform contract");
  fails += expect(client != NULL, "successful init returns a client");
  const h2_pal_webrtc_track_vtable_t test_track_vtable = {
      .read = test_track_read,
  };
  h2_pal_webrtc_track_t test_track = {
      .vtable = &test_track_vtable,
  };
  test_expected_track = &test_track;
  config.webrtc_media_track = &test_track;
  h2_gizclaw_client_t *track_client = NULL;
  fails += expect(h2_gizclaw_client_init(&config, &track_client) == H2_PAL_OK,
                  "track-mode init accepts an opaque provider track");
  h2_pal_webrtc_peer_t *track_peer = NULL;
  fails +=
      expect(h2_gizclaw_test_peer_create(track_client, &track_peer) == GZC_OK &&
                 track_peer == (h2_pal_webrtc_peer_t *)0x2 &&
                 test_media_track_bind_calls == 1u,
             "GizClaw binds the opaque track before offer setup");
  const struct {
    int pal;
    int gzc;
  } poll_results[] = {{H2_PAL_OK, GZC_OK},
                      {H2_PAL_ERR_TIMEOUT, GZC_OK},
                      {H2_PAL_ERR_WOULD_BLOCK, GZC_OK},
                      {H2_PAL_ERR_IO, GZC_ERR_WEBRTC},
                      {H2_PAL_ERR_CLOSED, GZC_ERR_CLOSED}};
  for (size_t i = 0u; i < sizeof(poll_results) / sizeof(*poll_results); ++i) {
    test_poll_result = poll_results[i].pal;
    for (int timeout = 0; timeout <= 10; timeout += 10) {
      const int before = test_poll_calls;
      fails += expect(
          h2_gizclaw_test_peer_poll(track_client, track_peer, timeout) ==
                  poll_results[i].gzc &&
              test_poll_calls == before + 1 &&
              test_last_poll_timeout_ms == timeout,
          "SDK peer poll treats idle waits as progress, not transport failure");
    }
  }
  test_poll_result = H2_PAL_OK;
  test_poll_event_error = H2_PAL_ERR_TIMEOUT;
  fails += expect(h2_gizclaw_test_peer_poll(track_client, track_peer, 10) ==
                      GZC_ERR_TIMEOUT,
                  "an explicit timeout ERROR event remains terminal");
  test_poll_event_error = H2_PAL_OK;
  test_poll_calls = test_warn_logs = 0;
  h2_gizclaw_client_deinit(track_client);
  test_peer_close_calls = 0u;
  config.webrtc_media_track = NULL;
  fails += expect(h2_gizclaw_test_media_registered(client),
                  "init pins the C SDK Opus media adapter");
  const uint8_t opus_payload[] = {0xf8u, 0xffu, 0xfeu};
  test_opus_send_result = H2_PAL_ERR_WOULD_BLOCK;
  fails += expect(
      h2_gizclaw_test_media_send_opus(client, (gzc_rtc_peer_t *)0x2,
                                      opus_payload, sizeof(opus_payload)) ==
              GZC_ERR_WOULD_BLOCK &&
          test_opus_send_calls == 1u && test_opus_len == sizeof(opus_payload) &&
          memcmp(test_opus_bytes, opus_payload, sizeof(opus_payload)) == 0,
      "Opus adapter preserves raw payload and backpressure");
  test_opus_send_result = H2_PAL_OK;
  fails += expect(h2_gizclaw_test_media_send_opus(
                      client, (gzc_rtc_peer_t *)0x2, opus_payload,
                      sizeof(opus_payload)) == GZC_OK &&
                      test_opus_send_calls == 2u,
                  "Opus adapter consumes the same frame after retry succeeds");
  fails +=
      expect(h2_gizclaw_test_media_send_opus(client, (gzc_rtc_peer_t *)0x2,
                                             opus_payload,
                                             0u) == GZC_ERR_INVALID_ARGUMENT &&
                 test_opus_send_calls == 2u,
             "Opus adapter rejects an empty frame before provider dispatch");
  fails += expect(
      h2_gizclaw_test_media_send_opus(client, (gzc_rtc_peer_t *)0x2,
                                      opus_payload,
                                      H2_PAL_WEBRTC_OPUS_MAX_PACKET_SIZE +
                                          1u) == GZC_ERR_INVALID_ARGUMENT &&
          test_opus_send_calls == 2u,
      "Opus adapter rejects an oversized frame before provider dispatch");
  uint8_t opus_max[H2_PAL_WEBRTC_OPUS_MAX_PACKET_SIZE];
  memset(opus_max, 0u, sizeof(opus_max));
  fails += expect(h2_gizclaw_test_media_send_opus(client, (gzc_rtc_peer_t *)0x2,
                                                  opus_max, 1u) == GZC_OK &&
                      test_opus_send_calls == 3u && test_opus_len == 1u,
                  "Opus adapter accepts the minimum complete packet");
  fails += expect(
      h2_gizclaw_test_media_send_opus(client, (gzc_rtc_peer_t *)0x2, opus_max,
                                      sizeof(opus_max)) == GZC_OK &&
          test_opus_send_calls == 4u && test_opus_len == sizeof(opus_max),
      "Opus adapter accepts the maximum complete packet");
  test_opus_send_result = H2_PAL_ERR_UNSUPPORTED;
  fails += expect(h2_gizclaw_test_media_send_opus(
                      client, (gzc_rtc_peer_t *)0x2, opus_payload,
                      sizeof(opus_payload)) == GZC_ERR_UNSUPPORTED &&
                      test_opus_send_calls == 5u,
                  "Opus adapter preserves explicit provider unsupported");
  test_opus_send_result = H2_PAL_OK;
  fails += expect(h2_gizclaw_test_media_set_opus_frame_callback(
                      client, (gzc_rtc_peer_t *)0x2, test_opus_receive,
                      (void *)0x3) == GZC_OK,
                  "Opus adapter accepts a receive callback");
  h2_gizclaw_test_media_emit_opus(client, (gzc_rtc_peer_t *)0x2, opus_payload,
                                  sizeof(opus_payload));
  h2_gizclaw_test_media_emit_opus(client, (gzc_rtc_peer_t *)0x4, opus_payload,
                                  sizeof(opus_payload));
  fails += expect(test_opus_receive_calls == 1u,
                  "Opus adapter forwards only the registered peer payload");
  fails += expect(h2_gizclaw_test_media_set_opus_frame_callback(
                      client, (gzc_rtc_peer_t *)0x2, NULL, NULL) == GZC_OK &&
                      h2_gizclaw_test_media_set_opus_frame_callback(
                          client, (gzc_rtc_peer_t *)0x2, NULL, NULL) == GZC_OK,
                  "Opus callback unregister is idempotent");
  h2_gizclaw_test_media_emit_opus(client, (gzc_rtc_peer_t *)0x2, opus_payload,
                                  sizeof(opus_payload));
  fails += expect(test_opus_receive_calls == 1u,
                  "Opus callback does not run after unregister");
  fails += expect(h2_gizclaw_test_media_set_opus_frame_callback(
                      client, (gzc_rtc_peer_t *)0x2, test_opus_receive,
                      (void *)0x3) == GZC_OK,
                  "Opus callback can be registered again");
  h2_gizclaw_test_media_close_peer(client, (gzc_rtc_peer_t *)0x2);
  h2_gizclaw_test_media_emit_opus(client, (gzc_rtc_peer_t *)0x2, opus_payload,
                                  sizeof(opus_payload));
  fails += expect(test_peer_close_calls == 1u && test_opus_receive_calls == 1u,
                  "peer close clears the Opus callback before provider close");
  const uint8_t payload[] = {1u, 2u, 3u};
  size_t offset = 0u;
  bool blocked = false;
  h2_pal_webrtc_channel_t *channel = (h2_pal_webrtc_channel_t *)(uintptr_t)1u;
  fails += expect(
      h2_gizclaw_test_try_write_bytes(client, channel, payload, sizeof(payload),
                                      &offset, &blocked) == GZC_OK &&
          offset == 0u && blocked && test_send_calls == 1 &&
          test_poll_calls == 0,
      "adapter exposes PAL backpressure without polling or retrying");
  test_send_calls = 0;
  test_poll_calls = 0;
  test_sleep_calls = 0;
  test_warn_logs = 0;
  test_send_would_block_count = 1;
  test_poll_result = H2_PAL_ERR_WOULD_BLOCK;
  offset = 0u;
  blocked = false;
  fails += expect(
      h2_gizclaw_test_try_write_bytes(client, channel, payload, sizeof(payload),
                                      &offset, &blocked) == GZC_OK &&
          offset == 0u && blocked && test_send_calls == 1 &&
          test_poll_calls == 0 && test_sleep_calls == 0 && test_warn_logs == 0,
      "adapter does not hide peer poll backoff behind channel send");
  test_send_calls = 0;
  test_poll_calls = 0;
  test_sleep_calls = 0;
  test_warn_logs = 0;
  test_send_would_block_count = 1;
  test_sleep_result = H2_PAL_ERR_UNSUPPORTED;
  offset = 0u;
  blocked = false;
  fails += expect(
      h2_gizclaw_test_try_write_bytes(client, channel, payload, sizeof(payload),
                                      &offset, &blocked) == GZC_OK &&
          offset == 0u && blocked && test_send_calls == 1 &&
          test_poll_calls == 0 && test_sleep_calls == 0 && test_warn_logs == 0,
      "channel backpressure does not depend on optional sleep support");
  test_sleep_result = H2_PAL_OK;
  test_poll_result = H2_PAL_OK;
  test_send_calls = 0;
  test_poll_calls = 0;
  test_send_would_block_count = 3000;
  offset = 0u;
  blocked = false;
  fails += expect(h2_gizclaw_test_try_write_bytes(client, channel, payload,
                                                  sizeof(payload), &offset,
                                                  &blocked) == GZC_OK &&
                      offset == 0u && blocked && test_send_calls == 1 &&
                      test_poll_calls == 0,
                  "adapter returns persistent PAL backpressure immediately");
  config.write_timeout_ms = 7;
  h2_gizclaw_client_t *short_timeout_client = NULL;
  fails += expect(h2_gizclaw_client_init(&config, &short_timeout_client) ==
                          H2_PAL_OK &&
                      short_timeout_client != NULL,
                  "client accepts a write timeout below the backoff cap");
  test_send_calls = 0;
  test_poll_calls = 0;
  test_sleep_calls = 0;
  test_warn_logs = 0;
  test_send_would_block_count = 2;
  test_poll_result = H2_PAL_ERR_WOULD_BLOCK;
  offset = 0u;
  blocked = false;
  fails += expect(
      h2_gizclaw_test_try_write_bytes(short_timeout_client, channel, payload,
                                      sizeof(payload), &offset,
                                      &blocked) == GZC_OK &&
          offset == 0u && blocked && test_send_calls == 1 &&
          test_poll_calls == 0 && test_sleep_calls == 0 && test_warn_logs == 0,
      "write timeout does not introduce a hidden send retry loop");
  h2_gizclaw_client_deinit(short_timeout_client);
  config.write_timeout_ms = 2000;
  test_poll_result = H2_PAL_OK;
  test_send_calls = 0;
  test_poll_calls = 0;
  test_cancel_requested = true;
  offset = 0u;
  blocked = false;
  fails += expect(
      h2_gizclaw_test_try_write_bytes(client, channel, payload, sizeof(payload),
                                      &offset, &blocked) == GZC_OK &&
          offset == 0u && blocked && test_send_calls == 1 &&
          test_poll_calls == 0,
      "send path leaves cancellation handling to the owning operation");
  test_cancel_requested = false;
  test_send_calls = 0;
  test_poll_calls = 0;
  test_poll_result = H2_PAL_ERR_CLOSED;
  offset = 0u;
  blocked = false;
  fails += expect(h2_gizclaw_test_try_write_bytes(client, channel, payload,
                                                  sizeof(payload), &offset,
                                                  &blocked) == GZC_OK &&
                      offset == 0u && blocked && test_send_calls == 1 &&
                      test_poll_calls == 0,
                  "send path does not consume unrelated terminal poll state");
  test_poll_result = H2_PAL_ERR_IO;
  test_send_calls = 0;
  test_poll_calls = 0;
  offset = 0u;
  blocked = false;
  fails += expect(
      h2_gizclaw_test_try_write_bytes(client, channel, payload, sizeof(payload),
                                      &offset, &blocked) == GZC_OK &&
          offset == 0u && blocked && test_send_calls == 1 &&
          test_poll_calls == 0,
      "send path leaves terminal poll failures to the event consumer");
  test_poll_result = H2_PAL_OK;
  test_send_would_block_count = 0;
  test_send_result = H2_PAL_ERR_IO;
  test_send_calls = 0;
  test_poll_calls = 0;
  offset = 0u;
  blocked = false;
  fails += expect(
      h2_gizclaw_test_try_write_bytes(client, channel, payload, sizeof(payload),
                                      &offset, &blocked) == GZC_ERR_WEBRTC &&
          offset == 0u && test_send_calls == 1 && test_poll_calls == 0,
      "adapter does not retry a terminal channel send failure");
  test_send_result = H2_PAL_OK;
  test_send_would_block_count = 0;
  fails += test_stable_registration_token_reuse(client);
  fails += test_telemetry_adapter(client);
  fails += test_conversation_event_lease(client);
  fails += test_closed_poll_mapping(&config);
  fails += test_client_event_backpressure(&config);
  fails += test_event_failures_poison_client(client, &config);
  fails += test_conversation_barge_in(&config);
  h2_gizclaw_client_deinit(client);
  fails += expect(h2_gizclaw_client_connect(NULL) == H2_PAL_ERR_INVALID_ARG,
                  "connect rejects null client");
  fails += expect(h2_gizclaw_client_poll(NULL, 0) == H2_PAL_ERR_INVALID_ARG,
                  "poll rejects null client");
  h2_gizclaw_rpc_response_t rpc_response = {0};
  h2_gizclaw_rpc_request_t *rpc_request = (h2_gizclaw_rpc_request_t *)1;
  fails +=
      expect(h2_gizclaw_client_rpc_request_start(
                 NULL, H2_GIZCLAW_RPC_ALL_PING, (h2_gizclaw_rpc_bytes_t){0},
                 1000u, &rpc_request) == H2_PAL_ERR_INVALID_ARG &&
                 rpc_request == NULL,
             "async rpc rejects null client and clears output");
  fails += expect(h2_gizclaw_rpc_request_result(NULL, &rpc_response) ==
                      H2_PAL_ERR_INVALID_ARG,
                  "async rpc result rejects null request");
  h2_gizclaw_rpc_request_cancel(NULL);
  h2_gizclaw_rpc_request_destroy(NULL);
  fails += expect(h2_gizclaw_client_close(NULL) == H2_PAL_ERR_INVALID_ARG,
                  "close rejects null client");
  h2_gizclaw_req_t *social_request = NULL;
  fails += expect(h2_gizclaw_req_create_friend_group_list(
                      NULL, 0u, (h2_gizclaw_str_t){0}, 8u, 1000u,
                      &social_request) == H2_PAL_ERR_INVALID_ARG,
                  "friend group list rejects null client");
  h2_gizclaw_profile_t profile = {0};
  h2_gizclaw_req_t *pet_request = NULL;
  const h2_gizclaw_pet_adopt_options_t adopt = {
      .name = {.data = "pet-test-1", .len = 10u},
      .display_name = {.data = "Test Pet", .len = 8u},
  };
  const h2_gizclaw_pet_drive_options_t empty_drive = {
      .pet_name = {.data = "pet-test-1", .len = 10u},
      .idempotency_key = {.data = "drive-test-1", .len = 12u},
  };
  fails += expect(h2_gizclaw_req_create_pet_get(NULL, 0u, empty_drive.pet_name,
                                                1000u, &pet_request) ==
                      H2_PAL_ERR_INVALID_ARG,
                  "pet get rejects null client");
  fails += expect(
      h2_gizclaw_req_create_pet_adopt(NULL, 0u, &adopt, 1000u, &pet_request) ==
          H2_PAL_ERR_INVALID_ARG,
      "pet adopt rejects null client");
  fails += expect(h2_gizclaw_req_create_pet_adopt(
                      (h2_gizclaw_service_t *)0x1, 0u, NULL, 1000u,
                      &pet_request) == H2_PAL_ERR_INVALID_ARG,
                  "pet adopt rejects null options");
  fails += expect(h2_gizclaw_req_create_pet_drive(NULL, 0u, &empty_drive, 1000u,
                                                  &pet_request) ==
                      H2_PAL_ERR_INVALID_ARG,
                  "pet drive rejects null client");
  fails += expect(h2_gizclaw_req_create_pet_action_get(
                      NULL, 0u, empty_drive.pet_name, 1000u, &pet_request) ==
                      H2_PAL_ERR_INVALID_ARG,
                  "pet actions rejects null client");
  const h2_gizclaw_pet_game_result_t game_result = {
      .game_name = {.data = "dinorun", .len = 7u},
      .score = 120,
      .max_score = 999,
      .duration_ms = 3000,
      .has_score = true,
      .has_max_score = true,
      .has_duration_ms = true,
  };
  const h2_gizclaw_pet_drive_options_t invalid_mixed_drive = {
      .pet_name = {.data = "pet-test-1", .len = 10u},
      .behavior = H2_GIZCLAW_PET_BEHAVIOR_PLAY,
      .game_result = &game_result,
  };
  fails += expect(h2_gizclaw_req_create_pet_drive(
                      (h2_gizclaw_service_t *)0x1, 0u, &invalid_mixed_drive,
                      1000u, &pet_request) == H2_PAL_ERR_INVALID_ARG,
                  "pet drive rejects behavior plus game result");
  uint8_t profile_request[16];
  size_t profile_request_len = 0u;
  fails += expect(h2_gizclaw_profile_encode_name_request(
                      (h2_gizclaw_str_t){.data = "Alice", .len = 5u},
                      profile_request, sizeof(profile_request),
                      &profile_request_len) == H2_PAL_OK,
                  "profile name request encodes");
  const uint8_t expected_name_request[] = {
      0x0a, 0x07, 0x0a, 0x05, 'A', 'l', 'i', 'c', 'e',
  };
  fails += expect(profile_request_len == sizeof(expected_name_request) &&
                      memcmp(profile_request, expected_name_request,
                             sizeof(expected_name_request)) == 0,
                  "profile name request omits emoji");
  fails +=
      expect(h2_gizclaw_profile_encode_emoji_request(
                 (h2_gizclaw_str_t){.data = "🤖", .len = 4u}, profile_request,
                 sizeof(profile_request), &profile_request_len) == H2_PAL_OK,
             "profile emoji request encodes");
  const uint8_t expected_profile_request[] = {
      0x0a, 0x06, 0x12, 0x04, 0xf0, 0x9f, 0xa4, 0x96,
  };
  fails += expect(profile_request_len == sizeof(expected_profile_request) &&
                      memcmp(profile_request, expected_profile_request,
                             sizeof(expected_profile_request)) == 0,
                  "profile emoji request omits name");
  const uint8_t profile_response[] = {
      0x0a, 0x0d, 0x12, 0x05, 'A',  'l',  'i',  'c',
      'e',  0x22, 0x04, 0xf0, 0x9f, 0xa4, 0x96,
  };
  fails += expect(h2_gizclaw_profile_decode_info_response(
                      profile_response, sizeof(profile_response), &profile) ==
                      H2_PAL_OK,
                  "profile response decodes");
  fails += expect(profile.has_name && strcmp(profile.name, "Alice") == 0 &&
                      profile.has_emoji && strcmp(profile.emoji, "🤖") == 0,
                  "profile response preserves name and emoji");
  const uint8_t overflowing_profile_response[] = {
      0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x02,
  };
  fails += expect(h2_gizclaw_profile_decode_info_response(
                      overflowing_profile_response,
                      sizeof(overflowing_profile_response),
                      &profile) == H2_PAL_ERR_FORMAT,
                  "profile response rejects overflowing varint");
  h2_gizclaw_req_t *speech = (h2_gizclaw_req_t *)1;
  fails +=
      expect(h2_gizclaw_req_create_speech_transcribe(
                 NULL, 0u, NULL, 1000u, &speech) == H2_PAL_ERR_INVALID_ARG &&
                 speech == NULL,
             "speech create rejects invalid arguments");
  fails += expect(h2_gizclaw_req_create_contact_list(
                      NULL, 0u, (h2_gizclaw_str_t){0}, 1u, 1000u,
                      &social_request) == H2_PAL_ERR_INVALID_ARG,
                  "contact list rejects invalid arguments");
  fails +=
      expect(h2_gizclaw_req_create_contact_create(
                 NULL, 0u, (h2_gizclaw_str_t){.data = "contact-1", .len = 9u},
                 (h2_gizclaw_str_t){.data = "A", .len = 1u},
                 (h2_gizclaw_str_t){.data = "1", .len = 1u}, 1000u,
                 &social_request) == H2_PAL_ERR_INVALID_ARG,
             "contact create rejects invalid arguments");
  fails += expect(h2_gizclaw_req_create_contact_get(
                      NULL, 0u, (h2_gizclaw_str_t){.data = "id", .len = 2u},
                      1000u, &social_request) == H2_PAL_ERR_INVALID_ARG,
                  "contact get rejects invalid arguments");
  char oversized_contact_name[H2_GIZCLAW_CONTACT_NAME_MAX_BYTES + 1u];
  memset(oversized_contact_name, 'a', sizeof(oversized_contact_name));
  fails += expect(h2_gizclaw_req_create_contact_get(
                      (h2_gizclaw_service_t *)0x1, 0u,
                      (h2_gizclaw_str_t){.data = oversized_contact_name,
                                         .len = sizeof(oversized_contact_name)},
                      1000u, &social_request) == H2_PAL_ERR_INVALID_ARG,
                  "contact get rejects an oversized name before allocation");
  fails += expect(h2_gizclaw_req_create_contact_put(
                      NULL, 0u, (h2_gizclaw_str_t){.data = "id", .len = 2u},
                      (h2_gizclaw_str_t){.data = "A", .len = 1u},
                      (h2_gizclaw_str_t){.data = "1", .len = 1u}, 1000u,
                      &social_request) == H2_PAL_ERR_INVALID_ARG,
                  "contact put rejects invalid arguments");
  fails += expect(h2_gizclaw_req_create_contact_delete(
                      NULL, 0u, (h2_gizclaw_str_t){.data = "id", .len = 2u},
                      1000u, &social_request) == H2_PAL_ERR_INVALID_ARG,
                  "contact delete rejects invalid arguments");
  const h2_gizclaw_str_t id = {.data = "id", .len = 2u};
  fails += expect(h2_gizclaw_req_create_friend_list(
                      NULL, 0u, (h2_gizclaw_str_t){0}, 8u, 1000u,
                      &social_request) == H2_PAL_ERR_INVALID_ARG,
                  "friend list rejects invalid arguments");
  fails += expect(h2_gizclaw_req_create_friend_info_get(NULL, 0u, id, 1000u,
                                                        &social_request) ==
                      H2_PAL_ERR_INVALID_ARG,
                  "friend info rejects invalid arguments");
  fails += expect(
      h2_gizclaw_req_create_friend_add(NULL, 0u, id, 1000u, &social_request) ==
              H2_PAL_ERR_INVALID_ARG &&
          h2_gizclaw_req_create_friend_delete(
              NULL, 0u, id, 1000u, &social_request) == H2_PAL_ERR_INVALID_ARG,
      "friend mutations reject invalid arguments");
  fails += expect(
      h2_gizclaw_req_create_friend_invite_token_create(
          NULL, 0u, 1000u, &social_request) == H2_PAL_ERR_INVALID_ARG &&
          h2_gizclaw_req_create_friend_invite_token_get(
              NULL, 0u, 1000u, &social_request) == H2_PAL_ERR_INVALID_ARG &&
          h2_gizclaw_req_create_friend_invite_token_clear(
              NULL, 0u, 1000u, &social_request) == H2_PAL_ERR_INVALID_ARG,
      "friend token operations reject invalid arguments");
  fails += expect(
      h2_gizclaw_req_create_friend_group_get(
          NULL, 0u, id, 1000u, &social_request) == H2_PAL_ERR_INVALID_ARG &&
          h2_gizclaw_req_create_friend_group_create(NULL, 0u, id, id, id, 1000u,
                                                    &social_request) ==
              H2_PAL_ERR_INVALID_ARG &&
          h2_gizclaw_req_create_friend_group_put(NULL, 0u, id, id, id, 1000u,
                                                 &social_request) ==
              H2_PAL_ERR_INVALID_ARG &&
          h2_gizclaw_req_create_friend_group_delete(
              NULL, 0u, id, 1000u, &social_request) == H2_PAL_ERR_INVALID_ARG &&
          h2_gizclaw_req_create_friend_group_join(NULL, 0u, id, id, 1000u,
                                                  &social_request) ==
              H2_PAL_ERR_INVALID_ARG,
      "friend group mutations reject invalid arguments");
  char oversized_group_name[H2_GIZCLAW_FRIEND_GROUP_NAME_MAX_BYTES + 1u];
  memset(oversized_group_name, 'g', sizeof(oversized_group_name));
  const h2_gizclaw_str_t oversized_group = {
      .data = oversized_group_name, .len = sizeof(oversized_group_name)};
  fails += expect(
      h2_gizclaw_req_create_friend_group_get(
          (h2_gizclaw_service_t *)0x1, 0u, oversized_group, 1000u,
          &social_request) == H2_PAL_ERR_INVALID_ARG &&
          h2_gizclaw_req_create_friend_group_create(
              (h2_gizclaw_service_t *)0x1, 0u, oversized_group, id, id, 1000u,
              &social_request) == H2_PAL_ERR_INVALID_ARG &&
          h2_gizclaw_req_create_friend_group_join(
              (h2_gizclaw_service_t *)0x1, 0u, id, oversized_group, 1000u,
              &social_request) == H2_PAL_ERR_INVALID_ARG,
      "friend group operations reject oversized names before allocation");
  fails += expect(
      h2_gizclaw_req_create_friend_group_invite_token_create(
          NULL, 0u, id, 1000u, &social_request) == H2_PAL_ERR_INVALID_ARG &&
          h2_gizclaw_req_create_friend_group_invite_token_get(
              NULL, 0u, id, 1000u, &social_request) == H2_PAL_ERR_INVALID_ARG &&
          h2_gizclaw_req_create_friend_group_invite_token_clear(
              NULL, 0u, id, 1000u, &social_request) == H2_PAL_ERR_INVALID_ARG,
      "friend group token operations reject invalid arguments");
  fails += expect(h2_gizclaw_req_create_friend_group_member_list(
                      NULL, 0u, id, (h2_gizclaw_str_t){0}, 8u, 1000u,
                      &social_request) == H2_PAL_ERR_INVALID_ARG &&
                      h2_gizclaw_req_create_friend_group_member_put(
                          NULL, 0u, id, id, H2_GIZCLAW_FRIEND_GROUP_ROLE_MEMBER,
                          1000u, &social_request) == H2_PAL_ERR_INVALID_ARG &&
                      h2_gizclaw_req_create_friend_group_member_delete(
                          NULL, 0u, id, id, 1000u, &social_request) ==
                          H2_PAL_ERR_INVALID_ARG,
                  "friend group member operations reject invalid arguments");
  fails += expect(h2_gizclaw_provider_result_to_gzc(H2_PAL_ERR_NOT_FOUND) ==
                      GZC_ERR_UNSUPPORTED,
                  "provider not-found maps to unsupported");
  fails += expect(h2_gizclaw_provider_result_to_gzc(H2_PAL_ERR_UNSUPPORTED) ==
                      GZC_ERR_UNSUPPORTED,
                  "provider unsupported remains unsupported");
  fails +=
      expect(h2_gizclaw_provider_result_to_gzc(H2_PAL_ERR_IO) == GZC_ERR_RPC,
             "provider failures map to RPC errors");
  fails += h2_gizclaw_home_tests();
  const int stream_errors[] = {
      H2_PAL_ERR_NOT_FOUND,   H2_GIZCLAW_ERR_REMOTE,
      H2_PAL_ERR_FORMAT,      H2_PAL_ERR_TIMEOUT,
      H2_PAL_ERR_CLOSED,      H2_PAL_ERR_NO_MEMORY,
      H2_PAL_ERR_WOULD_BLOCK, 1,
  };
  for (size_t i = 0; i < sizeof(stream_errors) / sizeof(stream_errors[0]);
       ++i) {
    for (int eos = 0; eos < 2; ++eos) {
      int sdk_result = GZC_OK;
      const int expected =
          stream_errors[i] == H2_PAL_ERR_WOULD_BLOCK || stream_errors[i] > 0
              ? H2_PAL_ERR_IO
              : stream_errors[i];
      fails += expect(
          h2_gizclaw_test_stream_failure_result(stream_errors[i], eos != 0,
                                                &sdk_result) == expected &&
              sdk_result == GZC_ERR_RPC,
          "stream callbacks preserve PAL errors without SDK code collisions");
    }
  }
  fails += expect(h2_gizclaw_test_audio_rings(),
                  "PCM and packet rings preserve wrap order and bounds");

  if (fails == 0) {
    printf("PASS h2_gizclaw_client\n");
  }
  return fails == 0 ? 0 : 1;
}
