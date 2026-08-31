#include "h2_gizclaw_client.h"
#include "h2_gizclaw_conversation.h"
#include "h2_gizclaw_internal.h"
#include "h2_gizclaw_pet.h"
#include "h2_gizclaw_points.h"
#include "h2_gizclaw_profile.h"
#include "h2_gizclaw_profile_internal.h"
#include "h2_gizclaw_registration.h"
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
static uint64_t test_monotonic_ms = 1234u;
static bool test_cancel_requested;
static int test_opus_send_calls;
static h2_pal_result_t test_opus_send_result;
static uint8_t test_opus_bytes[H2_PAL_WEBRTC_OPUS_MAX_PACKET_SIZE];
static size_t test_opus_len;
static int test_opus_receive_calls;
static int test_peer_close_calls;

int h2_gizclaw_home_tests(void);
int h2_gizclaw_conversation_audio_tests(void);
static int expect(int condition, const char *message);

typedef struct test_speed_test {
  int result;
  int calls;
  int64_t requested_up_bytes;
  int64_t requested_down_bytes;
  h2_gizclaw_test_speed_test_result_t response;
} test_speed_test_t;

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

typedef struct test_contact_text {
  const char *data;
  size_t len;
} test_contact_text_t;

typedef struct test_contact_rpc {
  h2_gizclaw_rpc_method_t expected_method;
  const uint8_t *expected_request;
  size_t expected_request_len;
  const uint8_t *response;
  size_t response_len;
  bool has_error;
  int error_code;
  bool request_matches;
  int calls;
} test_contact_rpc_t;

typedef struct test_rpc_sequence {
  test_contact_rpc_t *steps;
  size_t step_count;
  size_t next_step;
} test_rpc_sequence_t;

typedef struct test_message_audio_rpc {
  const uint8_t *expected_request;
  size_t expected_request_len;
  const uint8_t *response;
  size_t response_len;
  const uint8_t *audio;
  size_t audio_len;
  size_t eos_count;
  bool request_matches;
  int calls;
} test_message_audio_rpc_t;

typedef struct test_audio_sink {
  uint8_t bytes[16];
  size_t len;
} test_audio_sink_t;

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

static bool test_encode_contact_text(pb_ostream_t *stream,
                                     const pb_field_t *field,
                                     void *const *arg) {
  const test_contact_text_t *text = *arg;
  return text != NULL && pb_encode_tag_for_field(stream, field) &&
         pb_encode_string(stream, (const pb_byte_t *)text->data, text->len);
}

static bool test_encode_friend_group_message(pb_ostream_t *stream,
                                             const pb_field_t *field,
                                             void *const *arg) {
  const gizclaw_rpc_v1_FriendGroupMessageObject *message = *arg;
  return message != NULL && pb_encode_tag_for_field(stream, field) &&
         pb_encode_submessage(
             stream, gizclaw_rpc_v1_FriendGroupMessageObject_fields, message);
}

static void test_friend_group_message_fixture(
    gizclaw_rpc_v1_FriendGroupMessageObject *message) {
  *message = (gizclaw_rpc_v1_FriendGroupMessageObject)
      gizclaw_rpc_v1_FriendGroupMessageObject_init_zero;
  (void)snprintf(message->created_at, sizeof(message->created_at), "%s",
                 "2026-08-05T00:00:00Z");
  (void)snprintf(message->friend_group_name, sizeof(message->friend_group_name),
                 "%s", "group-1");
  (void)snprintf(message->sender_peer_public_key,
                 sizeof(message->sender_peer_public_key), "%s", "peer-1");
  message->has_sender_peer_public_key = true;
  (void)snprintf(message->name, sizeof(message->name), "%s", "history-1");
  (void)snprintf(message->actor_name, sizeof(message->actor_name), "%s",
                 "Nova");
  (void)snprintf(message->text, sizeof(message->text), "%s", "hello");
  message->type =
      gizclaw_rpc_v1_PeerRunHistoryEntryType_PEER_RUN_HISTORY_ENTRY_TYPE_AGENT;
  message->audio_available = true;
}

static bool test_encode_friend_group_message_list_response(uint8_t *buffer,
                                                           size_t capacity,
                                                           bool invalid_utf8,
                                                           size_t *out_len) {
  gizclaw_rpc_v1_FriendGroupMessageObject message;
  test_friend_group_message_fixture(&message);
  if (invalid_utf8) {
    message.text[0] = (char)0xc0;
    message.text[1] = (char)0x80;
    message.text[2] = '\0';
  }
  gizclaw_rpc_v1_FriendGroupMessageListResponse response =
      gizclaw_rpc_v1_FriendGroupMessageListResponse_init_zero;
  response.items.funcs.encode = test_encode_friend_group_message;
  response.items.arg = &message;
  pb_ostream_t stream = pb_ostream_from_buffer(buffer, capacity);
  if (!pb_encode(&stream, gizclaw_rpc_v1_FriendGroupMessageListResponse_fields,
                 &response)) {
    return false;
  }
  *out_len = stream.bytes_written;
  return true;
}

static bool test_encode_friend_group_message_get_response(uint8_t *buffer,
                                                          size_t capacity,
                                                          size_t *out_len) {
  gizclaw_rpc_v1_FriendGroupMessageGetResponse response =
      gizclaw_rpc_v1_FriendGroupMessageGetResponse_init_zero;
  response.has_value = true;
  test_friend_group_message_fixture(&response.value);
  pb_ostream_t stream = pb_ostream_from_buffer(buffer, capacity);
  if (!pb_encode(&stream, gizclaw_rpc_v1_FriendGroupMessageGetResponse_fields,
                 &response)) {
    return false;
  }
  *out_len = stream.bytes_written;
  return true;
}

static bool test_encode_contact_response_with_name(uint8_t *buffer,
                                                   size_t capacity,
                                                   const char *display_name,
                                                   size_t display_name_len,
                                                   size_t *out_len) {
  gizclaw_rpc_v1_ContactPutResponse response =
      gizclaw_rpc_v1_ContactPutResponse_init_zero;
  test_contact_text_t text[] = {
      {.data = "2026-07-27T00:00:00Z", .len = 20u},
      {.data = display_name, .len = display_name_len},
      {.data = "contact-1", .len = 9u},
      {.data = "+8613900000000", .len = 14u},
      {.data = "2026-07-27T00:01:00Z", .len = 20u},
  };
  response.has_value = true;
  response.value.created_at.funcs.encode = test_encode_contact_text;
  response.value.created_at.arg = &text[0];
  response.value.display_name.funcs.encode = test_encode_contact_text;
  response.value.display_name.arg = &text[1];
  response.value.name.funcs.encode = test_encode_contact_text;
  response.value.name.arg = &text[2];
  response.value.phone_number.funcs.encode = test_encode_contact_text;
  response.value.phone_number.arg = &text[3];
  response.value.updated_at.funcs.encode = test_encode_contact_text;
  response.value.updated_at.arg = &text[4];
  pb_ostream_t stream = pb_ostream_from_buffer(buffer, capacity);
  if (!pb_encode(&stream, gizclaw_rpc_v1_ContactPutResponse_fields,
                 &response)) {
    return false;
  }
  *out_len = stream.bytes_written;
  return true;
}

static bool test_encode_contact_response(uint8_t *buffer, size_t capacity,
                                         size_t *out_len) {
  return test_encode_contact_response_with_name(buffer, capacity, "Alice", 5u,
                                                out_len);
}

static bool test_encode_workspace_delete_response(uint8_t *buffer,
                                                  size_t capacity,
                                                  size_t *out_len) {
  gizclaw_rpc_v1_WorkspaceDeleteResponse response =
      gizclaw_rpc_v1_WorkspaceDeleteResponse_init_zero;
  test_contact_text_t text[] = {
      {.data = "workspace-1", .len = 11u},
      {.data = "chat", .len = 4u},
  };
  response.has_value = true;
  response.value.name.funcs.encode = test_encode_contact_text;
  response.value.name.arg = &text[0];
  response.value.workflow_name.funcs.encode = test_encode_contact_text;
  response.value.workflow_name.arg = &text[1];
  response.value.available = true;
  pb_ostream_t stream = pb_ostream_from_buffer(buffer, capacity);
  if (!pb_encode(&stream, gizclaw_rpc_v1_WorkspaceDeleteResponse_fields,
                 &response)) {
    return false;
  }
  *out_len = stream.bytes_written;
  return true;
}

static bool test_encode_workspace_put_response(uint8_t *buffer, size_t capacity,
                                               size_t *out_len) {
  gizclaw_rpc_v1_WorkspacePutResponse response =
      gizclaw_rpc_v1_WorkspacePutResponse_init_zero;
  test_contact_text_t text[] = {
      {.data = "workspace-1", .len = 11u},
      {.data = "chat", .len = 4u},
  };
  response.has_value = true;
  response.value.name.funcs.encode = test_encode_contact_text;
  response.value.name.arg = &text[0];
  response.value.workflow_name.funcs.encode = test_encode_contact_text;
  response.value.workflow_name.arg = &text[1];
  response.value.available = true;
  pb_ostream_t stream = pb_ostream_from_buffer(buffer, capacity);
  if (!pb_encode(&stream, gizclaw_rpc_v1_WorkspacePutResponse_fields,
                 &response)) {
    return false;
  }
  *out_len = stream.bytes_written;
  return true;
}

static bool test_encode_workspace_get_response(uint8_t *buffer, size_t capacity,
                                               bool has_override,
                                               size_t *out_len) {
  gizclaw_rpc_v1_WorkspaceGetResponse response =
      gizclaw_rpc_v1_WorkspaceGetResponse_init_zero;
  response.has_value = true;
  response.value.has_parameters = true;
  response.value.parameters.which_value =
      gizclaw_rpc_v1_WorkspaceParameters_flowcraft_workspace_parameters_tag;
  response.value.parameters.value.flowcraft_workspace_parameters.agent_type =
      gizclaw_rpc_v1_FlowcraftWorkspaceParametersAgentType_FLOWCRAFT_WORKSPACE_PARAMETERS_AGENT_TYPE_FLOWCRAFT;
  response.value.parameters.value.flowcraft_workspace_parameters.has_input =
      true;
  response.value.parameters.value.flowcraft_workspace_parameters.input =
      gizclaw_rpc_v1_WorkspaceInputMode_WORKSPACE_INPUT_MODE_PUSH_TO_TALK;
  response.value.parameters.value.flowcraft_workspace_parameters.has_e2e =
      has_override;
  response.value.parameters.value.flowcraft_workspace_parameters.e2e =
      has_override;
  pb_ostream_t stream = pb_ostream_from_buffer(buffer, capacity);
  if (!pb_encode(&stream, gizclaw_rpc_v1_WorkspaceGetResponse_fields,
                 &response)) {
    return false;
  }
  *out_len = stream.bytes_written;
  return true;
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

static bool test_encode_message_audio_response(uint8_t *buffer, size_t capacity,
                                               const char *friend_group_name,
                                               const char *history_name,
                                               size_t audio_len,
                                               size_t *out_len) {
  gizclaw_rpc_v1_FriendGroupMessageAudioGetResponse response =
      gizclaw_rpc_v1_FriendGroupMessageAudioGetResponse_init_zero;
  (void)snprintf(response.friend_group_name, sizeof(response.friend_group_name),
                 "%s", friend_group_name);
  (void)snprintf(response.history_name, sizeof(response.history_name), "%s",
                 history_name);
  (void)snprintf(response.mime_type, sizeof(response.mime_type), "%s",
                 "audio/ogg");
  response.size_bytes = (int64_t)audio_len;
  pb_ostream_t stream = pb_ostream_from_buffer(buffer, capacity);
  if (!pb_encode(&stream,
                 gizclaw_rpc_v1_FriendGroupMessageAudioGetResponse_fields,
                 &response)) {
    return false;
  }
  *out_len = stream.bytes_written;
  return true;
}

static int test_message_audio_rpc_call(void *user, h2_gizclaw_client_t *client,
                                       h2_gizclaw_rpc_method_t method,
                                       h2_gizclaw_rpc_bytes_t params_payload,
                                       h2_gizclaw_rpc_stream_fn on_event,
                                       void *event_user) {
  test_message_audio_rpc_t *mock = user;
  (void)client;
  ++mock->calls;
  mock->request_matches =
      method == H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_MESSAGES_AUDIO_GET &&
      params_payload.len == mock->expected_request_len &&
      memcmp(params_payload.data, mock->expected_request,
             mock->expected_request_len) == 0;
  h2_gizclaw_rpc_stream_event_t event = {
      .kind = H2_GIZCLAW_RPC_STREAM_RESPONSE,
      .result_payload =
          {
              .data = mock->response,
              .len = mock->response_len,
          },
  };
  int rc = on_event(event_user, &event);
  if (rc != H2_PAL_OK)
    return rc;
  const size_t first_len = mock->audio_len / 2u;
  event = (h2_gizclaw_rpc_stream_event_t){
      .kind = H2_GIZCLAW_RPC_STREAM_DATA,
      .data = {.data = mock->audio, .len = first_len},
  };
  rc = on_event(event_user, &event);
  if (rc != H2_PAL_OK)
    return rc;
  event.data = (h2_gizclaw_rpc_bytes_t){
      .data = mock->audio + first_len,
      .len = mock->audio_len - first_len,
  };
  rc = on_event(event_user, &event);
  if (rc != H2_PAL_OK)
    return rc;
  event = (h2_gizclaw_rpc_stream_event_t){
      .kind = H2_GIZCLAW_RPC_STREAM_EOS,
  };
  for (size_t index = 0u; index < mock->eos_count; ++index) {
    rc = on_event(event_user, &event);
    if (rc != H2_PAL_OK)
      return rc;
  }
  return H2_PAL_OK;
}

static int test_audio_write(void *user, const uint8_t *data, size_t len) {
  test_audio_sink_t *sink = user;
  if (sink == NULL || data == NULL || len == 0u ||
      len > sizeof(sink->bytes) - sink->len) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memcpy(sink->bytes + sink->len, data, len);
  sink->len += len;
  return H2_PAL_OK;
}

static int test_message_audio_stream(h2_gizclaw_client_t *client) {
  int fails = 0;
  const uint8_t expected_request[] = {
      0x0a, 0x07, 'g', 'r', 'o', 'u', 'p', '-', 'a', 0x12,
      0x09, 'h',  'i', 's', 't', 'o', 'r', 'y', '-', '1',
  };
  const uint8_t audio[] = {0x4f, 0x67, 0x67, 0x53};
  uint8_t response[256];
  size_t response_len = 0u;
  fails += expect(test_encode_message_audio_response(
                      response, sizeof(response), "group-a", "history-1",
                      sizeof(audio), &response_len),
                  "friend group audio metadata fixture encodes");
  test_message_audio_rpc_t mock = {
      .expected_request = expected_request,
      .expected_request_len = sizeof(expected_request),
      .response = response,
      .response_len = response_len,
      .audio = audio,
      .audio_len = sizeof(audio),
      .eos_count = 1u,
  };
  h2_gizclaw_test_set_rpc_call_stream(test_message_audio_rpc_call, &mock);
  test_audio_sink_t sink = {0};
  h2_gizclaw_friend_group_message_audio_info_t info = {0};
  int rc = h2_gizclaw_client_friend_group_message_audio_get(
      client, (h2_gizclaw_str_t){.data = "group-a", .len = 7u},
      (h2_gizclaw_str_t){.data = "history-1", .len = 9u}, test_audio_write,
      &sink, &info);
  fails += expect(
      rc == H2_PAL_OK && mock.calls == 1 && mock.request_matches &&
          sink.len == sizeof(audio) &&
          memcmp(sink.bytes, audio, sizeof(audio)) == 0 &&
          info.friend_group_name != NULL &&
          strcmp(info.friend_group_name, "group-a") == 0 &&
          info.history_id != NULL &&
          strcmp(info.history_id, "history-1") == 0 && info.mime_type != NULL &&
          strcmp(info.mime_type, "audio/ogg") == 0 &&
          info.size_bytes == sizeof(audio) &&
          info.received_bytes == sizeof(audio),
      "friend group audio validates request, metadata, chunks, and echoes");
  h2_gizclaw_friend_group_message_audio_info_deinit(client, &info);

  fails += expect(test_encode_message_audio_response(
                      response, sizeof(response), "other-group", "history-1",
                      sizeof(audio), &response_len),
                  "friend group audio mismatch fixture encodes");
  mock.response_len = response_len;
  sink = (test_audio_sink_t){0};
  rc = h2_gizclaw_client_friend_group_message_audio_get(
      client, (h2_gizclaw_str_t){.data = "group-a", .len = 7u},
      (h2_gizclaw_str_t){.data = "history-1", .len = 9u}, test_audio_write,
      &sink, &info);
  fails += expect(rc == H2_PAL_ERR_FORMAT && info.friend_group_name == NULL &&
                      info.history_id == NULL && info.mime_type == NULL &&
                      info.size_bytes == 0u && info.received_bytes == 0u,
                  "friend group audio rejects and clears mismatched echoes");

  fails += expect(test_encode_message_audio_response(
                      response, sizeof(response), "group-a", "history-1",
                      sizeof(audio), &response_len),
                  "friend group audio EOS fixture encodes");
  mock.response_len = response_len;
  mock.eos_count = 0u;
  sink = (test_audio_sink_t){0};
  rc = h2_gizclaw_client_friend_group_message_audio_get(
      client, (h2_gizclaw_str_t){.data = "group-a", .len = 7u},
      (h2_gizclaw_str_t){.data = "history-1", .len = 9u}, test_audio_write,
      &sink, &info);
  fails += expect(rc == H2_PAL_ERR_FORMAT && info.friend_group_name == NULL &&
                      info.history_id == NULL && info.mime_type == NULL,
                  "friend group audio rejects a missing EOS");

  mock.eos_count = 2u;
  sink = (test_audio_sink_t){0};
  rc = h2_gizclaw_client_friend_group_message_audio_get(
      client, (h2_gizclaw_str_t){.data = "group-a", .len = 7u},
      (h2_gizclaw_str_t){.data = "history-1", .len = 9u}, test_audio_write,
      &sink, &info);
  fails += expect(rc == H2_PAL_ERR_FORMAT && info.friend_group_name == NULL &&
                      info.history_id == NULL && info.mime_type == NULL,
                  "friend group audio rejects a duplicate EOS");
  h2_gizclaw_test_set_rpc_call_stream(NULL, NULL);
  return fails;
}

static bool test_encode_pet_delete_response(uint8_t *buffer, size_t capacity,
                                            size_t *out_len) {
  gizclaw_rpc_v1_ServerPetDeleteResponse response =
      gizclaw_rpc_v1_ServerPetDeleteResponse_init_zero;
  test_contact_text_t text[] = {
      {.data = "pet-1", .len = 5u},
      {.data = "petdef-1", .len = 8u},
      {.data = "E2E Pet", .len = 7u},
  };
  response.has_value = true;
  response.value.name.funcs.encode = test_encode_contact_text;
  response.value.name.arg = &text[0];
  response.value.pet_def_name.funcs.encode = test_encode_contact_text;
  response.value.pet_def_name.arg = &text[1];
  response.value.display_name.funcs.encode = test_encode_contact_text;
  response.value.display_name.arg = &text[2];
  pb_ostream_t stream = pb_ostream_from_buffer(buffer, capacity);
  if (!pb_encode(&stream, gizclaw_rpc_v1_ServerPetDeleteResponse_fields,
                 &response)) {
    return false;
  }
  *out_len = stream.bytes_written;
  return true;
}

static int
test_speed_test_call(void *user, h2_gizclaw_client_t *client,
                     const h2_gizclaw_test_speed_test_request_t *request,
                     h2_gizclaw_test_speed_test_result_t *out_result) {
  test_speed_test_t *test = user;
  (void)client;
  ++test->calls;
  test->requested_up_bytes = request->up_content_length;
  test->requested_down_bytes = request->down_content_length;
  *out_result = test->response;
  return test->result;
}

static int test_speed_test_adapter(h2_gizclaw_client_t *client) {
  int fails = 0;
  test_speed_test_t test = {
      .result = GZC_OK,
      .response =
          {
              .up_bytes = 2048,
              .down_bytes = 1024,
              .up_duration_ms = 10,
              .down_duration_ms = 20,
          },
  };
  h2_gizclaw_test_set_speed_test(test_speed_test_call, &test);
  h2_gizclaw_speedtest_result_t result = {0};
  fails +=
      expect(h2_gizclaw_client_speedtest_measure(client, 2048u, 1024u,
                                                 &result) == H2_PAL_OK,
             "full-duplex speedtest accepts complete directional SDK results");
  fails += expect(
      test.calls == 1 && test.requested_up_bytes == 2048 &&
          test.requested_down_bytes == 1024 && result.upload_bytes == 2048u &&
          result.download_bytes == 1024u && result.upload_elapsed_ms == 10u &&
          result.elapsed_ms == 20u &&
          result.upload_bits_per_second == 1638400u &&
          result.download_bits_per_second == 409600u,
      "full-duplex speedtest uses each direction's SDK duration");

  test.response.up_duration_ms = 0;
  test.response.down_duration_ms = 0;
  result = (h2_gizclaw_speedtest_result_t){0};
  fails +=
      expect(h2_gizclaw_client_speedtest_measure(client, 2048u, 1024u,
                                                 &result) == H2_PAL_OK &&
                 result.upload_elapsed_ms == 1u && result.elapsed_ms == 1u &&
                 result.upload_bits_per_second == 16384000u &&
                 result.download_bits_per_second == 8192000u,
             "speedtest normalizes each completed zero-duration direction");

  test.response.up_duration_ms = 10;
  test.response.down_duration_ms = 20;
  test.response.up_bytes = 2047;
  result = (h2_gizclaw_speedtest_result_t){.upload_bytes = UINT64_MAX};
  fails += expect(h2_gizclaw_client_speedtest_measure(
                      client, 2048u, 1024u, &result) == H2_PAL_ERR_IO &&
                      result.upload_bytes == 0u,
                  "speedtest rejects an incomplete SDK upload result");

  test.response.up_bytes = 2048;
  test.response.up_duration_ms = -1;
  result = (h2_gizclaw_speedtest_result_t){.upload_bytes = UINT64_MAX};
  fails += expect(h2_gizclaw_client_speedtest_measure(
                      client, 2048u, 1024u, &result) == H2_PAL_ERR_IO &&
                      result.upload_bytes == 0u,
                  "speedtest rejects a negative directional duration");

  test.response.up_duration_ms = 10;
  test.result = GZC_ERR_TIMEOUT;
  result = (h2_gizclaw_speedtest_result_t){.upload_bytes = UINT64_MAX};
  fails += expect(h2_gizclaw_client_speedtest_measure(
                      client, 2048u, 1024u, &result) == H2_PAL_ERR_IO &&
                      result.upload_bytes == 0u,
                  "speedtest clears the result after an SDK transport failure");

  test.result = GZC_OK;
  int calls_before_invalid = test.calls;
  result = (h2_gizclaw_speedtest_result_t){.upload_bytes = UINT64_MAX};
  fails += expect(
      h2_gizclaw_client_speedtest_measure(client, 0u, 0u, &result) ==
              H2_PAL_ERR_INVALID_ARG &&
          test.calls == calls_before_invalid && result.upload_bytes == 0u,
      "speedtest rejects an empty request before calling the SDK");
  result = (h2_gizclaw_speedtest_result_t){.upload_bytes = UINT64_MAX};
  fails += expect(
      h2_gizclaw_client_speedtest_measure(
          client, H2_GIZCLAW_SPEEDTEST_MAX_BYTES + 1u, 0u, &result) ==
              H2_PAL_ERR_INVALID_ARG &&
          test.calls == calls_before_invalid && result.upload_bytes == 0u,
      "speedtest rejects a direction above the public one-GiB limit");

  test.response.up_bytes = 1024 * 1024;
  test.response.down_bytes = 1024 * 1024;
  test.response.up_duration_ms = 10;
  test.response.down_duration_ms = 20;
  fails += expect(h2_gizclaw_client_speedtest(client) == H2_PAL_OK &&
                      test.requested_up_bytes == 1024 * 1024 &&
                      test.requested_down_bytes == 1024 * 1024,
                  "default speedtest requests one MiB in each direction");

  test.response.up_bytes = 0;
  test.response.down_bytes = 1024;
  test.response.up_duration_ms = 0;
  test.response.down_duration_ms = 20;
  result = (h2_gizclaw_speedtest_result_t){0};
  fails += expect(
      h2_gizclaw_client_speedtest_download(client, 1024u, &result) == H2_PAL_OK,
      "speedtest accepts SDK completion after response, data, and EOS");
  fails += expect(
      test.requested_up_bytes == 0 && test.requested_down_bytes == 1024 &&
          result.download_bytes == 1024u && result.elapsed_ms == 20u &&
          result.download_bits_per_second == 409600u,
      "speedtest uses the dedicated download result and direction duration");

  test.response.down_bytes = 1023;
  result = (h2_gizclaw_speedtest_result_t){
      .download_bytes = UINT64_MAX,
      .elapsed_ms = UINT64_MAX,
      .download_bits_per_second = UINT64_MAX,
  };
  fails += expect(h2_gizclaw_client_speedtest_download(
                      client, 1024u, &result) == H2_PAL_ERR_IO &&
                      result.download_bytes == 0u && result.elapsed_ms == 0u &&
                      result.download_bits_per_second == 0u,
                  "speedtest rejects an incomplete SDK download result");
  h2_gizclaw_test_set_speed_test(NULL, NULL);
  return fails;
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
        !h2_gizclaw_conversation_input_ready(test->conversation);
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
  fails += expect(h2_gizclaw_conversation_open(client, workspace, 11u, 1000,
                                               &conversation) == H2_PAL_OK &&
                      conversation != NULL && event.send_count == 1u,
                  "closed-poll mapping test opens an active conversation");
  event.conversation = conversation;
  fails +=
      expect(h2_gizclaw_client_poll(client, 17) == H2_PAL_ERR_WOULD_BLOCK &&
                 poll.calls == 1 && poll.client != NULL &&
                 poll.timeout_ms == 17 && event.close_count == 0u &&
                 h2_gizclaw_conversation_input_ready(conversation) &&
                 !h2_gizclaw_test_client_terminal_closed(client),
             "would-block poll preserves the active client and conversation");
  poll.result = GZC_ERR_CLOSED;
  fails += expect(
      h2_gizclaw_client_poll(client, 37) == H2_PAL_ERR_CLOSED &&
          poll.calls == 2 && poll.client != NULL && poll.timeout_ms == 37 &&
          event.close_count == 1u &&
          event.conversation_invalidated_when_closed &&
          !h2_gizclaw_conversation_input_ready(conversation) &&
          h2_gizclaw_test_client_terminal_closed(client),
      "SDK closed poll invalidates the conversation before releasing Event");
  fails +=
      expect(h2_gizclaw_conversation_poll(
                 conversation, 0, &(h2_gizclaw_conversation_event_t){0}) ==
                 H2_PAL_ERR_CLOSED,
             "closed-poll mapping leaves the active conversation unusable");
  fails +=
      expect(h2_gizclaw_client_poll(client, 99) == H2_PAL_ERR_CLOSED &&
                 poll.calls == 2 && event.close_count == 1u,
             "terminal poll bypasses the SDK and does not release Event twice");
  h2_gizclaw_conversation_deinit(conversation);
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
  fails += expect(h2_gizclaw_conversation_open(client, workspace, 9u, 1000,
                                               &conversation) == H2_PAL_OK &&
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
  fails += expect(h2_gizclaw_client_dispatch_event(client, 0, NULL, NULL) ==
                          H2_PAL_ERR_WOULD_BLOCK &&
                      test.read_count == 2u && test.close_count == 0u &&
                      h2_gizclaw_conversation_input_ready(conversation) &&
                      !h2_gizclaw_test_client_terminal_closed(client),
                  "would-block client Event dispatch preserves the active "
                  "conversation");
  test.read_result = GZC_ERR_RPC;
  fails +=
      expect(h2_gizclaw_client_dispatch_event(client, 0, NULL, NULL) ==
                          H2_PAL_ERR_IO &&
                      test.read_count == 3u && test.close_count == 1u &&
                      !h2_gizclaw_conversation_input_ready(conversation) &&
                      h2_gizclaw_test_client_terminal_closed(client),
                  "malformed client Event dispatch invalidates the conversation and "
                  "poisons the client");
  fails += expect(
      h2_gizclaw_client_poll(client, 0) == H2_PAL_ERR_CLOSED,
      "client poll remains closed after the mandatory Event transport closes");
  fails += expect(
      h2_gizclaw_client_close(client) == H2_PAL_OK && test.close_count == 1u,
      "explicit close after closed poll does not release Event twice");
  h2_gizclaw_conversation_deinit(conversation);
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
  fails +=
      expect(h2_gizclaw_conversation_open(send_client, workspace, 10u, 1000,
                                          &send_conversation) == H2_PAL_OK &&
                 send_conversation != NULL && send_test.send_count == 1u,
             "send-failure test opens an active logical conversation");
  fails += expect(
      h2_gizclaw_conversation_commit(send_conversation, 10u) == H2_PAL_ERR_IO &&
          send_test.send_count == 2u && send_test.close_count == 1u &&
          !h2_gizclaw_conversation_input_ready(send_conversation) &&
          h2_gizclaw_test_client_terminal_closed(send_client),
      "non-transient Event send failure invalidates the turn and client");
  fails += expect(h2_gizclaw_client_close(send_client) == H2_PAL_OK &&
                      send_test.close_count == 1u,
                  "send failure and explicit close release Event exactly once");
  h2_gizclaw_conversation_deinit(send_conversation);
  h2_gizclaw_client_deinit(send_client);
  h2_gizclaw_test_set_event_ops(NULL, NULL, NULL, NULL);
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

  gzc_peer_event_t event = gizclaw_events_v1_PeerEvent_init_zero;
  event.type = gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_TEXT_DELTA;
  event.which_payload = gizclaw_events_v1_PeerEvent_text_delta_tag;
  (void)snprintf(event.payload.text_delta.stream_id,
                 sizeof(event.payload.text_delta.stream_id), "%s", "demo-2");
  fails +=
      expect(h2_gizclaw_test_peer_event_matches_stream(&event, "demo-2") &&
                 !h2_gizclaw_test_peer_event_matches_stream(&event, "demo-1"),
             "conversation delivery rejects a stale text-delta event");
  event.type = gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_TEXT_DONE;
  event.which_payload = gizclaw_events_v1_PeerEvent_text_done_tag;
  (void)snprintf(event.payload.text_done.stream_id,
                 sizeof(event.payload.text_done.stream_id), "%s", "demo-2");
  fails +=
      expect(h2_gizclaw_test_peer_event_matches_stream(&event, "demo-2") &&
                 !h2_gizclaw_test_peer_event_matches_stream(&event, "demo-1"),
             "conversation delivery rejects a stale text-done event");
  event.type = gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_EOS;
  event.which_payload = gizclaw_events_v1_PeerEvent_eos_tag;
  (void)snprintf(event.payload.eos.stream_id,
                 sizeof(event.payload.eos.stream_id), "%s", "demo-2");
  fails +=
      expect(h2_gizclaw_test_peer_event_matches_stream(&event, "demo-2") &&
                 !h2_gizclaw_test_peer_event_matches_stream(&event, "demo-1"),
             "conversation delivery rejects a stale EOS event");
  event.type =
      gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_WORKSPACE_HISTORY_UPDATED;
  event.which_payload =
      gizclaw_events_v1_PeerEvent_workspace_history_updated_tag;
  fails += expect(
      h2_gizclaw_test_peer_event_matches_stream(&event, "demo-2"),
      "connection-scoped events are not rejected by conversation filtering");

  event.type = gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_TEXT_DELTA;
  event.which_payload = gizclaw_events_v1_PeerEvent_text_delta_tag;
  (void)snprintf(event.payload.text_delta.stream_id,
                 sizeof(event.payload.text_delta.stream_id), "%s",
                 "assistant-response");
  (void)snprintf(event.payload.text_delta.label,
                 sizeof(event.payload.text_delta.label), "%s", "assistant");
  char response_stream_id[H2_GIZCLAW_CONVERSATION_STREAM_ID_MAX_BYTES + 1u] = {
      0};
  char transcript_stream_id[H2_GIZCLAW_CONVERSATION_STREAM_ID_MAX_BYTES + 1u] =
      {0};
  char assistant_stream_id[H2_GIZCLAW_CONVERSATION_STREAM_ID_MAX_BYTES + 1u] = {
      0};
  fails += expect(
      h2_gizclaw_test_peer_event_matches_conversation(
          &event, "demo-2", response_stream_id, transcript_stream_id,
          assistant_stream_id, sizeof(response_stream_id)) &&
          response_stream_id[0] == '\0' && transcript_stream_id[0] == '\0' &&
          strcmp(assistant_stream_id, "assistant-response") == 0,
      "conversation binds the first assistant response-local stream ID");
  (void)snprintf(event.payload.text_delta.stream_id,
                 sizeof(event.payload.text_delta.stream_id), "%s",
                 "transcript-response");
  (void)snprintf(event.payload.text_delta.label,
                 sizeof(event.payload.text_delta.label), "%s", "transcript");
  fails += expect(
      h2_gizclaw_test_peer_event_matches_conversation(
          &event, "demo-2", response_stream_id, transcript_stream_id,
          assistant_stream_id, sizeof(response_stream_id)) &&
          strcmp(transcript_stream_id, "transcript-response") == 0 &&
          strcmp(assistant_stream_id, "assistant-response") == 0,
      "conversation binds transcript and assistant response routes separately");
  (void)snprintf(event.payload.text_delta.stream_id,
                 sizeof(event.payload.text_delta.stream_id), "%s",
                 "assistant-response:audio");
  (void)snprintf(event.payload.text_delta.label,
                 sizeof(event.payload.text_delta.label), "%s", "assistant");
  fails += expect(
      h2_gizclaw_test_peer_event_matches_conversation(
          &event, "demo-2", response_stream_id, transcript_stream_id,
          assistant_stream_id, sizeof(response_stream_id)),
      "conversation accepts a bound assistant response-local stream suffix");
  (void)snprintf(event.payload.text_delta.stream_id,
                 sizeof(event.payload.text_delta.stream_id), "%s",
                 "stale-response");
  fails +=
      expect(!h2_gizclaw_test_peer_event_matches_conversation(
                 &event, "demo-2", response_stream_id, transcript_stream_id,
                 assistant_stream_id, sizeof(response_stream_id)),
             "conversation rejects a different assistant stream after binding");

  fails +=
      expect(h2_gizclaw_test_replace_event_stream(client, NULL) == events,
             "test removes the borrowed Event handle before client deinit");
  return fails;
}

static int test_contact_rpc_call(void *user, h2_gizclaw_client_t *client,
                                 h2_gizclaw_rpc_method_t method,
                                 h2_gizclaw_rpc_bytes_t params_payload,
                                 h2_gizclaw_rpc_response_t *out_response) {
  test_contact_rpc_t *mock = user;
  ++mock->calls;
  mock->request_matches = method == mock->expected_method &&
                          params_payload.len == mock->expected_request_len &&
                          memcmp(params_payload.data, mock->expected_request,
                                 params_payload.len) == 0;
  out_response->has_error = mock->has_error;
  out_response->error_code = mock->error_code;
  if (mock->response_len == 0u)
    return H2_PAL_OK;
  const h2_pal_mem_api_t *allocator =
      h2_gizclaw_client_allocator_internal(client);
  out_response->result_payload =
      h2_pal_mem_alloc(allocator, mock->response_len);
  if (out_response->result_payload == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memcpy(out_response->result_payload, mock->response, mock->response_len);
  out_response->result_payload_len = mock->response_len;
  return H2_PAL_OK;
}

static int test_rpc_sequence_call(void *user, h2_gizclaw_client_t *client,
                                  h2_gizclaw_rpc_method_t method,
                                  h2_gizclaw_rpc_bytes_t params_payload,
                                  h2_gizclaw_rpc_response_t *out_response) {
  test_rpc_sequence_t *sequence = user;
  if (sequence->next_step >= sequence->step_count)
    return H2_PAL_ERR_INVALID_STATE;
  return test_contact_rpc_call(&sequence->steps[sequence->next_step++], client,
                               method, params_payload, out_response);
}

static int test_stable_registration_token_reuse(h2_gizclaw_client_t *client) {
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
  test_contact_rpc_t mock = {
      .expected_method = H2_GIZCLAW_RPC_SERVER_REGISTER,
      .expected_request = request,
      .expected_request_len = sizeof(request),
      .response = response,
      .response_len = response_len,
  };
  h2_gizclaw_test_set_rpc_call(test_contact_rpc_call, &mock);
  for (size_t attempt = 0u; attempt < 2u; ++attempt) {
    h2_gizclaw_registration_result_t registration = {0};
    fails += expect(
        h2_gizclaw_client_register(client, token, &registration) == H2_PAL_OK,
        "stable product token can establish each connection snapshot");
    fails +=
        expect(strcmp(registration.runtime_profile_name, "demo-test") == 0,
               "repeated registration preserves the product binding response");
  }
  fails += expect(mock.calls == 2 && mock.request_matches,
                  "stable product token is forwarded for every registration");
  h2_gizclaw_test_set_rpc_call(NULL, NULL);
  return fails;
}

static int
test_friend_group_message_projection_rpcs(h2_gizclaw_client_t *client) {
  int fails = 0;
  const h2_gizclaw_str_t group_name = {.data = "group-1", .len = 7u};
  const h2_gizclaw_str_t history_name = {.data = "history-1", .len = 9u};
  const uint8_t list_request[] = {
      0x12, 0x07, 'g', 'r', 'o', 'u', 'p', '-', '1', 0x18, 0x08,
  };
  uint8_t response[512];
  size_t response_len = 0u;
  fails += expect(test_encode_friend_group_message_list_response(
                      response, sizeof(response), false, &response_len),
                  "friend group message list response fixture encodes");
  test_contact_rpc_t mock = {
      .expected_method = H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_MESSAGES_LIST,
      .expected_request = list_request,
      .expected_request_len = sizeof(list_request),
      .response = response,
      .response_len = response_len,
  };
  h2_gizclaw_test_set_rpc_call(test_contact_rpc_call, &mock);
  h2_gizclaw_friend_group_message_page_t page = {0};
  fails += expect(h2_gizclaw_client_friend_group_messages_list(
                      client, group_name, (h2_gizclaw_str_t){0}, 8u, &page) ==
                      H2_PAL_OK,
                  "friend group message list accepts a history projection");
  fails += expect(
      mock.calls == 1 && mock.request_matches && page.count == 1u &&
          strcmp(page.items[0].friend_group_name, "group-1") == 0 &&
          strcmp(page.items[0].history_id, "history-1") == 0 &&
          strcmp(page.items[0].sender_peer_public_key, "peer-1") == 0 &&
          strcmp(page.items[0].name, "Nova") == 0 &&
          strcmp(page.items[0].text, "hello") == 0 &&
          page.items[0].type == H2_GIZCLAW_FRIEND_GROUP_MESSAGE_TYPE_AGENT &&
          page.items[0].audio_available,
      "friend group message list maps group and Workspace History identity");
  h2_gizclaw_friend_group_message_page_deinit(client, &page);

  response_len = 0u;
  fails += expect(test_encode_friend_group_message_list_response(
                      response, sizeof(response), true, &response_len),
                  "invalid UTF-8 message response fixture encodes");
  mock = (test_contact_rpc_t){
      .expected_method = H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_MESSAGES_LIST,
      .expected_request = list_request,
      .expected_request_len = sizeof(list_request),
      .response = response,
      .response_len = response_len,
  };
  fails += expect(h2_gizclaw_client_friend_group_messages_list(
                      client, group_name, (h2_gizclaw_str_t){0}, 8u, &page) ==
                      H2_PAL_ERR_FORMAT,
                  "friend group message list rejects invalid UTF-8 text");
  fails += expect(page.items == NULL && page.count == 0u,
                  "invalid message projection releases partial ownership");

  const uint8_t get_request[] = {
      0x0a, 0x07, 'g', 'r', 'o', 'u', 'p', '-', '1', 0x12,
      0x09, 'h',  'i', 's', 't', 'o', 'r', 'y', '-', '1',
  };
  response_len = 0u;
  fails += expect(test_encode_friend_group_message_get_response(
                      response, sizeof(response), &response_len),
                  "friend group message get response fixture encodes");
  mock = (test_contact_rpc_t){
      .expected_method = H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_MESSAGES_GET,
      .expected_request = get_request,
      .expected_request_len = sizeof(get_request),
      .response = response,
      .response_len = response_len,
  };
  h2_gizclaw_friend_group_message_t message = {0};
  fails += expect(h2_gizclaw_client_friend_group_message_get(
                      client, group_name, history_name, &message) == H2_PAL_OK,
                  "friend group message get accepts a history projection");
  fails += expect(
      mock.calls == 1 && mock.request_matches &&
          strcmp(message.friend_group_name, "group-1") == 0 &&
          strcmp(message.history_id, "history-1") == 0 &&
          message.audio_available,
      "friend group message get preserves the requested history identity");
  h2_gizclaw_friend_group_message_deinit(client, &message);
  h2_gizclaw_test_set_rpc_call(NULL, NULL);
  return fails;
}

static int test_contact_mutation_rpcs(h2_gizclaw_client_t *client) {
  int fails = 0;
  uint8_t response[128];
  size_t response_len = 0u;
  fails += expect(
      test_encode_contact_response(response, sizeof(response), &response_len),
      "contact mutation response fixture encodes");
  const uint8_t put_request[] = {
      0x0a, 0x05, 'A', 'l', 'i', 'c', 'e',  0x12, 0x09, 'c', 'o', 'n',
      't',  'a',  'c', 't', '-', '1', 0x1a, 0x0e, '+',  '8', '6', '1',
      '3',  '9',  '0', '0', '0', '0', '0',  '0',  '0',  '0',
  };
  test_contact_rpc_t mock = {
      .expected_method = H2_GIZCLAW_RPC_SERVER_CONTACT_PUT,
      .expected_request = put_request,
      .expected_request_len = sizeof(put_request),
      .response = response,
      .response_len = response_len,
  };
  h2_gizclaw_test_set_rpc_call(test_contact_rpc_call, &mock);
  h2_gizclaw_contact_t contact = {0};
  fails +=
      expect(h2_gizclaw_client_contact_put(
                 client, (h2_gizclaw_str_t){.data = "contact-1", .len = 9u},
                 (h2_gizclaw_str_t){.data = "Alice", .len = 5u},
                 (h2_gizclaw_str_t){.data = "+8613900000000", .len = 14u},
                 &contact) == H2_PAL_OK,
             "contact put accepts mocked RPC response");
  fails += expect(mock.calls == 1 && mock.request_matches &&
                      strcmp(contact.name, "contact-1") == 0 &&
                      strcmp(contact.display_name, "Alice") == 0 &&
                      strcmp(contact.phone_number, "+8613900000000") == 0 &&
                      strcmp(contact.created_at, "2026-07-27T00:00:00Z") == 0 &&
                      strcmp(contact.updated_at, "2026-07-27T00:01:00Z") == 0,
                  "contact put maps request and owns decoded response");
  h2_gizclaw_contact_deinit(client, &contact);
  fails += expect(contact.name == NULL && contact.display_name == NULL &&
                      contact.phone_number == NULL &&
                      contact.created_at == NULL && contact.updated_at == NULL,
                  "contact put response releases owned strings");

  const uint8_t delete_request[] = {
      0x0a, 0x09, 'c', 'o', 'n', 't', 'a', 'c', 't', '-', '1',
  };
  mock = (test_contact_rpc_t){
      .expected_method = H2_GIZCLAW_RPC_SERVER_CONTACT_GET,
      .expected_request = delete_request,
      .expected_request_len = sizeof(delete_request),
      .response = response,
      .response_len = response_len,
  };
  fails +=
      expect(h2_gizclaw_client_contact_get(
                 client, (h2_gizclaw_str_t){.data = "contact-1", .len = 9u},
                 &contact) == H2_PAL_OK,
             "contact get accepts mocked RPC response");
  fails += expect(mock.calls == 1 && mock.request_matches &&
                      strcmp(contact.name, "contact-1") == 0,
                  "contact get maps stable name and decodes response");
  h2_gizclaw_contact_deinit(client, &contact);

  mock = (test_contact_rpc_t){
      .expected_method = H2_GIZCLAW_RPC_SERVER_CONTACT_GET,
      .expected_request = delete_request,
      .expected_request_len = sizeof(delete_request),
      .has_error = true,
      .error_code = H2_GIZCLAW_RPC_ERROR_NOT_FOUND,
  };
  contact.name = (char *)0x1;
  fails +=
      expect(h2_gizclaw_client_contact_get(
                 client, (h2_gizclaw_str_t){.data = "contact-1", .len = 9u},
                 &contact) == H2_PAL_ERR_NOT_FOUND &&
                 contact.name == NULL,
             "contact get maps RPC not found and clears output");

  mock = (test_contact_rpc_t){
      .expected_method = H2_GIZCLAW_RPC_SERVER_CONTACT_DELETE,
      .expected_request = delete_request,
      .expected_request_len = sizeof(delete_request),
      .response = response,
      .response_len = response_len,
  };
  fails +=
      expect(h2_gizclaw_client_contact_delete(
                 client, (h2_gizclaw_str_t){.data = "contact-1", .len = 9u},
                 &contact) == H2_PAL_OK,
             "contact delete accepts mocked RPC response");
  fails += expect(mock.calls == 1 && mock.request_matches &&
                      strcmp(contact.name, "contact-1") == 0,
                  "contact delete maps request and decodes response");
  h2_gizclaw_contact_deinit(client, &contact);

  const uint8_t malformed_response[] = {0x0a, 0x01, 0xff};
  mock = (test_contact_rpc_t){
      .expected_method = H2_GIZCLAW_RPC_SERVER_CONTACT_DELETE,
      .expected_request = delete_request,
      .expected_request_len = sizeof(delete_request),
      .response = malformed_response,
      .response_len = sizeof(malformed_response),
  };
  fails +=
      expect(h2_gizclaw_client_contact_delete(
                 client, (h2_gizclaw_str_t){.data = "contact-1", .len = 9u},
                 &contact) == H2_PAL_ERR_FORMAT,
             "contact delete rejects malformed mocked response");
  fails +=
      expect(mock.calls == 1 && mock.request_matches && contact.name == NULL &&
                 contact.display_name == NULL && contact.phone_number == NULL &&
                 contact.created_at == NULL && contact.updated_at == NULL,
             "malformed contact response releases partial ownership");

  char oversized_name[H2_GIZCLAW_CONTACT_DISPLAY_NAME_MAX_BYTES + 1u];
  memset(oversized_name, 'A', sizeof(oversized_name));
  uint8_t oversized_response[512];
  size_t oversized_response_len = 0u;
  fails +=
      expect(test_encode_contact_response_with_name(
                 oversized_response, sizeof(oversized_response), oversized_name,
                 sizeof(oversized_name), &oversized_response_len),
             "oversized contact response fixture encodes");
  mock = (test_contact_rpc_t){
      .expected_method = H2_GIZCLAW_RPC_SERVER_CONTACT_GET,
      .expected_request = delete_request,
      .expected_request_len = sizeof(delete_request),
      .response = oversized_response,
      .response_len = oversized_response_len,
  };
  fails +=
      expect(h2_gizclaw_client_contact_get(
                 client, (h2_gizclaw_str_t){.data = "contact-1", .len = 9u},
                 &contact) == H2_PAL_ERR_FORMAT,
             "contact get rejects an oversized decoded field");
  fails +=
      expect(mock.calls == 1 && contact.name == NULL &&
                 contact.display_name == NULL && contact.phone_number == NULL &&
                 contact.created_at == NULL && contact.updated_at == NULL,
             "oversized contact response releases partial ownership");
  h2_gizclaw_test_set_rpc_call(NULL, NULL);
  return fails;
}

static int test_cleanup_mutation_rpcs(h2_gizclaw_client_t *client) {
  int fails = 0;
  const uint8_t workspace_get_request[] = {
      0x0a, 0x0b, 'w', 'o', 'r', 'k', 's', 'p', 'a', 'c', 'e', '-', '1',
  };
  const uint8_t workspace_put_request[] = {
      0x0a, 0x08, 0x22, 0x06, 0x0a, 0x04, 0x08, 0x01, 0x20, 0x02, 0x12, 0x0b,
      'w',  'o',  'r',  'k',  's',  'p',  'a',  'c',  'e',  '-',  '1',
  };
  uint8_t workspace_put_response[128];
  size_t workspace_put_response_len = 0u;
  fails += expect(test_encode_workspace_put_response(
                      workspace_put_response, sizeof(workspace_put_response),
                      &workspace_put_response_len),
                  "workspace put response fixture encodes");
  uint8_t workspace_get_response[128];
  size_t workspace_get_response_len = 0u;
  fails += expect(test_encode_workspace_get_response(
                      workspace_get_response, sizeof(workspace_get_response),
                      false, &workspace_get_response_len),
                  "workspace get response fixture encodes");
  test_contact_rpc_t workspace_steps[] = {
      {
          .expected_method = H2_GIZCLAW_RPC_SERVER_WORKSPACE_GET,
          .expected_request = workspace_get_request,
          .expected_request_len = sizeof(workspace_get_request),
          .response = workspace_get_response,
          .response_len = workspace_get_response_len,
      },
      {
          .expected_method = H2_GIZCLAW_RPC_SERVER_WORKSPACE_PUT,
          .expected_request = workspace_put_request,
          .expected_request_len = sizeof(workspace_put_request),
          .response = workspace_put_response,
          .response_len = workspace_put_response_len,
      },
  };
  test_rpc_sequence_t workspace_sequence = {
      .steps = workspace_steps,
      .step_count = sizeof(workspace_steps) / sizeof(workspace_steps[0]),
  };
  h2_gizclaw_test_set_rpc_call(test_rpc_sequence_call, &workspace_sequence);
  h2_gizclaw_workspace_t workspace = {0};
  fails +=
      expect(h2_gizclaw_client_workspace_set_input(
                 client, (h2_gizclaw_str_t){.data = "workspace-1", .len = 11u},
                 H2_GIZCLAW_WORKSPACE_INPUT_REALTIME, &workspace) == H2_PAL_OK,
             "workspace input update follows the existing parameter shape");
  fails += expect(
      workspace_sequence.next_step == workspace_sequence.step_count &&
          workspace_steps[0].calls == 1 && workspace_steps[0].request_matches &&
          workspace_steps[1].calls == 1 && workspace_steps[1].request_matches &&
          strcmp(workspace.name, "workspace-1") == 0 &&
          strcmp(workspace.workflow_name, "chat") == 0 && workspace.available,
      "workspace input update discovers parameters before PUT");
  h2_gizclaw_workspace_deinit(client, &workspace);

  uint8_t overridden_get_response[128];
  size_t overridden_get_response_len = 0u;
  fails += expect(test_encode_workspace_get_response(
                      overridden_get_response, sizeof(overridden_get_response),
                      true, &overridden_get_response_len),
                  "workspace override response fixture encodes");
  test_contact_rpc_t override_mock = {
      .expected_method = H2_GIZCLAW_RPC_SERVER_WORKSPACE_GET,
      .expected_request = workspace_get_request,
      .expected_request_len = sizeof(workspace_get_request),
      .response = overridden_get_response,
      .response_len = overridden_get_response_len,
  };
  h2_gizclaw_test_set_rpc_call(test_contact_rpc_call, &override_mock);
  fails +=
      expect(h2_gizclaw_client_workspace_set_input(
                 client, (h2_gizclaw_str_t){.data = "workspace-1", .len = 11u},
                 H2_GIZCLAW_WORKSPACE_INPUT_REALTIME,
                 &workspace) == H2_PAL_ERR_INVALID_STATE,
             "workspace input update refuses to discard parameter overrides");
  fails += expect(override_mock.calls == 1 && override_mock.request_matches &&
                      workspace.name == NULL,
                  "workspace override rejection stops before PUT");

  uint8_t unsupported_get_response[128];
  memcpy(unsupported_get_response, workspace_get_response,
         workspace_get_response_len);
  bool parameters_tag_replaced = false;
  for (size_t index = 0u; index + 3u < workspace_get_response_len; ++index) {
    if (unsupported_get_response[index] == 0x22u &&
        unsupported_get_response[index + 1u] == 0x06u &&
        unsupported_get_response[index + 2u] == 0x0au &&
        unsupported_get_response[index + 3u] == 0x04u) {
      unsupported_get_response[index + 2u] = 0x7au;
      parameters_tag_replaced = true;
      break;
    }
  }
  fails += expect(parameters_tag_replaced,
                  "workspace parameter fixture tag is replaced");
  test_contact_rpc_t unsupported_mock = {
      .expected_method = H2_GIZCLAW_RPC_SERVER_WORKSPACE_GET,
      .expected_request = workspace_get_request,
      .expected_request_len = sizeof(workspace_get_request),
      .response = unsupported_get_response,
      .response_len = workspace_get_response_len,
  };
  h2_gizclaw_test_set_rpc_call(test_contact_rpc_call, &unsupported_mock);
  workspace.name = (char *)0x1;
  fails += expect(h2_gizclaw_client_workspace_set_input(
                      client,
                      (h2_gizclaw_str_t){.data = "workspace-1", .len = 11u},
                      H2_GIZCLAW_WORKSPACE_INPUT_PUSH_TO_TALK,
                      &workspace) == H2_PAL_ERR_UNSUPPORTED &&
                      workspace.name == NULL,
                  "workspace input update rejects unsupported parameter types");
  fails += expect(unsupported_mock.calls == 1 &&
                      unsupported_mock.request_matches,
                  "unsupported Workspace parameters stop before PUT");

  uint8_t incomplete_get_response[128];
  memcpy(incomplete_get_response, workspace_get_response,
         workspace_get_response_len);
  bool input_field_replaced = false;
  for (size_t index = 0u; index + 7u < workspace_get_response_len; ++index) {
    if (incomplete_get_response[index] == 0x22u &&
        incomplete_get_response[index + 1u] == 0x06u &&
        incomplete_get_response[index + 2u] == 0x0au &&
        incomplete_get_response[index + 3u] == 0x04u &&
        incomplete_get_response[index + 4u] == 0x08u &&
        incomplete_get_response[index + 6u] == 0x20u) {
      incomplete_get_response[index + 6u] = 0x08u;
      input_field_replaced = true;
      break;
    }
  }
  fails += expect(input_field_replaced,
                  "workspace input fixture field is replaced");
  test_contact_rpc_t incomplete_mock = {
      .expected_method = H2_GIZCLAW_RPC_SERVER_WORKSPACE_GET,
      .expected_request = workspace_get_request,
      .expected_request_len = sizeof(workspace_get_request),
      .response = incomplete_get_response,
      .response_len = workspace_get_response_len,
  };
  h2_gizclaw_test_set_rpc_call(test_contact_rpc_call, &incomplete_mock);
  fails += expect(h2_gizclaw_client_workspace_set_input(
                      client,
                      (h2_gizclaw_str_t){.data = "workspace-1", .len = 11u},
                      H2_GIZCLAW_WORKSPACE_INPUT_REALTIME,
                      &workspace) == H2_PAL_ERR_INVALID_STATE,
                  "workspace input update rejects an incomplete parameter shape");
  fails += expect(incomplete_mock.calls == 1 &&
                      incomplete_mock.request_matches,
                  "incomplete Workspace parameters stop before PUT");

  const uint8_t duplicate_parameters_response[] = {
      0x0a, 0x0e, 0x22, 0x0c, 0x0a, 0x04, 0x08, 0x01,
      0x20, 0x01, 0x0a, 0x04, 0x08, 0x01, 0x20, 0x01,
  };
  test_contact_rpc_t duplicate_parameters_mock = {
      .expected_method = H2_GIZCLAW_RPC_SERVER_WORKSPACE_GET,
      .expected_request = workspace_get_request,
      .expected_request_len = sizeof(workspace_get_request),
      .response = duplicate_parameters_response,
      .response_len = sizeof(duplicate_parameters_response),
  };
  h2_gizclaw_test_set_rpc_call(test_contact_rpc_call,
                               &duplicate_parameters_mock);
  fails += expect(h2_gizclaw_client_workspace_set_input(
                      client,
                      (h2_gizclaw_str_t){.data = "workspace-1", .len = 11u},
                      H2_GIZCLAW_WORKSPACE_INPUT_REALTIME,
                      &workspace) == H2_PAL_ERR_INVALID_STATE,
                  "workspace input update rejects duplicate parameter oneofs");
  fails += expect(duplicate_parameters_mock.calls == 1 &&
                      duplicate_parameters_mock.request_matches,
                  "duplicate Workspace parameter oneofs stop before PUT");

  const uint8_t *workspace_request = workspace_get_request;
  uint8_t workspace_response[128];
  size_t workspace_response_len = 0u;
  fails += expect(test_encode_workspace_delete_response(
                      workspace_response, sizeof(workspace_response),
                      &workspace_response_len),
                  "workspace delete response fixture encodes");
  test_contact_rpc_t mock = {
      .expected_method = H2_GIZCLAW_RPC_SERVER_WORKSPACE_DELETE,
      .expected_request = workspace_request,
      .expected_request_len = sizeof(workspace_get_request),
      .response = workspace_response,
      .response_len = workspace_response_len,
  };
  h2_gizclaw_test_set_rpc_call(test_contact_rpc_call, &mock);
  fails +=
      expect(h2_gizclaw_client_workspace_delete(
                 client, (h2_gizclaw_str_t){.data = "workspace-1", .len = 11u},
                 &workspace) == H2_PAL_OK,
             "workspace delete accepts the typed mocked RPC response");
  fails += expect(mock.calls == 1 && mock.request_matches &&
                      strcmp(workspace.name, "workspace-1") == 0 &&
                      strcmp(workspace.workflow_name, "chat") == 0 &&
                      workspace.available,
                  "workspace delete encodes method 28 and owns its snapshot");
  h2_gizclaw_workspace_deinit(client, &workspace);

  const uint8_t pet_request[] = {
      0x0a, 0x07, 0x0a, 0x05, 'p', 'e', 't', '-', '1',
  };
  uint8_t pet_response[128];
  size_t pet_response_len = 0u;
  fails += expect(test_encode_pet_delete_response(
                      pet_response, sizeof(pet_response), &pet_response_len),
                  "pet delete response fixture encodes");
  mock = (test_contact_rpc_t){
      .expected_method = H2_GIZCLAW_RPC_SERVER_PET_DELETE,
      .expected_request = pet_request,
      .expected_request_len = sizeof(pet_request),
      .response = pet_response,
      .response_len = pet_response_len,
  };
  h2_gizclaw_pet_t pet = {0};
  fails += expect(h2_gizclaw_client_pet_delete(
                      client, (h2_gizclaw_str_t){.data = "pet-1", .len = 5u},
                      &pet) == H2_PAL_OK,
                  "pet delete accepts the typed mocked RPC response");
  fails += expect(mock.calls == 1 && mock.request_matches &&
                      strcmp(pet.name, "pet-1") == 0 &&
                      strcmp(pet.pet_def_name, "petdef-1") == 0,
                  "pet delete encodes method 69 and owns its snapshot");
  h2_gizclaw_pet_deinit(client, &pet);

  mock = (test_contact_rpc_t){
      .expected_method = H2_GIZCLAW_RPC_SERVER_WORKSPACE_DELETE,
      .expected_request = workspace_request,
      .expected_request_len = sizeof(workspace_request),
      .has_error = true,
      .error_code = H2_GIZCLAW_RPC_ERROR_NOT_FOUND,
  };
  workspace.name = (char *)0x1;
  fails +=
      expect(h2_gizclaw_client_workspace_delete(
                 client, (h2_gizclaw_str_t){.data = "workspace-1", .len = 11u},
                 &workspace) == H2_PAL_ERR_NOT_FOUND &&
                 workspace.name == NULL,
             "workspace delete maps RPC errors and clears output");

  const char invalid_utf8[] = {(char)0xc0, (char)0x80};
  fails +=
      expect(h2_gizclaw_client_workspace_delete(
                 client, (h2_gizclaw_str_t){.data = invalid_utf8, .len = 2u},
                 &workspace) == H2_PAL_ERR_INVALID_ARG &&
                 workspace.name == NULL,
             "workspace delete rejects invalid UTF-8 before RPC dispatch");

  mock = (test_contact_rpc_t){
      .expected_method = H2_GIZCLAW_RPC_SERVER_PET_DELETE,
      .expected_request = pet_request,
      .expected_request_len = sizeof(pet_request),
      .has_error = true,
      .error_code = H2_GIZCLAW_RPC_ERROR_NOT_FOUND,
  };
  pet.name = (char *)0x1;
  fails += expect(h2_gizclaw_client_pet_delete(
                      client, (h2_gizclaw_str_t){.data = "pet-1", .len = 5u},
                      &pet) == H2_PAL_ERR_NOT_FOUND &&
                      pet.name == NULL,
                  "pet delete maps RPC errors and clears output");

  mock = (test_contact_rpc_t){
      .expected_method = H2_GIZCLAW_RPC_SERVER_PET_DELETE,
      .expected_request = pet_request,
      .expected_request_len = sizeof(pet_request),
      .response = (const uint8_t *)"\x0a\x01\xff",
      .response_len = 3u,
  };
  pet.name = (char *)0x1;
  fails += expect(h2_gizclaw_client_pet_delete(
                      client, (h2_gizclaw_str_t){.data = "pet-1", .len = 5u},
                      &pet) == H2_PAL_ERR_FORMAT &&
                      pet.name == NULL,
                  "pet delete rejects malformed response and clears output");

  fails +=
      expect(h2_gizclaw_client_pet_delete(
                 client, (h2_gizclaw_str_t){.data = invalid_utf8, .len = 2u},
                 &pet) == H2_PAL_ERR_INVALID_ARG &&
                 pet.name == NULL,
             "pet delete rejects invalid UTF-8 before RPC dispatch");
  h2_gizclaw_test_set_rpc_call(NULL, NULL);
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
  fails += expect(h2_gizclaw_client_telemetry_send(client, &frame) == H2_PAL_OK,
                  "telemetry adapter submits a bounded frame");
  fails += expect(mock.calls == 1 && mock.sequence == 7u &&
                      mock.observed_at_unix_ms == 1785110400000ll &&
                      mock.observation_count == 4u && mock.mapped,
                  "telemetry adapter maps all four observation kinds");

  observations[0].value.battery.percent = NAN;
  fails += expect(h2_gizclaw_client_telemetry_send(client, &frame) ==
                          H2_PAL_ERR_INVALID_ARG &&
                      mock.calls == 1,
                  "telemetry rejects fabricated non-finite values before send");
  observations[0].value.battery.percent = 72.5;

  mock.result = GZC_ERR_TIMEOUT;
  fails += expect(h2_gizclaw_client_telemetry_send(client, &frame) ==
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
                                      int timeout_ms) {
  (void)peer;
  test_poll_calls++;
  test_last_poll_timeout_ms = timeout_ms;
  if (test_poll_result != H2_PAL_ERR_WOULD_BLOCK) {
    test_monotonic_ms += timeout_ms > 0 ? (uint64_t)timeout_ms : 1u;
  }
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

static h2_pal_result_t
test_webrtc_peer_create(void *user, const h2_pal_webrtc_callbacks_t *callbacks,
                        h2_pal_webrtc_peer_t **out_peer) {
  (void)user;
  assert(callbacks != NULL);
  assert(callbacks->on_opus_frame == NULL);
  *out_peer = (h2_pal_webrtc_peer_t *)0x2;
  return H2_PAL_OK;
}

static h2_pal_result_t test_peer_set_media_track(h2_pal_webrtc_peer_t *peer,
                                                 h2_pal_webrtc_track_t *track) {
  assert(peer == (h2_pal_webrtc_peer_t *)0x2);
  assert(track == (h2_pal_webrtc_track_t *)0x4);
  ++test_media_track_bind_calls;
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
  fails += expect(H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_MESSAGES_AUDIO_GET == 95,
                  "friend group message audio get wire method remains 95");

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
      .peer_set_media_track = test_peer_set_media_track,
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
  config.webrtc_media_track = (h2_pal_webrtc_track_t *)0x4;
  h2_gizclaw_client_t *track_client = NULL;
  fails += expect(h2_gizclaw_client_init(&config, &track_client) == H2_PAL_OK,
                  "track-mode init accepts an opaque provider track");
  h2_pal_webrtc_peer_t *track_peer = NULL;
  fails +=
      expect(h2_gizclaw_test_peer_create(track_client, &track_peer) == GZC_OK &&
                 track_peer == (h2_pal_webrtc_peer_t *)0x2 &&
                 test_media_track_bind_calls == 1u,
             "GizClaw binds the opaque track before offer setup");
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
          offset == sizeof(payload) && !blocked && test_send_calls == 2 &&
          test_poll_calls == 1 && test_last_poll_timeout_ms > 0,
      "adapter waits for peer progress before retrying a full PAL queue");
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
          offset == sizeof(payload) && !blocked && test_send_calls == 2 &&
          test_poll_calls == 1 && test_sleep_calls == 1 &&
          test_last_sleep_ms == 10u && test_warn_logs == 1 &&
          strstr(test_last_log_message, "rc=-9 backoff_ms=10") != NULL,
      "adapter warns and backs off when peer poll unexpectedly would block");
  test_poll_result = H2_PAL_OK;
  test_send_calls = 0;
  test_poll_calls = 0;
  test_send_would_block_count = 3000;
  offset = 0u;
  blocked = false;
  fails += expect(
      h2_gizclaw_test_try_write_bytes(client, channel, payload, sizeof(payload),
                                      &offset, &blocked) == GZC_ERR_TIMEOUT &&
          offset == 0u && test_poll_calls == 40 &&
          test_last_poll_timeout_ms == 50,
      "adapter bounds persistent PAL backpressure by the independent write "
      "timeout");
  test_send_calls = 0;
  test_poll_calls = 0;
  test_cancel_requested = true;
  offset = 0u;
  blocked = false;
  fails += expect(
      h2_gizclaw_test_try_write_bytes(client, channel, payload, sizeof(payload),
                                      &offset, &blocked) == GZC_ERR_CLOSED &&
          offset == 0u && test_send_calls == 1 && test_poll_calls == 0,
      "adapter stops a blocked write promptly when cancellation is requested");
  test_cancel_requested = false;
  test_send_calls = 0;
  test_poll_calls = 0;
  test_poll_result = H2_PAL_ERR_CLOSED;
  offset = 0u;
  blocked = false;
  fails += expect(
      h2_gizclaw_test_try_write_bytes(client, channel, payload, sizeof(payload),
                                      &offset, &blocked) == GZC_ERR_CLOSED &&
          offset == 0u && test_send_calls == 1 && test_poll_calls == 1,
      "adapter preserves peer closure while draining backpressure");
  test_poll_result = H2_PAL_ERR_IO;
  test_send_calls = 0;
  test_poll_calls = 0;
  offset = 0u;
  blocked = false;
  fails += expect(
      h2_gizclaw_test_try_write_bytes(client, channel, payload, sizeof(payload),
                                      &offset, &blocked) == GZC_ERR_WEBRTC &&
          offset == 0u && test_send_calls == 1 && test_poll_calls == 1,
      "adapter stops on a terminal peer poll failure");
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
  fails += test_contact_mutation_rpcs(client);
  fails += test_cleanup_mutation_rpcs(client);
  fails += test_friend_group_message_projection_rpcs(client);
  fails += test_message_audio_stream(client);
  fails += test_stable_registration_token_reuse(client);
  fails += test_telemetry_adapter(client);
  fails += test_speed_test_adapter(client);
  fails += test_conversation_event_lease(client);
  fails += test_closed_poll_mapping(&config);
  fails += test_event_failures_poison_client(client, &config);
  h2_gizclaw_client_deinit(client);
  fails += expect(h2_gizclaw_client_connect(NULL) == H2_PAL_ERR_INVALID_ARG,
                  "connect rejects null client");
  fails += expect(h2_gizclaw_client_poll(NULL, 0) == H2_PAL_ERR_INVALID_ARG,
                  "poll rejects null client");
  fails += expect(h2_gizclaw_client_ping(NULL) == H2_PAL_ERR_INVALID_ARG,
                  "ping rejects null client");
  h2_gizclaw_ping_result_t ping_result = {
      .round_trip_ms = UINT64_MAX,
      .server_time_ms = INT64_MAX,
  };
  fails += expect(h2_gizclaw_client_ping_measure(NULL, &ping_result) ==
                      H2_PAL_ERR_INVALID_ARG,
                  "ping measure rejects null client");
  fails +=
      expect(ping_result.round_trip_ms == 0u && ping_result.server_time_ms == 0,
             "failed ping measure clears output");
  fails += expect(h2_gizclaw_client_speedtest(NULL) == H2_PAL_ERR_INVALID_ARG,
                  "speedtest rejects null client");
  fails += expect(h2_gizclaw_client_delete_peer(NULL) == H2_PAL_ERR_INVALID_ARG,
                  "peer delete rejects null client");
  h2_gizclaw_speedtest_result_t speed_result = {
      .upload_bytes = UINT64_MAX,
      .download_bytes = UINT64_MAX,
      .upload_elapsed_ms = UINT64_MAX,
      .elapsed_ms = UINT64_MAX,
      .upload_bits_per_second = UINT64_MAX,
      .download_bits_per_second = UINT64_MAX,
  };
  fails +=
      expect(h2_gizclaw_client_speedtest_measure(
                 NULL, 1024u, 1024u, &speed_result) == H2_PAL_ERR_INVALID_ARG,
             "full-duplex speedtest rejects null client");
  fails += expect(speed_result.upload_bytes == 0u &&
                      speed_result.download_bytes == 0u &&
                      speed_result.upload_elapsed_ms == 0u &&
                      speed_result.elapsed_ms == 0u &&
                      speed_result.upload_bits_per_second == 0u &&
                      speed_result.download_bits_per_second == 0u,
                  "failed full-duplex speedtest clears output");
  speed_result = (h2_gizclaw_speedtest_result_t){
      .upload_bytes = UINT64_MAX,
      .download_bytes = UINT64_MAX,
      .upload_elapsed_ms = UINT64_MAX,
      .elapsed_ms = UINT64_MAX,
      .upload_bits_per_second = UINT64_MAX,
      .download_bits_per_second = UINT64_MAX,
  };
  fails += expect(h2_gizclaw_client_speedtest_download(
                      NULL, 1024u, &speed_result) == H2_PAL_ERR_INVALID_ARG,
                  "speedtest download rejects null client");
  fails += expect(speed_result.upload_bytes == 0u &&
                      speed_result.download_bytes == 0u &&
                      speed_result.upload_elapsed_ms == 0u &&
                      speed_result.elapsed_ms == 0u &&
                      speed_result.upload_bits_per_second == 0u &&
                      speed_result.download_bits_per_second == 0u,
                  "failed speedtest download clears output");
  h2_gizclaw_rpc_response_t rpc_response = {0};
  fails += expect(
      h2_gizclaw_client_rpc_call(NULL, 1, (h2_gizclaw_rpc_bytes_t){0},
                                 &rpc_response) == H2_PAL_ERR_INVALID_ARG,
      "generic rpc rejects null client");
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
  fails += expect(
      h2_gizclaw_client_rpc_call_stream(NULL, 1, (h2_gizclaw_rpc_bytes_t){0},
                                        NULL, NULL) == H2_PAL_ERR_INVALID_ARG,
      "stream rpc rejects null client and callback");
  fails += expect(h2_gizclaw_client_close(NULL) == H2_PAL_ERR_INVALID_ARG,
                  "close rejects null client");
  h2_gizclaw_registration_result_t registration = {0};
  fails += expect(h2_gizclaw_client_register(NULL, "token", &registration) ==
                      H2_PAL_ERR_INVALID_ARG,
                  "registration rejects null client");
  fails += expect(registration.runtime_profile_name[0] == '\0',
                  "failed registration clears output");
  h2_gizclaw_points_account_t points_account = {0};
  h2_gizclaw_points_transaction_page_t points_page = {0};
  fails += expect(h2_gizclaw_client_points_get(NULL, &points_account) ==
                      H2_PAL_ERR_INVALID_ARG,
                  "points get rejects null client");
  fails += expect(h2_gizclaw_client_points_transactions_list(
                      NULL, (h2_gizclaw_str_t){0}, 8u, &points_page) ==
                      H2_PAL_ERR_INVALID_ARG,
                  "points list rejects null client");
  h2_gizclaw_profile_t profile = {0};
  h2_gizclaw_friend_group_page_t friend_groups = {0};
  fails += expect(h2_gizclaw_client_friend_groups_list(
                      NULL, (h2_gizclaw_str_t){0}, 8u, &friend_groups) ==
                      H2_PAL_ERR_INVALID_ARG,
                  "friend group list rejects null client");
  fails += expect(h2_gizclaw_client_profile_get(NULL, &profile) ==
                      H2_PAL_ERR_INVALID_ARG,
                  "profile get rejects null client");
  fails += expect(h2_gizclaw_client_profile_put_emoji(
                      NULL, (h2_gizclaw_str_t){.data = "🤖", .len = 4u},
                      &profile) == H2_PAL_ERR_INVALID_ARG,
                  "profile put rejects null client");
  h2_gizclaw_pet_t pet = {0};
  const h2_gizclaw_pet_adopt_options_t adopt = {
      .name = {.data = "pet-test-1", .len = 10u},
      .display_name = {.data = "Test Pet", .len = 8u},
  };
  const h2_gizclaw_pet_drive_options_t empty_drive = {
      .pet_name = {.data = "pet-test-1", .len = 10u},
      .idempotency_key = {.data = "drive-test-1", .len = 12u},
  };
  fails += expect(h2_gizclaw_client_pet_get(NULL, empty_drive.pet_name, &pet) ==
                      H2_PAL_ERR_INVALID_ARG,
                  "pet get rejects null client");
  fails += expect(h2_gizclaw_client_pet_adopt(NULL, &adopt, &pet) ==
                      H2_PAL_ERR_INVALID_ARG,
                  "pet adopt rejects null client");
  fails += expect(h2_gizclaw_client_pet_adopt((h2_gizclaw_client_t *)0x1, NULL,
                                              &pet) == H2_PAL_ERR_INVALID_ARG,
                  "pet adopt rejects null options");
  fails += expect(h2_gizclaw_client_pet_drive(NULL, &empty_drive, &pet) ==
                      H2_PAL_ERR_INVALID_ARG,
                  "pet drive rejects null client");
  h2_gizclaw_pet_actions_t pet_actions = {0};
  fails += expect(h2_gizclaw_client_pet_actions_get(NULL, empty_drive.pet_name,
                                                    &pet_actions) ==
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
  fails += expect(h2_gizclaw_client_pet_drive((h2_gizclaw_client_t *)0x1,
                                              &invalid_mixed_drive,
                                              &pet) == H2_PAL_ERR_INVALID_ARG,
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
  h2_gizclaw_speech_upload_t *speech_upload = (h2_gizclaw_speech_upload_t *)0x1;
  fails += expect(h2_gizclaw_client_speech_transcribe_open(
                      NULL, NULL, &speech_upload) == H2_PAL_ERR_INVALID_ARG &&
                      speech_upload == (h2_gizclaw_speech_upload_t *)0x1,
                  "speech open rejects invalid arguments");
  fails += expect(h2_gizclaw_speech_transcribe_write(NULL, NULL, 0u) ==
                      H2_PAL_ERR_INVALID_ARG,
                  "speech write rejects invalid arguments");
  char transcript[8] = "dirty";
  size_t transcript_len = 99u;
  fails += expect(h2_gizclaw_speech_transcribe_finish(
                      NULL, transcript, sizeof(transcript), &transcript_len) ==
                          H2_PAL_ERR_INVALID_ARG &&
                      strcmp(transcript, "dirty") == 0 && transcript_len == 99u,
                  "speech finish rejects invalid arguments");
  fails += expect(h2_gizclaw_client_speech_extract_open(
                      NULL, NULL, &speech_upload) == H2_PAL_ERR_INVALID_ARG &&
                      speech_upload == (h2_gizclaw_speech_upload_t *)0x1,
                  "speech extraction open rejects invalid arguments");
  fails += expect(h2_gizclaw_speech_extract_write(NULL, NULL, 0u) ==
                      H2_PAL_ERR_INVALID_ARG,
                  "speech extraction write rejects invalid arguments");
  fails += expect(h2_gizclaw_speech_extract_finish(NULL, NULL, NULL) ==
                      H2_PAL_ERR_INVALID_ARG,
                  "speech extraction finish rejects invalid arguments");
  h2_gizclaw_contact_page_t contact_page = {0};
  h2_gizclaw_contact_t contact = {0};
  fails += expect(h2_gizclaw_client_contacts_list(NULL, (h2_gizclaw_str_t){0},
                                                  1u, &contact_page) ==
                      H2_PAL_ERR_INVALID_ARG,
                  "contact list rejects invalid arguments");
  fails += expect(h2_gizclaw_client_contact_create(
                      NULL, (h2_gizclaw_str_t){.data = "contact-1", .len = 9u},
                      (h2_gizclaw_str_t){.data = "A", .len = 1u},
                      (h2_gizclaw_str_t){.data = "1", .len = 1u},
                      &contact) == H2_PAL_ERR_INVALID_ARG,
                  "contact create rejects invalid arguments");
  fails += expect(h2_gizclaw_client_contact_get(
                      NULL, (h2_gizclaw_str_t){.data = "id", .len = 2u},
                      &contact) == H2_PAL_ERR_INVALID_ARG,
                  "contact get rejects invalid arguments");
  char oversized_contact_name[H2_GIZCLAW_CONTACT_NAME_MAX_BYTES + 1u];
  memset(oversized_contact_name, 'a', sizeof(oversized_contact_name));
  fails += expect(h2_gizclaw_client_contact_get(
                      (h2_gizclaw_client_t *)0x1,
                      (h2_gizclaw_str_t){.data = oversized_contact_name,
                                         .len = sizeof(oversized_contact_name)},
                      &contact) == H2_PAL_ERR_INVALID_ARG,
                  "contact get rejects an oversized name before allocation");
  fails += expect(h2_gizclaw_client_contact_put(
                      NULL, (h2_gizclaw_str_t){.data = "id", .len = 2u},
                      (h2_gizclaw_str_t){.data = "A", .len = 1u},
                      (h2_gizclaw_str_t){.data = "1", .len = 1u},
                      &contact) == H2_PAL_ERR_INVALID_ARG,
                  "contact put rejects invalid arguments");
  fails += expect(h2_gizclaw_client_contact_delete(
                      NULL, (h2_gizclaw_str_t){.data = "id", .len = 2u},
                      &contact) == H2_PAL_ERR_INVALID_ARG,
                  "contact delete rejects invalid arguments");
  h2_gizclaw_friend_t friend_value = {0};
  h2_gizclaw_friend_page_t friend_page = {0};
  h2_gizclaw_invite_token_t invite_token = {0};
  h2_gizclaw_friend_group_t friend_group = {0};
  h2_gizclaw_friend_group_member_t member = {0};
  h2_gizclaw_friend_group_member_page_t member_page = {0};
  h2_gizclaw_friend_group_message_t message = {0};
  h2_gizclaw_friend_group_message_page_t message_page = {0};
  const h2_gizclaw_str_t id = {.data = "id", .len = 2u};
  fails += expect(h2_gizclaw_client_friends_list(NULL, (h2_gizclaw_str_t){0},
                                                 8u, &friend_page) ==
                      H2_PAL_ERR_INVALID_ARG,
                  "friend list rejects invalid arguments");
  fails += expect(h2_gizclaw_client_friend_info_get(NULL, id, &friend_value) ==
                      H2_PAL_ERR_INVALID_ARG,
                  "friend info rejects invalid arguments");
  fails += expect(h2_gizclaw_client_friend_add(NULL, id, &friend_value) ==
                          H2_PAL_ERR_INVALID_ARG &&
                      h2_gizclaw_client_friend_delete(
                          NULL, id, &friend_value) == H2_PAL_ERR_INVALID_ARG,
                  "friend mutations reject invalid arguments");
  fails += expect(h2_gizclaw_client_friend_invite_token_create(
                      NULL, &invite_token) == H2_PAL_ERR_INVALID_ARG &&
                      h2_gizclaw_client_friend_invite_token_get(
                          NULL, &invite_token) == H2_PAL_ERR_INVALID_ARG &&
                      h2_gizclaw_client_friend_invite_token_clear(NULL) ==
                          H2_PAL_ERR_INVALID_ARG,
                  "friend token operations reject invalid arguments");
  fails += expect(
      h2_gizclaw_client_friend_group_get(NULL, id, &friend_group) ==
              H2_PAL_ERR_INVALID_ARG &&
          h2_gizclaw_client_friend_group_create(
              NULL, id, id, id, &friend_group) == H2_PAL_ERR_INVALID_ARG &&
          h2_gizclaw_client_friend_group_put(NULL, id, id, id, &friend_group) ==
              H2_PAL_ERR_INVALID_ARG &&
          h2_gizclaw_client_friend_group_delete(NULL, id, &friend_group) ==
              H2_PAL_ERR_INVALID_ARG &&
          h2_gizclaw_client_friend_group_join(NULL, id, id, &friend_group) ==
              H2_PAL_ERR_INVALID_ARG,
      "friend group mutations reject invalid arguments");
  char oversized_group_name[H2_GIZCLAW_FRIEND_GROUP_NAME_MAX_BYTES + 1u];
  memset(oversized_group_name, 'g', sizeof(oversized_group_name));
  const h2_gizclaw_str_t oversized_group = {
      .data = oversized_group_name, .len = sizeof(oversized_group_name)};
  fails += expect(
      h2_gizclaw_client_friend_group_get((h2_gizclaw_client_t *)0x1,
                                         oversized_group, &friend_group) ==
              H2_PAL_ERR_INVALID_ARG &&
          h2_gizclaw_client_friend_group_create(
              (h2_gizclaw_client_t *)0x1, oversized_group, id, id,
              &friend_group) == H2_PAL_ERR_INVALID_ARG &&
          h2_gizclaw_client_friend_group_join((h2_gizclaw_client_t *)0x1, id,
                                              oversized_group, &friend_group) ==
              H2_PAL_ERR_INVALID_ARG,
      "friend group operations reject oversized names before allocation");
  fails += expect(h2_gizclaw_client_friend_group_invite_token_create(
                      NULL, id, &invite_token) == H2_PAL_ERR_INVALID_ARG &&
                      h2_gizclaw_client_friend_group_invite_token_get(
                          NULL, id, &invite_token) == H2_PAL_ERR_INVALID_ARG &&
                      h2_gizclaw_client_friend_group_invite_token_clear(
                          NULL, id) == H2_PAL_ERR_INVALID_ARG,
                  "friend group token operations reject invalid arguments");
  fails += expect(h2_gizclaw_client_friend_group_members_list(
                      NULL, id, (h2_gizclaw_str_t){0}, 8u, &member_page) ==
                          H2_PAL_ERR_INVALID_ARG &&
                      h2_gizclaw_client_friend_group_member_put(
                          NULL, id, id, H2_GIZCLAW_FRIEND_GROUP_ROLE_MEMBER,
                          &member) == H2_PAL_ERR_INVALID_ARG &&
                      h2_gizclaw_client_friend_group_member_delete(
                          NULL, id, id, &member) == H2_PAL_ERR_INVALID_ARG,
                  "friend group member operations reject invalid arguments");
  fails +=
      expect(h2_gizclaw_client_friend_group_messages_list(
                 NULL, id, (h2_gizclaw_str_t){0}, 8u, &message_page) ==
                     H2_PAL_ERR_INVALID_ARG &&
                 h2_gizclaw_client_friend_group_message_get(
                     NULL, id, id, &message) == H2_PAL_ERR_INVALID_ARG &&
                 h2_gizclaw_client_friend_group_message_audio_get(
                     NULL, id, id, NULL, NULL, NULL) == H2_PAL_ERR_INVALID_ARG,
             "friend group message operations reject invalid arguments");
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
  fails += h2_gizclaw_conversation_audio_tests();

  if (fails == 0) {
    printf("PASS h2_gizclaw_client\n");
  }
  return fails == 0 ? 0 : 1;
}
