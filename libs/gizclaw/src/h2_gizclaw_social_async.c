#include "h2_gizclaw_social_internal.h"
#include "h2_gizclaw_service_internal.h"
#include "h2_gizclaw_internal.h"

#include "payload/social.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include <stdatomic.h>
#include <string.h>

#define H2_GIZCLAW_SOCIAL_TEXT_MAX_BYTES 4096u

struct h2_gizclaw_social_request {
  const h2_pal_mem_api_t *allocator;
  h2_gizclaw_operation_t *operation;
  const h2_gizclaw_cancel_token_t *cancel_token;
  h2_gizclaw_social_request_kind_t kind;
  h2_gizclaw_social_completion_fn completion;
  void *completion_user;
  h2_gizclaw_social_result_t result;
  h2_gizclaw_operation_result_t operation_result;
  h2_gizclaw_rpc_request_t *rpc_request;
  h2_gizclaw_rpc_response_t rpc_response;
  h2_gizclaw_async_stream_t *stream;
  char *text[3];
  size_t limit;
  h2_gizclaw_friend_group_role_t role;
  h2_gizclaw_friend_group_message_audio_write_fn write;
  void *write_user;
  atomic_bool terminal;
  bool metadata_received;
  bool eos_received;
};

static h2_gizclaw_str_t social_span(const char *text) {
  return (h2_gizclaw_str_t){.data = text,
                            .len = text == NULL ? 0u : strlen(text)};
}

static bool social_text_valid(h2_gizclaw_str_t text, bool required) {
  return (!required || text.len > 0u) &&
         text.len <= H2_GIZCLAW_SOCIAL_TEXT_MAX_BYTES &&
         (text.len == 0u || text.data != NULL);
}

static char *social_copy(const h2_pal_mem_api_t *allocator,
                         h2_gizclaw_str_t text) {
  if (!social_text_valid(text, false))
    return NULL;
  char *copy = h2_pal_mem_alloc(allocator, text.len + 1u);
  if (copy == NULL)
    return NULL;
  if (text.len > 0u)
    memcpy(copy, text.data, text.len);
  copy[text.len] = '\0';
  return copy;
}

static void social_contact_clear(const h2_pal_mem_api_t *allocator,
                                 h2_gizclaw_contact_t *value) {
  h2_pal_mem_free(allocator, value->name);
  h2_pal_mem_free(allocator, value->display_name);
  h2_pal_mem_free(allocator, value->phone_number);
  h2_pal_mem_free(allocator, value->created_at);
  h2_pal_mem_free(allocator, value->updated_at);
  memset(value, 0, sizeof(*value));
}

static void social_group_clear(const h2_pal_mem_api_t *allocator,
                               h2_gizclaw_friend_group_t *value) {
  h2_pal_mem_free(allocator, value->name);
  h2_pal_mem_free(allocator, value->display_name);
  h2_pal_mem_free(allocator, value->description);
  h2_pal_mem_free(allocator, value->workspace_name);
  memset(value, 0, sizeof(*value));
}

static void social_friend_clear(const h2_pal_mem_api_t *allocator,
                                h2_gizclaw_friend_t *value) {
  h2_pal_mem_free(allocator, value->id);
  h2_pal_mem_free(allocator, value->peer_public_key);
  h2_pal_mem_free(allocator, value->workspace_name);
  h2_pal_mem_free(allocator, value->created_at);
  h2_pal_mem_free(allocator, value->updated_at);
  h2_pal_mem_free(allocator, value->name);
  h2_pal_mem_free(allocator, value->emoji);
  memset(value, 0, sizeof(*value));
}

static void social_member_clear(const h2_pal_mem_api_t *allocator,
                                h2_gizclaw_friend_group_member_t *value) {
  h2_pal_mem_free(allocator, value->id);
  h2_pal_mem_free(allocator, value->friend_group_name);
  h2_pal_mem_free(allocator, value->peer_public_key);
  h2_pal_mem_free(allocator, value->created_at);
  h2_pal_mem_free(allocator, value->updated_at);
  memset(value, 0, sizeof(*value));
}

static void social_message_clear(const h2_pal_mem_api_t *allocator,
                                 h2_gizclaw_friend_group_message_t *value) {
  h2_pal_mem_free(allocator, value->history_id);
  h2_pal_mem_free(allocator, value->friend_group_name);
  h2_pal_mem_free(allocator, value->sender_peer_public_key);
  h2_pal_mem_free(allocator, value->created_at);
  h2_pal_mem_free(allocator, value->expires_at);
  h2_pal_mem_free(allocator, value->name);
  h2_pal_mem_free(allocator, value->text);
  memset(value, 0, sizeof(*value));
}

static void social_result_clear(h2_gizclaw_social_request_t *request) {
  switch (request->kind) {
  case H2_GIZCLAW_SOCIAL_CONTACTS_LIST:
    for (size_t index = 0u; index < request->result.value.contact_page.count;
         ++index)
      social_contact_clear(request->allocator,
                           &request->result.value.contact_page.items[index]);
    h2_pal_mem_free(request->allocator,
                    request->result.value.contact_page.items);
    h2_pal_mem_free(request->allocator,
                    request->result.value.contact_page.next_cursor);
    memset(&request->result.value.contact_page, 0,
           sizeof(request->result.value.contact_page));
    break;
  case H2_GIZCLAW_SOCIAL_CONTACT_GET:
  case H2_GIZCLAW_SOCIAL_CONTACT_CREATE:
  case H2_GIZCLAW_SOCIAL_CONTACT_PUT:
  case H2_GIZCLAW_SOCIAL_CONTACT_DELETE:
    social_contact_clear(request->allocator, &request->result.value.contact);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUPS_LIST:
    for (size_t index = 0u;
         index < request->result.value.friend_group_page.count; ++index)
      social_group_clear(request->allocator,
                         &request->result.value.friend_group_page.items[index]);
    h2_pal_mem_free(request->allocator,
                    request->result.value.friend_group_page.items);
    h2_pal_mem_free(request->allocator,
                    request->result.value.friend_group_page.next_cursor);
    memset(&request->result.value.friend_group_page, 0,
           sizeof(request->result.value.friend_group_page));
    break;
  case H2_GIZCLAW_SOCIAL_FRIENDS_LIST:
    for (size_t index = 0u; index < request->result.value.friend_page.count;
         ++index)
      social_friend_clear(request->allocator,
                          &request->result.value.friend_page.items[index]);
    h2_pal_mem_free(request->allocator,
                    request->result.value.friend_page.items);
    h2_pal_mem_free(request->allocator,
                    request->result.value.friend_page.next_cursor);
    memset(&request->result.value.friend_page, 0,
           sizeof(request->result.value.friend_page));
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_INFO_GET:
  case H2_GIZCLAW_SOCIAL_FRIEND_ADD:
  case H2_GIZCLAW_SOCIAL_FRIEND_DELETE:
    social_friend_clear(request->allocator,
                        &request->result.value.friend_value);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_INVITE_TOKEN_GET:
  case H2_GIZCLAW_SOCIAL_FRIEND_INVITE_TOKEN_CREATE:
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_INVITE_TOKEN_GET:
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_INVITE_TOKEN_CREATE:
    h2_pal_mem_free(request->allocator,
                    request->result.value.invite_token.value);
    h2_pal_mem_free(request->allocator,
                    request->result.value.invite_token.expires_at);
    memset(&request->result.value.invite_token, 0,
           sizeof(request->result.value.invite_token));
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_GET:
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_CREATE:
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_PUT:
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_DELETE:
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_JOIN:
    social_group_clear(request->allocator, &request->result.value.friend_group);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MEMBERS_LIST:
    for (size_t index = 0u; index < request->result.value.member_page.count;
         ++index)
      social_member_clear(request->allocator,
                          &request->result.value.member_page.items[index]);
    h2_pal_mem_free(request->allocator,
                    request->result.value.member_page.items);
    h2_pal_mem_free(request->allocator,
                    request->result.value.member_page.next_cursor);
    memset(&request->result.value.member_page, 0,
           sizeof(request->result.value.member_page));
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MEMBER_PUT:
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MEMBER_DELETE:
    social_member_clear(request->allocator, &request->result.value.member);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MESSAGES_LIST:
    for (size_t index = 0u; index < request->result.value.message_page.count;
         ++index)
      social_message_clear(request->allocator,
                           &request->result.value.message_page.items[index]);
    h2_pal_mem_free(request->allocator,
                    request->result.value.message_page.items);
    h2_pal_mem_free(request->allocator,
                    request->result.value.message_page.next_cursor);
    memset(&request->result.value.message_page, 0,
           sizeof(request->result.value.message_page));
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MESSAGE_GET:
    social_message_clear(request->allocator, &request->result.value.message);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MESSAGE_AUDIO_DOWNLOAD:
    h2_pal_mem_free(request->allocator,
                    request->result.value.message_audio.friend_group_name);
    h2_pal_mem_free(request->allocator,
                    request->result.value.message_audio.history_id);
    h2_pal_mem_free(request->allocator,
                    request->result.value.message_audio.mime_type);
    memset(&request->result.value.message_audio, 0,
           sizeof(request->result.value.message_audio));
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_INVITE_TOKEN_CLEAR:
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_INVITE_TOKEN_CLEAR:
    break;
  }
}

typedef struct social_write_dispatch {
  h2_gizclaw_social_request_t *request;
  const uint8_t *data;
  size_t len;
} social_write_dispatch_t;

static h2_pal_result_t social_dispatch_write(void *user) {
  social_write_dispatch_t *dispatch = user;
  return (h2_pal_result_t)dispatch->request->write(
      dispatch->request->write_user, dispatch->data, dispatch->len);
}

static int social_write(void *user, const uint8_t *data, size_t len) {
  h2_gizclaw_social_request_t *request = user;
  social_write_dispatch_t dispatch = {
      .request = request, .data = data, .len = len};
  return (int)h2_gizclaw_operation_dispatch_call(
      request->cancel_token, social_dispatch_write, &dispatch);
}

static bool social_copy_fixed(char *out, size_t capacity,
                              h2_gizclaw_str_t value) {
  if (out == NULL || value.data == NULL || value.len == 0u ||
      value.len >= capacity)
    return false;
  memcpy(out, value.data, value.len);
  out[value.len] = '\0';
  return true;
}

static h2_pal_result_t social_audio_event(
    void *user, h2_gizclaw_async_stream_t *stream,
    const h2_gizclaw_rpc_stream_event_t *event) {
  (void)stream;
  h2_gizclaw_social_request_t *request = user;
  h2_gizclaw_friend_group_message_audio_info_t *info =
      &request->result.value.message_audio;
  if (event == NULL || request->eos_received)
    return H2_PAL_ERR_FORMAT;
  if (event->has_error) {
    return event->error_code == H2_GIZCLAW_RPC_ERROR_NOT_FOUND
               ? H2_PAL_ERR_NOT_FOUND
               : event->error_code == H2_GIZCLAW_RPC_ERROR_METHOD_NOT_FOUND
                     ? H2_PAL_ERR_UNSUPPORTED
                     : H2_PAL_ERR_IO;
  }
  if (event->kind == H2_GIZCLAW_RPC_STREAM_RESPONSE) {
    if (request->metadata_received)
      return H2_PAL_ERR_FORMAT;
    gizclaw_rpc_v1_FriendGroupMessageAudioDownloadResponse decoded =
        gizclaw_rpc_v1_FriendGroupMessageAudioDownloadResponse_init_zero;
    pb_istream_t input = pb_istream_from_buffer(event->result_payload.data,
                                                event->result_payload.len);
    if (!pb_decode(
            &input,
            gizclaw_rpc_v1_FriendGroupMessageAudioDownloadResponse_fields,
            &decoded) ||
        decoded.size_bytes <= 0 || decoded.friend_group_name[0] == '\0' ||
        decoded.history_name[0] == '\0' ||
        strncmp(decoded.mime_type, "audio/", 6u) != 0)
      return H2_PAL_ERR_FORMAT;
    info->friend_group_name = social_copy(
        request->allocator,
        (h2_gizclaw_str_t){decoded.friend_group_name,
                           strlen(decoded.friend_group_name)});
    info->history_id = social_copy(
        request->allocator,
        (h2_gizclaw_str_t){decoded.history_name,
                           strlen(decoded.history_name)});
    info->mime_type = social_copy(
        request->allocator,
        (h2_gizclaw_str_t){decoded.mime_type, strlen(decoded.mime_type)});
    if (info->friend_group_name == NULL || info->history_id == NULL ||
        info->mime_type == NULL)
      return H2_PAL_ERR_NO_MEMORY;
    info->size_bytes = (uint64_t)decoded.size_bytes;
    request->metadata_received = true;
    return H2_PAL_OK;
  }
  if (event->kind == H2_GIZCLAW_RPC_STREAM_DATA) {
    if (!request->metadata_received ||
        info->received_bytes > info->size_bytes ||
        event->data.len > info->size_bytes - info->received_bytes)
      return H2_PAL_ERR_FORMAT;
    const h2_pal_result_t rc = (h2_pal_result_t)request->write(
        request->write_user, event->data.data, event->data.len);
    if (rc == H2_PAL_OK)
      info->received_bytes += event->data.len;
    return rc;
  }
  if (event->kind != H2_GIZCLAW_RPC_STREAM_EOS ||
      !request->metadata_received ||
      info->received_bytes != info->size_bytes)
    return H2_PAL_ERR_FORMAT;
  request->eos_received = true;
  return H2_PAL_OK;
}

static void social_audio_complete(void *user,
                                  h2_gizclaw_async_stream_t *stream) {
  h2_gizclaw_social_request_t *request = user;
  const h2_gizclaw_operation_result_t *operation_result =
      h2_gizclaw_async_stream_operation_result(stream);
  request->operation_result = operation_result == NULL
                                  ? (h2_gizclaw_operation_result_t){
                                        .result = H2_PAL_ERR_INVALID_STATE}
                                  : *operation_result;
  const h2_gizclaw_friend_group_message_audio_info_t *info =
      &request->result.value.message_audio;
  if (request->operation_result.result == H2_PAL_OK &&
      (!request->metadata_received || !request->eos_received ||
       info->friend_group_name == NULL || info->history_id == NULL ||
       strcmp(info->friend_group_name, request->text[0]) != 0 ||
       strcmp(info->history_id, request->text[1]) != 0)) {
    request->operation_result.result = H2_PAL_ERR_FORMAT;
  }
  if (request->operation_result.result != H2_PAL_OK)
    social_result_clear(request);
  atomic_store_explicit(&request->terminal, true, memory_order_release);
  request->completion(request->completion_user, request);
}

static h2_pal_result_t social_audio_submit(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_social_request_t *request) {
  gizclaw_rpc_v1_FriendGroupMessageAudioDownloadRequest message =
      gizclaw_rpc_v1_FriendGroupMessageAudioDownloadRequest_init_zero;
  if (!social_copy_fixed(message.friend_group_name,
                         sizeof(message.friend_group_name),
                         social_span(request->text[0])) ||
      !social_copy_fixed(message.history_name, sizeof(message.history_name),
                         social_span(request->text[1])))
    return H2_PAL_ERR_INVALID_ARG;
  pb_ostream_t sizing = PB_OSTREAM_SIZING;
  if (!pb_encode(
          &sizing,
          gizclaw_rpc_v1_FriendGroupMessageAudioDownloadRequest_fields,
          &message))
    return H2_PAL_ERR_FORMAT;
  uint8_t *payload = h2_pal_mem_alloc(
      request->allocator,
      sizing.bytes_written == 0u ? 1u : sizing.bytes_written);
  if (payload == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  pb_ostream_t output = pb_ostream_from_buffer(payload, sizing.bytes_written);
  h2_pal_result_t rc =
      pb_encode(&output,
                gizclaw_rpc_v1_FriendGroupMessageAudioDownloadRequest_fields,
                &message)
          ? H2_PAL_OK
          : H2_PAL_ERR_FORMAT;
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_service_rpc_stream_async(
        service, identity,
        H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_MESSAGES_AUDIO_DOWNLOAD,
        (h2_gizclaw_rpc_bytes_t){payload, output.bytes_written}, 30000u,
        social_audio_event, social_audio_complete, request, &request->stream);
  }
  h2_pal_mem_free(request->allocator, payload);
  return rc;
}

static h2_pal_result_t social_execute(h2_gizclaw_social_request_t *request,
                                     h2_gizclaw_client_t *client) {
  const h2_gizclaw_str_t a = social_span(request->text[0]);
  const h2_gizclaw_str_t b = social_span(request->text[1]);
  const h2_gizclaw_str_t c = social_span(request->text[2]);
  h2_pal_result_t rc = H2_PAL_ERR_INVALID_STATE;
  switch (request->kind) {
  case H2_GIZCLAW_SOCIAL_CONTACTS_LIST:
    rc = (h2_pal_result_t)h2_gizclaw_client_contacts_list(
        client, a, request->limit, &request->result.value.contact_page);
    break;
  case H2_GIZCLAW_SOCIAL_CONTACT_GET:
    rc = (h2_pal_result_t)h2_gizclaw_client_contact_get(
        client, a, &request->result.value.contact);
    break;
  case H2_GIZCLAW_SOCIAL_CONTACT_CREATE:
    rc = (h2_pal_result_t)h2_gizclaw_client_contact_create(
        client, a, b, c, &request->result.value.contact);
    break;
  case H2_GIZCLAW_SOCIAL_CONTACT_PUT:
    rc = (h2_pal_result_t)h2_gizclaw_client_contact_put(
        client, a, b, c, &request->result.value.contact);
    break;
  case H2_GIZCLAW_SOCIAL_CONTACT_DELETE:
    rc = (h2_pal_result_t)h2_gizclaw_client_contact_delete(
        client, a, &request->result.value.contact);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUPS_LIST:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_groups_list(
        client, a, request->limit, &request->result.value.friend_group_page);
    break;
  case H2_GIZCLAW_SOCIAL_FRIENDS_LIST:
    rc = (h2_pal_result_t)h2_gizclaw_client_friends_list(
        client, a, request->limit, &request->result.value.friend_page);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_INFO_GET:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_info_get(
        client, a, &request->result.value.friend_value);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_ADD:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_add(
        client, a, &request->result.value.friend_value);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_DELETE:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_delete(
        client, a, &request->result.value.friend_value);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_INVITE_TOKEN_GET:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_invite_token_get(
        client, &request->result.value.invite_token);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_INVITE_TOKEN_CREATE:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_invite_token_create(
        client, &request->result.value.invite_token);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_INVITE_TOKEN_CLEAR:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_invite_token_clear(client);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_GET:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_group_get(
        client, a, &request->result.value.friend_group);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_CREATE:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_group_create(
        client, a, b, c, &request->result.value.friend_group);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_PUT:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_group_put(
        client, a, b, c, &request->result.value.friend_group);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_DELETE:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_group_delete(
        client, a, &request->result.value.friend_group);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_JOIN:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_group_join(
        client, a, b, &request->result.value.friend_group);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_INVITE_TOKEN_GET:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_group_invite_token_get(
        client, a, &request->result.value.invite_token);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_INVITE_TOKEN_CREATE:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_group_invite_token_create(
        client, a, &request->result.value.invite_token);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_INVITE_TOKEN_CLEAR:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_group_invite_token_clear(
        client, a);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MEMBERS_LIST:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_group_members_list(
        client, a, b, request->limit, &request->result.value.member_page);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MEMBER_PUT:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_group_member_put(
        client, a, b, request->role, &request->result.value.member);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MEMBER_DELETE:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_group_member_delete(
        client, a, b, &request->result.value.member);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MESSAGES_LIST:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_group_messages_list(
        client, a, b, request->limit, &request->result.value.message_page);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MESSAGE_GET:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_group_message_get(
        client, a, b, &request->result.value.message);
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MESSAGE_AUDIO_DOWNLOAD:
    rc = (h2_pal_result_t)h2_gizclaw_client_friend_group_message_audio_download(
        client, a, b, social_write, request,
        &request->result.value.message_audio);
    break;
  }
  return rc;
}

static int social_capture_rpc(void *user, h2_gizclaw_client_t *client,
                              h2_gizclaw_rpc_method_t method,
                              h2_gizclaw_rpc_bytes_t params_payload,
                              h2_gizclaw_rpc_response_t *out_response) {
  (void)out_response;
  h2_gizclaw_social_request_t *request = user;
  if (request->rpc_request != NULL)
    return H2_PAL_ERR_INVALID_STATE;
  const int rc = h2_gizclaw_client_rpc_request_start(
      client, method, params_payload, 5000u, &request->rpc_request);
  return rc == H2_PAL_OK ? H2_PAL_ERR_WOULD_BLOCK : rc;
}

static int social_replay_rpc(void *user, h2_gizclaw_client_t *client,
                             h2_gizclaw_rpc_method_t method,
                             h2_gizclaw_rpc_bytes_t params_payload,
                             h2_gizclaw_rpc_response_t *out_response) {
  (void)client;
  (void)method;
  (void)params_payload;
  h2_gizclaw_social_request_t *request = user;
  *out_response = request->rpc_response;
  memset(&request->rpc_response, 0, sizeof(request->rpc_response));
  return H2_PAL_OK;
}

static h2_pal_result_t
social_start(void *user, h2_gizclaw_client_t *client,
             const h2_gizclaw_cancel_token_t *cancel_token) {
  h2_gizclaw_social_request_t *request = user;
  if (h2_gizclaw_cancel_requested(cancel_token))
    return H2_PAL_ERR_CLOSED;
  request->cancel_token = cancel_token;
  h2_gizclaw_client_set_rpc_interceptor_internal(
      client, social_capture_rpc, NULL, request);
  const h2_pal_result_t rc = social_execute(request, client);
  h2_gizclaw_client_set_rpc_interceptor_internal(client, NULL, NULL, NULL);
  if (rc != H2_PAL_ERR_WOULD_BLOCK || request->rpc_request == NULL) {
    request->cancel_token = NULL;
    return rc == H2_PAL_ERR_WOULD_BLOCK ? H2_PAL_ERR_INVALID_STATE : rc;
  }
  return H2_PAL_ERR_WOULD_BLOCK;
}

static h2_pal_result_t
social_poll(void *user, h2_gizclaw_client_t *client,
            const h2_gizclaw_cancel_token_t *cancel_token) {
  h2_gizclaw_social_request_t *request = user;
  if (h2_gizclaw_cancel_requested(cancel_token)) {
    h2_gizclaw_rpc_request_cancel(request->rpc_request);
    h2_gizclaw_rpc_request_destroy(request->rpc_request);
    request->rpc_request = NULL;
    request->cancel_token = NULL;
    return H2_PAL_ERR_CLOSED;
  }
  h2_pal_result_t rc = (h2_pal_result_t)h2_gizclaw_rpc_request_result(
      request->rpc_request, &request->rpc_response);
  if (rc == H2_PAL_ERR_WOULD_BLOCK)
    return rc;
  h2_gizclaw_rpc_request_destroy(request->rpc_request);
  request->rpc_request = NULL;
  if (rc == H2_PAL_OK) {
    h2_gizclaw_client_set_rpc_interceptor_internal(
        client, social_replay_rpc, NULL, request);
    rc = social_execute(request, client);
    h2_gizclaw_client_set_rpc_interceptor_internal(client, NULL, NULL, NULL);
  }
  request->cancel_token = NULL;
  return rc;
}

static void
social_complete(void *user, h2_gizclaw_operation_t *operation,
                const h2_gizclaw_operation_result_t *operation_result) {
  (void)operation;
  h2_gizclaw_social_request_t *request = user;
  h2_gizclaw_operation_result_t result = *operation_result;
  if (result.result != H2_PAL_OK)
    social_result_clear(request);
  request->operation_result = result;
  atomic_store_explicit(&request->terminal, true, memory_order_release);
  request->completion(request->completion_user, request);
}

static void social_free(h2_gizclaw_social_request_t *request) {
  if (request == NULL)
    return;
  for (size_t index = 0u; index < 3u; ++index)
    h2_pal_mem_free(request->allocator, request->text[index]);
  h2_pal_mem_free(request->allocator, request);
}

static h2_pal_result_t
social_submit(h2_gizclaw_service_t *service, uint64_t identity,
              h2_gizclaw_social_request_kind_t kind, h2_gizclaw_str_t a,
              h2_gizclaw_str_t b, h2_gizclaw_str_t c, size_t limit,
              h2_gizclaw_friend_group_role_t role,
              h2_gizclaw_friend_group_message_audio_write_fn write,
              void *write_user, h2_gizclaw_social_completion_fn completion,
              void *user, h2_gizclaw_social_request_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  bool require_a = false;
  bool require_b = false;
  switch (kind) {
  case H2_GIZCLAW_SOCIAL_CONTACT_GET:
  case H2_GIZCLAW_SOCIAL_CONTACT_CREATE:
  case H2_GIZCLAW_SOCIAL_CONTACT_PUT:
  case H2_GIZCLAW_SOCIAL_CONTACT_DELETE:
  case H2_GIZCLAW_SOCIAL_FRIEND_INFO_GET:
  case H2_GIZCLAW_SOCIAL_FRIEND_ADD:
  case H2_GIZCLAW_SOCIAL_FRIEND_DELETE:
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_GET:
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_CREATE:
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_PUT:
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_DELETE:
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_INVITE_TOKEN_GET:
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_INVITE_TOKEN_CREATE:
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_INVITE_TOKEN_CLEAR:
    require_a = true;
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_JOIN:
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MEMBER_PUT:
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MEMBER_DELETE:
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MESSAGE_GET:
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MESSAGE_AUDIO_DOWNLOAD:
    require_a = true;
    require_b = true;
    break;
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MEMBERS_LIST:
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MESSAGES_LIST:
    require_a = true;
    break;
  case H2_GIZCLAW_SOCIAL_CONTACTS_LIST:
  case H2_GIZCLAW_SOCIAL_FRIEND_GROUPS_LIST:
  case H2_GIZCLAW_SOCIAL_FRIENDS_LIST:
  case H2_GIZCLAW_SOCIAL_FRIEND_INVITE_TOKEN_GET:
  case H2_GIZCLAW_SOCIAL_FRIEND_INVITE_TOKEN_CREATE:
  case H2_GIZCLAW_SOCIAL_FRIEND_INVITE_TOKEN_CLEAR:
    break;
  }
  if (service == NULL || completion == NULL || out_request == NULL ||
      !social_text_valid(a, require_a) || !social_text_valid(b, require_b) ||
      !social_text_valid(c, false))
    return H2_PAL_ERR_INVALID_ARG;
  const h2_pal_mem_api_t *allocator = service->config.client_config->allocator;
  h2_gizclaw_social_request_t *request =
      h2_pal_mem_alloc(allocator, sizeof(*request));
  if (request == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(request, 0, sizeof(*request));
  request->allocator = allocator;
  request->kind = kind;
  request->result.kind = kind;
  request->completion = completion;
  request->completion_user = user;
  request->limit = limit;
  request->role = role;
  request->write = write;
  request->write_user = write_user;
  const h2_gizclaw_str_t values[3] = {a, b, c};
  for (size_t index = 0u; index < 3u; ++index) {
    request->text[index] = social_copy(allocator, values[index]);
    if (request->text[index] == NULL) {
      social_free(request);
      return H2_PAL_ERR_NO_MEMORY;
    }
  }
  const h2_pal_result_t rc =
      kind == H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MESSAGE_AUDIO_DOWNLOAD
          ? social_audio_submit(service, identity, request)
          : h2_gizclaw_service_submit_async_internal(
                service, identity, social_start, social_poll, social_complete,
                request, &request->operation);
  if (rc != H2_PAL_OK) {
    social_free(request);
    return rc;
  }
  *out_request = request;
  return H2_PAL_OK;
}

#define SOCIAL_SUBMIT0(kind)                                                   \
  return social_submit(service, identity, kind, (h2_gizclaw_str_t){0},         \
                       (h2_gizclaw_str_t){0}, (h2_gizclaw_str_t){0}, 0u,       \
                       H2_GIZCLAW_FRIEND_GROUP_ROLE_UNSPECIFIED, NULL, NULL,   \
                       completion, user, out_request)
#define SOCIAL_SUBMIT1(kind, a)                                                \
  return social_submit(service, identity, kind, a, (h2_gizclaw_str_t){0},      \
                       (h2_gizclaw_str_t){0}, 0u,                              \
                       H2_GIZCLAW_FRIEND_GROUP_ROLE_UNSPECIFIED, NULL, NULL,   \
                       completion, user, out_request)
#define SOCIAL_SUBMIT2(kind, a, b)                                             \
  return social_submit(service, identity, kind, a, b, (h2_gizclaw_str_t){0},   \
                       0u, H2_GIZCLAW_FRIEND_GROUP_ROLE_UNSPECIFIED, NULL,     \
                       NULL, completion, user, out_request)
#define SOCIAL_SUBMIT3(kind, a, b, c)                                          \
  return social_submit(service, identity, kind, a, b, c, 0u,                   \
                       H2_GIZCLAW_FRIEND_GROUP_ROLE_UNSPECIFIED, NULL, NULL,   \
                       completion, user, out_request)

h2_pal_result_t h2_gizclaw_service_contacts_list_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t cursor,
    size_t limit, h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  if (limit == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  return social_submit(service, identity, H2_GIZCLAW_SOCIAL_CONTACTS_LIST,
                       cursor, (h2_gizclaw_str_t){0}, (h2_gizclaw_str_t){0},
                       limit, H2_GIZCLAW_FRIEND_GROUP_ROLE_UNSPECIFIED, NULL,
                       NULL, completion, user, out_request);
}

h2_pal_result_t h2_gizclaw_service_contact_get_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT1(H2_GIZCLAW_SOCIAL_CONTACT_GET, name);
}

h2_pal_result_t h2_gizclaw_service_contact_create_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    h2_gizclaw_str_t display_name, h2_gizclaw_str_t phone_number,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT3(H2_GIZCLAW_SOCIAL_CONTACT_CREATE, name, display_name,
                 phone_number);
}

h2_pal_result_t h2_gizclaw_service_contact_put_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    h2_gizclaw_str_t display_name, h2_gizclaw_str_t phone_number,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT3(H2_GIZCLAW_SOCIAL_CONTACT_PUT, name, display_name,
                 phone_number);
}

h2_pal_result_t h2_gizclaw_service_contact_delete_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT1(H2_GIZCLAW_SOCIAL_CONTACT_DELETE, name);
}

static h2_pal_result_t
social_list_submit(h2_gizclaw_service_t *service, uint64_t identity,
                   h2_gizclaw_social_request_kind_t kind,
                   h2_gizclaw_str_t first, h2_gizclaw_str_t cursor,
                   size_t limit, h2_gizclaw_social_completion_fn completion,
                   void *user, h2_gizclaw_social_request_t **out_request) {
  if (limit == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  return social_submit(service, identity, kind, first, cursor,
                       (h2_gizclaw_str_t){0}, limit,
                       H2_GIZCLAW_FRIEND_GROUP_ROLE_UNSPECIFIED, NULL, NULL,
                       completion, user, out_request);
}

h2_pal_result_t h2_gizclaw_service_friend_groups_list_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t cursor,
    size_t limit, h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  return social_list_submit(
      service, identity, H2_GIZCLAW_SOCIAL_FRIEND_GROUPS_LIST,
      (h2_gizclaw_str_t){0}, cursor, limit, completion, user, out_request);
}

h2_pal_result_t h2_gizclaw_service_friends_list_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t cursor,
    size_t limit, h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  return social_list_submit(service, identity, H2_GIZCLAW_SOCIAL_FRIENDS_LIST,
                            (h2_gizclaw_str_t){0}, cursor, limit, completion,
                            user, out_request);
}

h2_pal_result_t h2_gizclaw_service_friend_info_get_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t friend_id, h2_gizclaw_social_completion_fn completion,
    void *user, h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT1(H2_GIZCLAW_SOCIAL_FRIEND_INFO_GET, friend_id);
}
h2_pal_result_t h2_gizclaw_service_friend_add_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t invite_token, h2_gizclaw_social_completion_fn completion,
    void *user, h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT1(H2_GIZCLAW_SOCIAL_FRIEND_ADD, invite_token);
}
h2_pal_result_t h2_gizclaw_service_friend_delete_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t friend_id, h2_gizclaw_social_completion_fn completion,
    void *user, h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT1(H2_GIZCLAW_SOCIAL_FRIEND_DELETE, friend_id);
}
h2_pal_result_t h2_gizclaw_service_friend_invite_token_get_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT0(H2_GIZCLAW_SOCIAL_FRIEND_INVITE_TOKEN_GET);
}
h2_pal_result_t h2_gizclaw_service_friend_invite_token_create_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT0(H2_GIZCLAW_SOCIAL_FRIEND_INVITE_TOKEN_CREATE);
}
h2_pal_result_t h2_gizclaw_service_friend_invite_token_clear_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT0(H2_GIZCLAW_SOCIAL_FRIEND_INVITE_TOKEN_CLEAR);
}
h2_pal_result_t h2_gizclaw_service_friend_group_get_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_social_completion_fn completion,
    void *user, h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT1(H2_GIZCLAW_SOCIAL_FRIEND_GROUP_GET, group_name);
}
h2_pal_result_t h2_gizclaw_service_friend_group_create_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    h2_gizclaw_str_t display_name, h2_gizclaw_str_t description,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT3(H2_GIZCLAW_SOCIAL_FRIEND_GROUP_CREATE, name, display_name,
                 description);
}
h2_pal_result_t h2_gizclaw_service_friend_group_put_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    h2_gizclaw_str_t display_name, h2_gizclaw_str_t description,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT3(H2_GIZCLAW_SOCIAL_FRIEND_GROUP_PUT, name, display_name,
                 description);
}
h2_pal_result_t h2_gizclaw_service_friend_group_delete_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_social_completion_fn completion,
    void *user, h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT1(H2_GIZCLAW_SOCIAL_FRIEND_GROUP_DELETE, group_name);
}
h2_pal_result_t h2_gizclaw_service_friend_group_join_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t invite_token, h2_gizclaw_str_t name,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT2(H2_GIZCLAW_SOCIAL_FRIEND_GROUP_JOIN, invite_token, name);
}
h2_pal_result_t h2_gizclaw_service_friend_group_invite_token_get_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_social_completion_fn completion,
    void *user, h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT1(H2_GIZCLAW_SOCIAL_FRIEND_GROUP_INVITE_TOKEN_GET, group_name);
}
h2_pal_result_t h2_gizclaw_service_friend_group_invite_token_create_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_social_completion_fn completion,
    void *user, h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT1(H2_GIZCLAW_SOCIAL_FRIEND_GROUP_INVITE_TOKEN_CREATE,
                 group_name);
}
h2_pal_result_t h2_gizclaw_service_friend_group_invite_token_clear_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_social_completion_fn completion,
    void *user, h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT1(H2_GIZCLAW_SOCIAL_FRIEND_GROUP_INVITE_TOKEN_CLEAR, group_name);
}
h2_pal_result_t h2_gizclaw_service_friend_group_members_list_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_str_t cursor, size_t limit,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  return social_list_submit(
      service, identity, H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MEMBERS_LIST,
      group_name, cursor, limit, completion, user, out_request);
}
h2_pal_result_t h2_gizclaw_service_friend_group_member_put_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_str_t member_id,
    h2_gizclaw_friend_group_role_t role,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  return social_submit(service, identity,
                       H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MEMBER_PUT, group_name,
                       member_id, (h2_gizclaw_str_t){0}, 0u, role, NULL, NULL,
                       completion, user, out_request);
}
h2_pal_result_t h2_gizclaw_service_friend_group_member_delete_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_str_t member_id,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT2(H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MEMBER_DELETE, group_name,
                 member_id);
}
h2_pal_result_t h2_gizclaw_service_friend_group_messages_list_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_str_t cursor, size_t limit,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  return social_list_submit(
      service, identity, H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MESSAGES_LIST,
      group_name, cursor, limit, completion, user, out_request);
}
h2_pal_result_t h2_gizclaw_service_friend_group_message_get_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_str_t history_id,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  SOCIAL_SUBMIT2(H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MESSAGE_GET, group_name,
                 history_id);
}
h2_pal_result_t h2_gizclaw_service_friend_group_message_audio_download_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t friend_group_name, h2_gizclaw_str_t history_id,
    h2_gizclaw_friend_group_message_audio_write_fn write, void *write_user,
    h2_gizclaw_social_completion_fn completion, void *user,
    h2_gizclaw_social_request_t **out_request) {
  if (write == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  return social_submit(service, identity,
                       H2_GIZCLAW_SOCIAL_FRIEND_GROUP_MESSAGE_AUDIO_DOWNLOAD,
                       friend_group_name, history_id, (h2_gizclaw_str_t){0}, 0u,
                       H2_GIZCLAW_FRIEND_GROUP_ROLE_UNSPECIFIED, write,
                       write_user, completion, user, out_request);
}

h2_pal_result_t
h2_gizclaw_social_request_cancel(h2_gizclaw_social_request_t *request) {
  if (request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  return request->stream != NULL
             ? h2_gizclaw_async_stream_cancel(request->stream)
             : h2_gizclaw_operation_cancel(request->operation);
}

h2_pal_result_t
h2_gizclaw_social_request_wait(h2_gizclaw_social_request_t *request,
                               uint32_t timeout_ms) {
  if (request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  return request->stream != NULL
             ? h2_gizclaw_async_stream_wait(request->stream, timeout_ms)
             : h2_gizclaw_operation_wait(request->operation, timeout_ms);
}

const h2_gizclaw_operation_result_t *
h2_gizclaw_social_request_operation_result(
    const h2_gizclaw_social_request_t *request) {
  if (request == NULL ||
      !atomic_load_explicit(&request->terminal, memory_order_acquire))
    return NULL;
  return &request->operation_result;
}

const h2_gizclaw_social_result_t *
h2_gizclaw_social_request_result(const h2_gizclaw_social_request_t *request) {
  const h2_gizclaw_operation_result_t *operation_result =
      h2_gizclaw_social_request_operation_result(request);
  if (operation_result == NULL || operation_result->result != H2_PAL_OK)
    return NULL;
  return &request->result;
}

void h2_gizclaw_social_request_release(h2_gizclaw_social_request_t *request) {
  if (request == NULL ||
      !atomic_load_explicit(&request->terminal, memory_order_acquire))
    return;
  if (request->stream != NULL)
    h2_gizclaw_async_stream_release(request->stream);
  else
    h2_gizclaw_operation_release(request->operation);
  social_result_clear(request);
  social_free(request);
}

#undef SOCIAL_SUBMIT0
#undef SOCIAL_SUBMIT1
#undef SOCIAL_SUBMIT2
#undef SOCIAL_SUBMIT3
