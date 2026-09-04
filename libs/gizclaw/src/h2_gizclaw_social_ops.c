#include "h2_gizclaw_download_internal.h"
#include "h2_gizclaw_response_internal.h"
#include "h2_gizclaw_service_internal.h"
#include "h2_gizclaw_social_internal.h"

#include "h2_gizclaw_internal.h"
#include "h2_gizclaw_rpc.h"

#include "payload/social.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

typedef struct social_text_decode {
  const h2_pal_mem_api_t *allocator;
  char **out;
  size_t max_len;
} social_text_decode_t;

typedef struct social_text_encode {
  const void *data;
  size_t len;
} social_text_encode_t;

typedef struct friend_page_decode {
  const h2_pal_mem_api_t *allocator;
  h2_gizclaw_friend_page_t *page;
  size_t limit;
  size_t capacity;
} friend_page_decode_t;

typedef struct member_page_decode {
  const h2_pal_mem_api_t *allocator;
  h2_gizclaw_friend_group_member_page_t *page;
  size_t limit;
  size_t capacity;
} member_page_decode_t;

typedef struct message_page_decode {
  const h2_pal_mem_api_t *allocator;
  h2_gizclaw_friend_group_message_page_t *page;
  size_t limit;
  size_t capacity;
} message_page_decode_t;

static bool valid_utf8_span(const char *text, size_t len) {
  if (text == NULL)
    return len == 0u;
  const unsigned char *cursor = (const unsigned char *)text;
  size_t remaining = len;
  while (remaining > 0u) {
    size_t width = 0u;
    if (cursor[0] == 0u) {
      return false;
    } else if (cursor[0] <= 0x7fu) {
      width = 1u;
    } else if (remaining >= 2u && cursor[0] >= 0xc2u && cursor[0] <= 0xdfu &&
               cursor[1] >= 0x80u && cursor[1] <= 0xbfu) {
      width = 2u;
    } else if (remaining >= 3u &&
               ((cursor[0] == 0xe0u && cursor[1] >= 0xa0u &&
                 cursor[1] <= 0xbfu) ||
                (cursor[0] >= 0xe1u && cursor[0] <= 0xecu &&
                 cursor[1] >= 0x80u && cursor[1] <= 0xbfu) ||
                (cursor[0] == 0xedu && cursor[1] >= 0x80u &&
                 cursor[1] <= 0x9fu) ||
                (cursor[0] >= 0xeeu && cursor[0] <= 0xefu &&
                 cursor[1] >= 0x80u && cursor[1] <= 0xbfu)) &&
               cursor[2] >= 0x80u && cursor[2] <= 0xbfu) {
      width = 3u;
    } else if (remaining >= 4u &&
               ((cursor[0] == 0xf0u && cursor[1] >= 0x90u &&
                 cursor[1] <= 0xbfu) ||
                (cursor[0] >= 0xf1u && cursor[0] <= 0xf3u &&
                 cursor[1] >= 0x80u && cursor[1] <= 0xbfu) ||
                (cursor[0] == 0xf4u && cursor[1] >= 0x80u &&
                 cursor[1] <= 0x8fu)) &&
               cursor[2] >= 0x80u && cursor[2] <= 0xbfu && cursor[3] >= 0x80u &&
               cursor[3] <= 0xbfu) {
      width = 4u;
    } else {
      return false;
    }
    cursor += width;
    remaining -= width;
  }
  return true;
}

static bool valid_text(h2_gizclaw_str_t value, bool allow_empty) {
  return (allow_empty || value.len > 0u) &&
         (value.len == 0u || value.data != NULL) &&
         valid_utf8_span(value.data, value.len);
}

static bool valid_group_name(h2_gizclaw_str_t value) {
  return value.len <= H2_GIZCLAW_FRIEND_GROUP_NAME_MAX_BYTES &&
         valid_text(value, false);
}

static bool valid_owned_text(const char *text) {
  return text == NULL || valid_utf8_span(text, strlen(text));
}

static bool decode_text(pb_istream_t *stream, const pb_field_t *field,
                        void **arg) {
  (void)field;
  social_text_decode_t *context = *arg;
  if (context == NULL || context->allocator == NULL || context->out == NULL ||
      *context->out != NULL || stream->bytes_left == SIZE_MAX ||
      stream->bytes_left > context->max_len) {
    return false;
  }
  const size_t len = stream->bytes_left;
  char *text = h2_pal_mem_alloc(context->allocator, len + 1u);
  if (text == NULL)
    return false;
  if (!pb_read(stream, (pb_byte_t *)text, len) ||
      memchr(text, '\0', len) != NULL || !valid_utf8_span(text, len)) {
    h2_pal_mem_free(context->allocator, text);
    return false;
  }
  text[len] = '\0';
  *context->out = text;
  return true;
}

static void set_decoder(pb_callback_t *callback, social_text_decode_t *context,
                        const h2_pal_mem_api_t *allocator, char **out) {
  *context = (social_text_decode_t){
      .allocator = allocator, .out = out, .max_len = SIZE_MAX - 1u};
  callback->funcs.decode = decode_text;
  callback->arg = context;
}

static void set_bounded_decoder(pb_callback_t *callback,
                                social_text_decode_t *context,
                                const h2_pal_mem_api_t *allocator, char **out,
                                size_t max_len) {
  set_decoder(callback, context, allocator, out);
  context->max_len = max_len;
}

static bool encode_text(pb_ostream_t *stream, const pb_field_t *field,
                        void *const *arg) {
  const social_text_encode_t *text = *arg;
  return text != NULL && (text->len == 0u || text->data != NULL) &&
         pb_encode_tag_for_field(stream, field) &&
         pb_encode_string(stream, text->data, text->len);
}

static void set_encoder(pb_callback_t *callback, social_text_encode_t *context,
                        h2_gizclaw_str_t value) {
  *context = (social_text_encode_t){.data = value.data, .len = value.len};
  callback->funcs.encode = encode_text;
  callback->arg = context;
}

static int encode_message(const h2_pal_mem_api_t *allocator,
                          const pb_msgdesc_t *fields, const void *request,
                          uint8_t **out_payload, size_t *out_payload_len) {
  if (allocator == NULL || fields == NULL || request == NULL ||
      out_payload == NULL || out_payload_len == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_payload = NULL;
  *out_payload_len = 0u;
  pb_ostream_t sizing = PB_OSTREAM_SIZING;
  if (!pb_encode(&sizing, fields, request))
    return H2_PAL_ERR_FORMAT;
  uint8_t *payload = h2_pal_mem_alloc(
      allocator, sizing.bytes_written == 0u ? 1u : sizing.bytes_written);
  if (payload == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  pb_ostream_t stream = pb_ostream_from_buffer(payload, sizing.bytes_written);
  if (!pb_encode(&stream, fields, request)) {
    h2_pal_mem_free(allocator, payload);
    return H2_PAL_ERR_FORMAT;
  }
  *out_payload = payload;
  *out_payload_len = stream.bytes_written;
  return H2_PAL_OK;
}

static char *duplicate_text(const h2_pal_mem_api_t *allocator,
                            const char *text) {
  if (text == NULL)
    return NULL;
  const size_t len = strlen(text);
  char *copy = h2_pal_mem_alloc(allocator, len + 1u);
  if (copy != NULL)
    memcpy(copy, text, len + 1u);
  return copy;
}

static bool copy_fixed_text(char *out, size_t capacity,
                            h2_gizclaw_str_t value) {
  if (out == NULL || capacity == 0u || value.len >= capacity ||
      !valid_text(value, false)) {
    return false;
  }
  memcpy(out, value.data, value.len);
  out[value.len] = '\0';
  return true;
}

static void friend_deinit(const h2_pal_mem_api_t *allocator,
                          h2_gizclaw_friend_t *friend_value) {
  if (friend_value == NULL)
    return;
  h2_pal_mem_free(allocator, friend_value->id);
  h2_pal_mem_free(allocator, friend_value->peer_public_key);
  h2_pal_mem_free(allocator, friend_value->workspace_name);
  h2_pal_mem_free(allocator, friend_value->created_at);
  h2_pal_mem_free(allocator, friend_value->updated_at);
  h2_pal_mem_free(allocator, friend_value->name);
  h2_pal_mem_free(allocator, friend_value->emoji);
  memset(friend_value, 0, sizeof(*friend_value));
}

static bool decode_friend_object(pb_istream_t *stream, h2_gizclaw_friend_t *out,
                                 const h2_pal_mem_api_t *allocator) {
  gizclaw_rpc_v1_FriendObject decoded = gizclaw_rpc_v1_FriendObject_init_zero;
  social_text_decode_t text[5];
  set_decoder(&decoded.name, &text[0], allocator, &out->id);
  set_decoder(&decoded.peer_public_key, &text[1], allocator,
              &out->peer_public_key);
  set_decoder(&decoded.workspace_name, &text[2], allocator,
              &out->workspace_name);
  set_decoder(&decoded.created_at, &text[3], allocator, &out->created_at);
  set_decoder(&decoded.updated_at, &text[4], allocator, &out->updated_at);
  if (!pb_decode(stream, gizclaw_rpc_v1_FriendObject_fields, &decoded) ||
      out->id == NULL || out->id[0] == '\0' ||
      !valid_owned_text(out->peer_public_key) ||
      !valid_owned_text(out->workspace_name) ||
      !valid_owned_text(out->created_at) ||
      !valid_owned_text(out->updated_at)) {
    friend_deinit(allocator, out);
    return false;
  }
  return true;
}

static bool decode_friend(pb_istream_t *stream, const pb_field_t *field,
                          void **arg) {
  (void)field;
  friend_page_decode_t *context = *arg;
  if (context == NULL || context->page == NULL ||
      context->page->count >= context->limit ||
      context->page->count == SIZE_MAX ||
      context->page->count + 1u > SIZE_MAX / sizeof(context->page->items[0])) {
    return false;
  }
  const size_t count = context->page->count;
  if (count >= context->capacity) {
    size_t capacity = context->capacity == 0u ? 4u : context->capacity * 2u;
    if (capacity <= count)
      capacity = count + 1u;
    if (capacity < context->capacity || capacity > context->limit)
      capacity = context->limit;
    if (capacity > SIZE_MAX / sizeof(context->page->items[0]))
      return false;
    h2_gizclaw_friend_t *grown = h2_pal_mem_realloc(
        context->allocator, context->page->items, capacity * sizeof(*grown));
    if (grown == NULL)
      return false;
    context->page->items = grown;
    context->capacity = capacity;
  }
  h2_gizclaw_friend_t *items = context->page->items;
  memset(&items[count], 0, sizeof(items[count]));
  if (!decode_friend_object(stream, &items[count], context->allocator))
    return false;
  context->page->count = count + 1u;
  return true;
}

static void group_deinit(const h2_pal_mem_api_t *allocator,
                         h2_gizclaw_friend_group_t *group) {
  if (group == NULL)
    return;
  h2_pal_mem_free(allocator, group->name);
  h2_pal_mem_free(allocator, group->display_name);
  h2_pal_mem_free(allocator, group->description);
  h2_pal_mem_free(allocator, group->workspace_name);
  memset(group, 0, sizeof(*group));
}

static void member_deinit(const h2_pal_mem_api_t *allocator,
                          h2_gizclaw_friend_group_member_t *member) {
  if (member == NULL)
    return;
  h2_pal_mem_free(allocator, member->id);
  h2_pal_mem_free(allocator, member->friend_group_name);
  h2_pal_mem_free(allocator, member->peer_public_key);
  h2_pal_mem_free(allocator, member->created_at);
  h2_pal_mem_free(allocator, member->updated_at);
  memset(member, 0, sizeof(*member));
}

static bool decode_member_object(pb_istream_t *stream,
                                 h2_gizclaw_friend_group_member_t *out,
                                 const h2_pal_mem_api_t *allocator) {
  gizclaw_rpc_v1_FriendGroupMemberObject decoded =
      gizclaw_rpc_v1_FriendGroupMemberObject_init_zero;
  social_text_decode_t text[5];
  set_decoder(&decoded.name, &text[0], allocator, &out->id);
  set_bounded_decoder(&decoded.friend_group_name, &text[1], allocator,
                      &out->friend_group_name,
                      H2_GIZCLAW_FRIEND_GROUP_NAME_MAX_BYTES);
  set_decoder(&decoded.peer_public_key, &text[2], allocator,
              &out->peer_public_key);
  set_decoder(&decoded.created_at, &text[3], allocator, &out->created_at);
  set_decoder(&decoded.updated_at, &text[4], allocator, &out->updated_at);
  if (!pb_decode(stream, gizclaw_rpc_v1_FriendGroupMemberObject_fields,
                 &decoded) ||
      out->id == NULL || out->id[0] == '\0' || out->friend_group_name == NULL ||
      out->friend_group_name[0] == '\0' ||
      !valid_owned_text(out->peer_public_key) ||
      (decoded.has_role &&
       (decoded.role <
            gizclaw_rpc_v1_FriendGroupMemberRole_FRIEND_GROUP_MEMBER_ROLE_UNSPECIFIED ||
        decoded.role >
            gizclaw_rpc_v1_FriendGroupMemberRole_FRIEND_GROUP_MEMBER_ROLE_MEMBER))) {
    member_deinit(allocator, out);
    return false;
  }
  out->role = decoded.has_role ? (h2_gizclaw_friend_group_role_t)decoded.role
                               : H2_GIZCLAW_FRIEND_GROUP_ROLE_UNSPECIFIED;
  return true;
}

static bool decode_member(pb_istream_t *stream, const pb_field_t *field,
                          void **arg) {
  (void)field;
  member_page_decode_t *context = *arg;
  if (context == NULL || context->page == NULL ||
      context->page->count >= context->limit ||
      context->page->count == SIZE_MAX ||
      context->page->count + 1u > SIZE_MAX / sizeof(context->page->items[0])) {
    return false;
  }
  const size_t count = context->page->count;
  if (count >= context->capacity) {
    size_t capacity = context->capacity == 0u ? 4u : context->capacity * 2u;
    if (capacity <= count)
      capacity = count + 1u;
    if (capacity < context->capacity || capacity > context->limit)
      capacity = context->limit;
    if (capacity > SIZE_MAX / sizeof(context->page->items[0]))
      return false;
    h2_gizclaw_friend_group_member_t *grown = h2_pal_mem_realloc(
        context->allocator, context->page->items, capacity * sizeof(*grown));
    if (grown == NULL)
      return false;
    context->page->items = grown;
    context->capacity = capacity;
  }
  h2_gizclaw_friend_group_member_t *items = context->page->items;
  memset(&items[count], 0, sizeof(items[count]));
  if (!decode_member_object(stream, &items[count], context->allocator))
    return false;
  context->page->count = count + 1u;
  return true;
}

static void message_deinit(const h2_pal_mem_api_t *allocator,
                           h2_gizclaw_friend_group_message_t *message) {
  if (message == NULL)
    return;
  h2_pal_mem_free(allocator, message->history_id);
  h2_pal_mem_free(allocator, message->friend_group_name);
  h2_pal_mem_free(allocator, message->sender_peer_public_key);
  h2_pal_mem_free(allocator, message->created_at);
  h2_pal_mem_free(allocator, message->expires_at);
  h2_pal_mem_free(allocator, message->name);
  h2_pal_mem_free(allocator, message->text);
  memset(message, 0, sizeof(*message));
}

static bool
copy_message_object(const gizclaw_rpc_v1_FriendGroupMessageObject *decoded,
                    h2_gizclaw_friend_group_message_t *out,
                    const h2_pal_mem_api_t *allocator) {
  if (decoded == NULL || decoded->name[0] == '\0' ||
      decoded->friend_group_name[0] == '\0' || decoded->created_at[0] == '\0' ||
      decoded->actor_name[0] == '\0' ||
      (decoded->type !=
           gizclaw_rpc_v1_PeerRunHistoryEntryType_PEER_RUN_HISTORY_ENTRY_TYPE_GEAR &&
       decoded->type !=
           gizclaw_rpc_v1_PeerRunHistoryEntryType_PEER_RUN_HISTORY_ENTRY_TYPE_AGENT) ||
      !valid_owned_text(decoded->name) ||
      !valid_owned_text(decoded->friend_group_name) ||
      !valid_owned_text(decoded->sender_peer_public_key) ||
      !valid_owned_text(decoded->created_at) ||
      !valid_owned_text(decoded->expires_at) ||
      !valid_owned_text(decoded->actor_name) ||
      !valid_owned_text(decoded->text) ||
      (decoded->has_expires_at && decoded->expires_at[0] == '\0') ||
      (decoded->has_sender_peer_public_key &&
       decoded->sender_peer_public_key[0] == '\0')) {
    message_deinit(allocator, out);
    return false;
  }
  out->history_id = duplicate_text(allocator, decoded->name);
  out->friend_group_name =
      duplicate_text(allocator, decoded->friend_group_name);
  out->sender_peer_public_key =
      decoded->has_sender_peer_public_key
          ? duplicate_text(allocator, decoded->sender_peer_public_key)
          : NULL;
  out->created_at = duplicate_text(allocator, decoded->created_at);
  out->expires_at = decoded->has_expires_at
                        ? duplicate_text(allocator, decoded->expires_at)
                        : NULL;
  out->name = duplicate_text(allocator, decoded->actor_name);
  out->text = duplicate_text(allocator, decoded->text);
  if (out->history_id == NULL || out->friend_group_name == NULL ||
      out->created_at == NULL || out->name == NULL || out->text == NULL ||
      (decoded->has_sender_peer_public_key &&
       out->sender_peer_public_key == NULL) ||
      (decoded->has_expires_at && out->expires_at == NULL)) {
    message_deinit(allocator, out);
    return false;
  }
  out->type = (h2_gizclaw_friend_group_message_type_t)decoded->type;
  out->audio_available = decoded->audio_available;
  return true;
}

static bool decode_message_object(pb_istream_t *stream,
                                  h2_gizclaw_friend_group_message_t *out,
                                  const h2_pal_mem_api_t *allocator) {
  gizclaw_rpc_v1_FriendGroupMessageObject decoded =
      gizclaw_rpc_v1_FriendGroupMessageObject_init_zero;
  return pb_decode(stream, gizclaw_rpc_v1_FriendGroupMessageObject_fields,
                   &decoded) &&
         copy_message_object(&decoded, out, allocator);
}

static bool decode_message(pb_istream_t *stream, const pb_field_t *field,
                           void **arg) {
  (void)field;
  message_page_decode_t *context = *arg;
  if (context == NULL || context->page == NULL ||
      context->page->count >= context->limit ||
      context->page->count == SIZE_MAX ||
      context->page->count + 1u > SIZE_MAX / sizeof(context->page->items[0])) {
    return false;
  }
  const size_t count = context->page->count;
  if (count >= context->capacity) {
    size_t capacity = context->capacity == 0u ? 4u : context->capacity * 2u;
    if (capacity <= count)
      capacity = count + 1u;
    if (capacity < context->capacity || capacity > context->limit)
      capacity = context->limit;
    if (capacity > SIZE_MAX / sizeof(context->page->items[0]))
      return false;
    h2_gizclaw_friend_group_message_t *grown = h2_pal_mem_realloc(
        context->allocator, context->page->items, capacity * sizeof(*grown));
    if (grown == NULL)
      return false;
    context->page->items = grown;
    context->capacity = capacity;
  }
  h2_gizclaw_friend_group_message_t *items = context->page->items;
  memset(&items[count], 0, sizeof(items[count]));
  if (!decode_message_object(stream, &items[count], context->allocator))
    return false;
  context->page->count = count + 1u;
  return true;
}

static bool decode_cursor(bool has_next, char *cursor) {
  return !has_next ||
         (cursor != NULL && cursor[0] != '\0' && valid_owned_text(cursor));
}

static void token_deinit(const h2_pal_mem_api_t *allocator,
                         h2_gizclaw_invite_token_t *token) {
  if (token == NULL)
    return;
  h2_pal_mem_free(allocator, token->value);
  h2_pal_mem_free(allocator, token->expires_at);
  memset(token, 0, sizeof(*token));
}

static int decode_token_response(const h2_pal_mem_api_t *allocator,
                                 const h2_gizclaw_rpc_response_t *response,
                                 const pb_msgdesc_t *fields, void *decoded,
                                 pb_callback_t *invite_token,
                                 pb_callback_t *expires_at,
                                 h2_gizclaw_invite_token_t *out_token,
                                 bool allow_empty) {
  social_text_decode_t text[2];
  set_decoder(invite_token, &text[0], allocator, &out_token->value);
  set_decoder(expires_at, &text[1], allocator, &out_token->expires_at);
  pb_istream_t stream = pb_istream_from_buffer(response->result_payload,
                                               response->result_payload_len);
  const bool decoded_ok = pb_decode(&stream, fields, decoded);
  if (decoded_ok && allow_empty && out_token->value == NULL &&
      out_token->expires_at == NULL)
    return H2_PAL_OK;
  if (!decoded_ok || out_token->value == NULL || out_token->value[0] == '\0' ||
      out_token->expires_at == NULL || out_token->expires_at[0] == '\0') {
    token_deinit(allocator, out_token);
    return H2_PAL_ERR_FORMAT;
  }
  return H2_PAL_OK;
}

static int decode_group_response(const h2_pal_mem_api_t *allocator,
                                 const h2_gizclaw_rpc_response_t *response,
                                 const pb_msgdesc_t *fields, void *decoded,
                                 gizclaw_rpc_v1_FriendGroupObject *value,
                                 bool *has_value,
                                 h2_gizclaw_friend_group_t *out_group) {
  social_text_decode_t text[4];
  set_bounded_decoder(&value->name, &text[0], allocator, &out_group->name,
                      H2_GIZCLAW_FRIEND_GROUP_NAME_MAX_BYTES);
  set_bounded_decoder(&value->display_name, &text[1], allocator,
                      &out_group->display_name,
                      H2_GIZCLAW_FRIEND_GROUP_DISPLAY_NAME_MAX_BYTES);
  set_bounded_decoder(&value->description, &text[2], allocator,
                      &out_group->description,
                      H2_GIZCLAW_FRIEND_GROUP_DESCRIPTION_MAX_BYTES);
  set_decoder(&value->workspace_name, &text[3], allocator,
              &out_group->workspace_name);
  pb_istream_t stream = pb_istream_from_buffer(response->result_payload,
                                               response->result_payload_len);
  if (!pb_decode(&stream, fields, decoded) || !*has_value ||
      out_group->name == NULL || out_group->name[0] == '\0' ||
      (value->has_my_role &&
       value->my_role >
           gizclaw_rpc_v1_FriendGroupMemberRole_FRIEND_GROUP_MEMBER_ROLE_MEMBER)) {
    group_deinit(allocator, out_group);
    return H2_PAL_ERR_FORMAT;
  }
  out_group->my_role = value->has_my_role
                           ? (h2_gizclaw_friend_group_role_t)value->my_role
                           : H2_GIZCLAW_FRIEND_GROUP_ROLE_UNSPECIFIED;
  return H2_PAL_OK;
}

static int decode_member_response(
    const h2_pal_mem_api_t *allocator,
    const h2_gizclaw_rpc_response_t *response, const pb_msgdesc_t *fields,
    void *decoded, gizclaw_rpc_v1_FriendGroupMemberObject *value,
    bool *has_value, h2_gizclaw_friend_group_member_t *out_member) {
  social_text_decode_t text[5];
  set_decoder(&value->name, &text[0], allocator, &out_member->id);
  set_bounded_decoder(&value->friend_group_name, &text[1], allocator,
                      &out_member->friend_group_name,
                      H2_GIZCLAW_FRIEND_GROUP_NAME_MAX_BYTES);
  set_decoder(&value->peer_public_key, &text[2], allocator,
              &out_member->peer_public_key);
  set_decoder(&value->created_at, &text[3], allocator, &out_member->created_at);
  set_decoder(&value->updated_at, &text[4], allocator, &out_member->updated_at);
  pb_istream_t stream = pb_istream_from_buffer(response->result_payload,
                                               response->result_payload_len);
  if (!pb_decode(&stream, fields, decoded) || !*has_value ||
      out_member->id == NULL || out_member->id[0] == '\0' ||
      out_member->friend_group_name == NULL ||
      out_member->friend_group_name[0] == '\0' ||
      (value->has_role &&
       value->role >
           gizclaw_rpc_v1_FriendGroupMemberRole_FRIEND_GROUP_MEMBER_ROLE_MEMBER)) {
    member_deinit(allocator, out_member);
    return H2_PAL_ERR_FORMAT;
  }
  out_member->role = value->has_role
                         ? (h2_gizclaw_friend_group_role_t)value->role
                         : H2_GIZCLAW_FRIEND_GROUP_ROLE_UNSPECIFIED;
  return H2_PAL_OK;
}

static int decode_message_response(
    const h2_pal_mem_api_t *allocator,
    const h2_gizclaw_rpc_response_t *response, const pb_msgdesc_t *fields,
    void *decoded, gizclaw_rpc_v1_FriendGroupMessageObject *value,
    bool *has_value, h2_gizclaw_friend_group_message_t *out_message) {
  pb_istream_t stream = pb_istream_from_buffer(response->result_payload,
                                               response->result_payload_len);
  if (!pb_decode(&stream, fields, decoded) || !*has_value ||
      !copy_message_object(value, out_message, allocator)) {
    return H2_PAL_ERR_FORMAT;
  }
  return H2_PAL_OK;
}

static const char friend_list_tag;
h2_pal_result_t h2_gizclaw_req_create_friend_list(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t cursor,
    size_t limit, uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (!(valid_text(cursor, true) && cursor.len <= 255u && limit > 0u &&
        limit <= 64u))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_FriendListRequest message =
      gizclaw_rpc_v1_FriendListRequest_init_zero;
  social_text_encode_t text;
  if (cursor.len > 0u)
    set_encoder(&message.cursor, &text, cursor);
  message.has_limit = true;
  message.limit = (int64_t)limit;
  return h2_gizclaw_social_create_message_internal(
      service, identity, &friend_list_tag, H2_GIZCLAW_RPC_SERVER_FRIEND_LIST,
      gizclaw_rpc_v1_FriendListRequest_fields, &message, timeout_ms,
      out_request);
}

h2_pal_result_t
h2_gizclaw_resp_parse_friend_list(const h2_gizclaw_req_t *request,
                                  h2_gizclaw_resp_storage_t *storage,
                                  h2_gizclaw_friend_page_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc =
      h2_gizclaw_req_response_internal(request, &friend_list_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_rpc_bytes_t input;
  rc = h2_gizclaw_req_input_internal(request, &friend_list_tag, &input);
  if (rc != H2_PAL_OK)
    return rc;
  gizclaw_rpc_v1_FriendListRequest params =
      gizclaw_rpc_v1_FriendListRequest_init_zero;
  pb_istream_t input_stream = pb_istream_from_buffer(input.data, input.len);
  if (!pb_decode(&input_stream, gizclaw_rpc_v1_FriendListRequest_fields,
                 &params) ||
      !params.has_limit || params.limit <= 0 || params.limit > 64)
    return H2_PAL_ERR_FORMAT;
  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  const h2_pal_mem_api_t *allocator = &arena.allocator;
  h2_gizclaw_friend_page_t result = {0};
  gizclaw_rpc_v1_FriendListResponse decoded =
      gizclaw_rpc_v1_FriendListResponse_init_zero;
  friend_page_decode_t items = {
      .allocator = allocator, .page = &result, .limit = (size_t)params.limit};
  social_text_decode_t cursor_decoder;
  decoded.items.funcs.decode = decode_friend;
  decoded.items.arg = &items;
  set_bounded_decoder(&decoded.next_cursor, &cursor_decoder, allocator,
                      &result.next_cursor, 255u);
  pb_istream_t stream = pb_istream_from_buffer(response->result_payload,
                                               response->result_payload_len);
  if (!pb_decode(&stream, gizclaw_rpc_v1_FriendListResponse_fields, &decoded) ||
      !decode_cursor(decoded.has_next, result.next_cursor) ||
      !valid_owned_text(result.next_cursor))
    rc = H2_PAL_ERR_FORMAT;
  else
    result.has_next = decoded.has_next;
  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}

h2_pal_result_t h2_gizclaw_rpc_friend_list(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t cursor, size_t limit,
    uint32_t timeout_ms, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_page_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_friend_list(
      service, 0u, cursor, limit, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_friend_list(request, storage, out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

static const char friend_info_get_tag;
h2_pal_result_t h2_gizclaw_req_create_friend_info_get(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t friend_id, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (!(valid_text(friend_id, false) &&
        friend_id.len <
            sizeof(((gizclaw_rpc_v1_FriendInfoGetRequest *)0)->name)))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_FriendInfoGetRequest message =
      gizclaw_rpc_v1_FriendInfoGetRequest_init_zero;
  memcpy(message.name, friend_id.data, friend_id.len);
  message.name[friend_id.len] = '\0';
  return h2_gizclaw_social_create_message_internal(
      service, identity, &friend_info_get_tag,
      H2_GIZCLAW_RPC_SERVER_FRIEND_INFO_GET,
      gizclaw_rpc_v1_FriendInfoGetRequest_fields, &message, timeout_ms,
      out_request);
}

h2_pal_result_t
h2_gizclaw_resp_parse_friend_info_get(const h2_gizclaw_req_t *request,
                                      h2_gizclaw_resp_storage_t *storage,
                                      h2_gizclaw_friend_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_response_internal(
      request, &friend_info_get_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_rpc_bytes_t input;
  rc = h2_gizclaw_req_input_internal(request, &friend_info_get_tag, &input);
  if (rc != H2_PAL_OK)
    return rc;
  gizclaw_rpc_v1_FriendInfoGetRequest params =
      gizclaw_rpc_v1_FriendInfoGetRequest_init_zero;
  pb_istream_t input_stream = pb_istream_from_buffer(input.data, input.len);
  if (!pb_decode(&input_stream, gizclaw_rpc_v1_FriendInfoGetRequest_fields,
                 &params))
    return H2_PAL_ERR_FORMAT;
  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  const h2_pal_mem_api_t *allocator = &arena.allocator;
  h2_gizclaw_friend_t result = {0};
  gizclaw_rpc_v1_FriendInfoGetResponse decoded =
      gizclaw_rpc_v1_FriendInfoGetResponse_init_zero;
  pb_istream_t stream = pb_istream_from_buffer(response->result_payload,
                                               response->result_payload_len);
  if (!pb_decode(&stream, gizclaw_rpc_v1_FriendInfoGetResponse_fields,
                 &decoded) ||
      !decoded.has_value || decoded.name[0] == '\0' ||
      !valid_owned_text(decoded.name) ||
      (decoded.value.has_display_name &&
       !valid_owned_text(decoded.value.display_name)) ||
      (decoded.value.has_emoji && !valid_owned_text(decoded.value.emoji)))
    rc = H2_PAL_ERR_FORMAT;
  else {
    result.id = duplicate_text(allocator, params.name);
    result.peer_public_key = duplicate_text(allocator, decoded.name);
    if (decoded.value.has_display_name)
      result.name = duplicate_text(allocator, decoded.value.display_name);
    if (decoded.value.has_emoji)
      result.emoji = duplicate_text(allocator, decoded.value.emoji);
    if (result.id == NULL || result.peer_public_key == NULL ||
        (decoded.value.has_display_name && result.name == NULL) ||
        (decoded.value.has_emoji && result.emoji == NULL))
      rc = H2_PAL_ERR_NO_MEMORY;
  }
  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}

h2_pal_result_t
h2_gizclaw_rpc_friend_info_get(h2_gizclaw_service_t *service,
                               h2_gizclaw_str_t friend_id, uint32_t timeout_ms,
                               h2_gizclaw_resp_storage_t *storage,
                               h2_gizclaw_friend_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_friend_info_get(
      service, 0u, friend_id, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_friend_info_get(request, storage, out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

static const char friend_add_tag;
h2_pal_result_t h2_gizclaw_req_create_friend_add(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t invite_token, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (!(valid_text(invite_token, false) && invite_token.len <= 4096u))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_FriendAddRequest message =
      gizclaw_rpc_v1_FriendAddRequest_init_zero;
  social_text_encode_t text;
  set_encoder(&message.invite_token, &text, invite_token);
  return h2_gizclaw_social_create_message_internal(
      service, identity, &friend_add_tag, H2_GIZCLAW_RPC_SERVER_FRIEND_ADD,
      gizclaw_rpc_v1_FriendAddRequest_fields, &message, timeout_ms,
      out_request);
}

h2_pal_result_t
h2_gizclaw_resp_parse_friend_add(const h2_gizclaw_req_t *request,
                                 h2_gizclaw_resp_storage_t *storage,
                                 h2_gizclaw_friend_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc =
      h2_gizclaw_req_response_internal(request, &friend_add_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;

  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  const h2_pal_mem_api_t *allocator = &arena.allocator;
  h2_gizclaw_friend_t result = {0};
  gizclaw_rpc_v1_FriendAddResponse decoded =
      gizclaw_rpc_v1_FriendAddResponse_init_zero;
  social_text_decode_t text[5];
  set_decoder(&decoded.value.name, &text[0], allocator, &result.id);
  set_decoder(&decoded.value.peer_public_key, &text[1], allocator,
              &result.peer_public_key);
  set_decoder(&decoded.value.workspace_name, &text[2], allocator,
              &result.workspace_name);
  set_decoder(&decoded.value.created_at, &text[3], allocator,
              &result.created_at);
  set_decoder(&decoded.value.updated_at, &text[4], allocator,
              &result.updated_at);
  pb_istream_t stream = pb_istream_from_buffer(response->result_payload,
                                               response->result_payload_len);
  if (!pb_decode(&stream, gizclaw_rpc_v1_FriendAddResponse_fields, &decoded) ||
      !decoded.has_value || result.id == NULL || result.id[0] == '\0')
    rc = H2_PAL_ERR_FORMAT;
  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}

h2_pal_result_t h2_gizclaw_rpc_friend_add(h2_gizclaw_service_t *service,
                                          h2_gizclaw_str_t invite_token,
                                          uint32_t timeout_ms,
                                          h2_gizclaw_resp_storage_t *storage,
                                          h2_gizclaw_friend_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_friend_add(
      service, 0u, invite_token, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_friend_add(request, storage, out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

static const char friend_delete_tag;
h2_pal_result_t h2_gizclaw_req_create_friend_delete(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t friend_id, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (!(valid_text(friend_id, false) && friend_id.len <= 4096u))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_FriendDeleteRequest message =
      gizclaw_rpc_v1_FriendDeleteRequest_init_zero;
  social_text_encode_t text;
  set_encoder(&message.name, &text, friend_id);
  return h2_gizclaw_social_create_message_internal(
      service, identity, &friend_delete_tag,
      H2_GIZCLAW_RPC_SERVER_FRIEND_DELETE,
      gizclaw_rpc_v1_FriendDeleteRequest_fields, &message, timeout_ms,
      out_request);
}

h2_pal_result_t
h2_gizclaw_resp_parse_friend_delete(const h2_gizclaw_req_t *request,
                                    h2_gizclaw_resp_storage_t *storage,
                                    h2_gizclaw_friend_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc =
      h2_gizclaw_req_response_internal(request, &friend_delete_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;

  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  const h2_pal_mem_api_t *allocator = &arena.allocator;
  h2_gizclaw_friend_t result = {0};
  gizclaw_rpc_v1_FriendDeleteResponse decoded =
      gizclaw_rpc_v1_FriendDeleteResponse_init_zero;
  social_text_decode_t text[5];
  set_decoder(&decoded.value.name, &text[0], allocator, &result.id);
  set_decoder(&decoded.value.peer_public_key, &text[1], allocator,
              &result.peer_public_key);
  set_decoder(&decoded.value.workspace_name, &text[2], allocator,
              &result.workspace_name);
  set_decoder(&decoded.value.created_at, &text[3], allocator,
              &result.created_at);
  set_decoder(&decoded.value.updated_at, &text[4], allocator,
              &result.updated_at);
  pb_istream_t stream = pb_istream_from_buffer(response->result_payload,
                                               response->result_payload_len);
  if (!pb_decode(&stream, gizclaw_rpc_v1_FriendDeleteResponse_fields,
                 &decoded) ||
      !decoded.has_value || result.id == NULL || result.id[0] == '\0')
    rc = H2_PAL_ERR_FORMAT;
  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}

h2_pal_result_t h2_gizclaw_rpc_friend_delete(h2_gizclaw_service_t *service,
                                             h2_gizclaw_str_t friend_id,
                                             uint32_t timeout_ms,
                                             h2_gizclaw_resp_storage_t *storage,
                                             h2_gizclaw_friend_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_friend_delete(
      service, 0u, friend_id, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_friend_delete(request, storage, out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

static const char friend_invite_token_get_tag;
h2_pal_result_t h2_gizclaw_req_create_friend_invite_token_get(
    h2_gizclaw_service_t *service, uint64_t identity, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (!(true))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_FriendInviteTokenGetRequest message =
      gizclaw_rpc_v1_FriendInviteTokenGetRequest_init_zero;

  return h2_gizclaw_social_create_message_internal(
      service, identity, &friend_invite_token_get_tag,
      H2_GIZCLAW_RPC_SERVER_FRIEND_INVITE_TOKEN_GET,
      gizclaw_rpc_v1_FriendInviteTokenGetRequest_fields, &message, timeout_ms,
      out_request);
}

h2_pal_result_t h2_gizclaw_resp_parse_friend_invite_token_get(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_invite_token_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_response_internal(
      request, &friend_invite_token_get_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;

  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  const h2_pal_mem_api_t *allocator = &arena.allocator;
  h2_gizclaw_invite_token_t result = {0};
  gizclaw_rpc_v1_FriendInviteTokenGetResponse decoded =
      gizclaw_rpc_v1_FriendInviteTokenGetResponse_init_zero;
  rc = (h2_pal_result_t)decode_token_response(
      allocator, response, gizclaw_rpc_v1_FriendInviteTokenGetResponse_fields,
      &decoded, &decoded.invite_token, &decoded.expires_at, &result, true);
  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}

h2_pal_result_t h2_gizclaw_rpc_friend_invite_token_get(
    h2_gizclaw_service_t *service, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_invite_token_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_friend_invite_token_get(
      service, 0u, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_friend_invite_token_get(request, storage,
                                                       out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

static const char friend_invite_token_create_tag;
h2_pal_result_t h2_gizclaw_req_create_friend_invite_token_create(
    h2_gizclaw_service_t *service, uint64_t identity, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (!(true))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_FriendInviteTokenCreateRequest message =
      gizclaw_rpc_v1_FriendInviteTokenCreateRequest_init_zero;

  return h2_gizclaw_social_create_message_internal(
      service, identity, &friend_invite_token_create_tag,
      H2_GIZCLAW_RPC_SERVER_FRIEND_INVITE_TOKEN_CREATE,
      gizclaw_rpc_v1_FriendInviteTokenCreateRequest_fields, &message,
      timeout_ms, out_request);
}

h2_pal_result_t h2_gizclaw_resp_parse_friend_invite_token_create(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_invite_token_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_response_internal(
      request, &friend_invite_token_create_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;

  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  const h2_pal_mem_api_t *allocator = &arena.allocator;
  h2_gizclaw_invite_token_t result = {0};
  gizclaw_rpc_v1_FriendInviteTokenCreateResponse decoded =
      gizclaw_rpc_v1_FriendInviteTokenCreateResponse_init_zero;
  rc = (h2_pal_result_t)decode_token_response(
      allocator, response,
      gizclaw_rpc_v1_FriendInviteTokenCreateResponse_fields, &decoded,
      &decoded.invite_token, &decoded.expires_at, &result, false);
  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}

h2_pal_result_t h2_gizclaw_rpc_friend_invite_token_create(
    h2_gizclaw_service_t *service, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_invite_token_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_friend_invite_token_create(
      service, 0u, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_friend_invite_token_create(request, storage,
                                                          out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

static const char friend_invite_token_clear_tag;
h2_pal_result_t h2_gizclaw_req_create_friend_invite_token_clear(
    h2_gizclaw_service_t *service, uint64_t identity, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (!(true))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_FriendInviteTokenClearRequest message =
      gizclaw_rpc_v1_FriendInviteTokenClearRequest_init_zero;

  return h2_gizclaw_social_create_message_internal(
      service, identity, &friend_invite_token_clear_tag,
      H2_GIZCLAW_RPC_SERVER_FRIEND_INVITE_TOKEN_CLEAR,
      gizclaw_rpc_v1_FriendInviteTokenClearRequest_fields, &message, timeout_ms,
      out_request);
}

h2_pal_result_t h2_gizclaw_resp_parse_friend_invite_token_clear(
    const h2_gizclaw_req_t *request) {

  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_response_internal(
      request, &friend_invite_token_clear_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;

  gizclaw_rpc_v1_FriendInviteTokenClearResponse decoded =
      gizclaw_rpc_v1_FriendInviteTokenClearResponse_init_zero;
  pb_istream_t stream = pb_istream_from_buffer(response->result_payload,
                                               response->result_payload_len);
  return pb_decode(&stream,
                   gizclaw_rpc_v1_FriendInviteTokenClearResponse_fields,
                   &decoded)
             ? H2_PAL_OK
             : H2_PAL_ERR_FORMAT;
}

h2_pal_result_t
h2_gizclaw_rpc_friend_invite_token_clear(h2_gizclaw_service_t *service,
                                         uint32_t timeout_ms) {

  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_friend_invite_token_clear(
      service, 0u, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_friend_invite_token_clear(request);
  h2_gizclaw_req_release(request);
  return rc;
}

static const char friend_group_get_tag;
h2_pal_result_t h2_gizclaw_req_create_friend_group_get(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (!(valid_group_name(group_name)))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_FriendGroupGetRequest message =
      gizclaw_rpc_v1_FriendGroupGetRequest_init_zero;
  social_text_encode_t text;
  set_encoder(&message.name, &text, group_name);
  return h2_gizclaw_social_create_message_internal(
      service, identity, &friend_group_get_tag,
      H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_GET,
      gizclaw_rpc_v1_FriendGroupGetRequest_fields, &message, timeout_ms,
      out_request);
}

h2_pal_result_t
h2_gizclaw_resp_parse_friend_group_get(const h2_gizclaw_req_t *request,
                                       h2_gizclaw_resp_storage_t *storage,
                                       h2_gizclaw_friend_group_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_response_internal(
      request, &friend_group_get_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_friend_group_t result = {0};
  gizclaw_rpc_v1_FriendGroupGetResponse decoded =
      gizclaw_rpc_v1_FriendGroupGetResponse_init_zero;
  rc = (h2_pal_result_t)decode_group_response(
      &arena.allocator, response, gizclaw_rpc_v1_FriendGroupGetResponse_fields,
      &decoded, &decoded.value, &decoded.has_value, &result);
  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}

h2_pal_result_t h2_gizclaw_rpc_friend_group_get(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t group_name,
    uint32_t timeout_ms, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_friend_group_get(
      service, 0u, group_name, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_friend_group_get(request, storage, out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

static const char friend_group_create_tag;
h2_pal_result_t h2_gizclaw_req_create_friend_group_create(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    h2_gizclaw_str_t display_name, h2_gizclaw_str_t description,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (!(valid_group_name(name) &&
        display_name.len <= H2_GIZCLAW_FRIEND_GROUP_DISPLAY_NAME_MAX_BYTES &&
        description.len <= H2_GIZCLAW_FRIEND_GROUP_DESCRIPTION_MAX_BYTES &&
        valid_text(display_name, true) && valid_text(description, true)))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_FriendGroupCreateRequest message =
      gizclaw_rpc_v1_FriendGroupCreateRequest_init_zero;
  social_text_encode_t text[3];
  set_encoder(&message.name, &text[0], name);
  if (display_name.len > 0u)
    set_encoder(&message.display_name, &text[1], display_name);
  if (description.len > 0u)
    set_encoder(&message.description, &text[2], description);
  return h2_gizclaw_social_create_message_internal(
      service, identity, &friend_group_create_tag,
      H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_CREATE,
      gizclaw_rpc_v1_FriendGroupCreateRequest_fields, &message, timeout_ms,
      out_request);
}

h2_pal_result_t h2_gizclaw_resp_parse_friend_group_create(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_response_internal(
      request, &friend_group_create_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_friend_group_t result = {0};
  gizclaw_rpc_v1_FriendGroupCreateResponse decoded =
      gizclaw_rpc_v1_FriendGroupCreateResponse_init_zero;
  rc = (h2_pal_result_t)decode_group_response(
      &arena.allocator, response,
      gizclaw_rpc_v1_FriendGroupCreateResponse_fields, &decoded, &decoded.value,
      &decoded.has_value, &result);
  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}

h2_pal_result_t h2_gizclaw_rpc_friend_group_create(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t name,
    h2_gizclaw_str_t display_name, h2_gizclaw_str_t description,
    uint32_t timeout_ms, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_friend_group_create(
      service, 0u, name, display_name, description, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc =
        h2_gizclaw_resp_parse_friend_group_create(request, storage, out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

static const char friend_group_put_tag;
h2_pal_result_t h2_gizclaw_req_create_friend_group_put(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    h2_gizclaw_str_t display_name, h2_gizclaw_str_t description,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (!(valid_group_name(name) &&
        display_name.len <= H2_GIZCLAW_FRIEND_GROUP_DISPLAY_NAME_MAX_BYTES &&
        description.len <= H2_GIZCLAW_FRIEND_GROUP_DESCRIPTION_MAX_BYTES &&
        valid_text(display_name, true) && valid_text(description, true)))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_FriendGroupPutRequest message =
      gizclaw_rpc_v1_FriendGroupPutRequest_init_zero;
  social_text_encode_t text[3];
  set_encoder(&message.name, &text[0], name);
  if (display_name.len > 0u)
    set_encoder(&message.display_name, &text[1], display_name);
  if (description.len > 0u)
    set_encoder(&message.description, &text[2], description);
  return h2_gizclaw_social_create_message_internal(
      service, identity, &friend_group_put_tag,
      H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_PUT,
      gizclaw_rpc_v1_FriendGroupPutRequest_fields, &message, timeout_ms,
      out_request);
}

h2_pal_result_t
h2_gizclaw_resp_parse_friend_group_put(const h2_gizclaw_req_t *request,
                                       h2_gizclaw_resp_storage_t *storage,
                                       h2_gizclaw_friend_group_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_response_internal(
      request, &friend_group_put_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_friend_group_t result = {0};
  gizclaw_rpc_v1_FriendGroupPutResponse decoded =
      gizclaw_rpc_v1_FriendGroupPutResponse_init_zero;
  rc = (h2_pal_result_t)decode_group_response(
      &arena.allocator, response, gizclaw_rpc_v1_FriendGroupPutResponse_fields,
      &decoded, &decoded.value, &decoded.has_value, &result);
  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}

h2_pal_result_t h2_gizclaw_rpc_friend_group_put(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t name,
    h2_gizclaw_str_t display_name, h2_gizclaw_str_t description,
    uint32_t timeout_ms, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_friend_group_put(
      service, 0u, name, display_name, description, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_friend_group_put(request, storage, out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

static const char friend_group_delete_tag;
h2_pal_result_t h2_gizclaw_req_create_friend_group_delete(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (!(valid_group_name(group_name)))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_FriendGroupDeleteRequest message =
      gizclaw_rpc_v1_FriendGroupDeleteRequest_init_zero;
  social_text_encode_t text;
  set_encoder(&message.name, &text, group_name);
  return h2_gizclaw_social_create_message_internal(
      service, identity, &friend_group_delete_tag,
      H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_DELETE,
      gizclaw_rpc_v1_FriendGroupDeleteRequest_fields, &message, timeout_ms,
      out_request);
}

h2_pal_result_t h2_gizclaw_resp_parse_friend_group_delete(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_response_internal(
      request, &friend_group_delete_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_friend_group_t result = {0};
  gizclaw_rpc_v1_FriendGroupDeleteResponse decoded =
      gizclaw_rpc_v1_FriendGroupDeleteResponse_init_zero;
  rc = (h2_pal_result_t)decode_group_response(
      &arena.allocator, response,
      gizclaw_rpc_v1_FriendGroupDeleteResponse_fields, &decoded, &decoded.value,
      &decoded.has_value, &result);
  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}

h2_pal_result_t h2_gizclaw_rpc_friend_group_delete(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t group_name,
    uint32_t timeout_ms, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_friend_group_delete(
      service, 0u, group_name, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc =
        h2_gizclaw_resp_parse_friend_group_delete(request, storage, out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

static const char friend_group_join_tag;
h2_pal_result_t h2_gizclaw_req_create_friend_group_join(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t invite_token, h2_gizclaw_str_t name, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (!(valid_group_name(name) && valid_text(invite_token, false) &&
        invite_token.len <= 4096u))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_FriendGroupJoinRequest message =
      gizclaw_rpc_v1_FriendGroupJoinRequest_init_zero;
  social_text_encode_t text[2];
  set_encoder(&message.invite_token, &text[0], invite_token);
  set_encoder(&message.name, &text[1], name);
  return h2_gizclaw_social_create_message_internal(
      service, identity, &friend_group_join_tag,
      H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_JOIN,
      gizclaw_rpc_v1_FriendGroupJoinRequest_fields, &message, timeout_ms,
      out_request);
}

h2_pal_result_t
h2_gizclaw_resp_parse_friend_group_join(const h2_gizclaw_req_t *request,
                                        h2_gizclaw_resp_storage_t *storage,
                                        h2_gizclaw_friend_group_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_response_internal(
      request, &friend_group_join_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_friend_group_t result = {0};
  gizclaw_rpc_v1_FriendGroupJoinResponse decoded =
      gizclaw_rpc_v1_FriendGroupJoinResponse_init_zero;
  rc = (h2_pal_result_t)decode_group_response(
      &arena.allocator, response, gizclaw_rpc_v1_FriendGroupJoinResponse_fields,
      &decoded, &decoded.group, &decoded.has_group, &result);
  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}

h2_pal_result_t h2_gizclaw_rpc_friend_group_join(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t invite_token,
    h2_gizclaw_str_t name, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_friend_group_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_friend_group_join(
      service, 0u, invite_token, name, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_friend_group_join(request, storage, out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

static const char friend_group_invite_token_get_tag;
h2_pal_result_t h2_gizclaw_req_create_friend_group_invite_token_get(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (!(valid_group_name(group_name)))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_FriendGroupInviteTokenGetRequest message =
      gizclaw_rpc_v1_FriendGroupInviteTokenGetRequest_init_zero;
  social_text_encode_t text;
  set_encoder(&message.friend_group_name, &text, group_name);
  return h2_gizclaw_social_create_message_internal(
      service, identity, &friend_group_invite_token_get_tag,
      H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_INVITE_TOKEN_GET,
      gizclaw_rpc_v1_FriendGroupInviteTokenGetRequest_fields, &message,
      timeout_ms, out_request);
}

h2_pal_result_t h2_gizclaw_resp_parse_friend_group_invite_token_get(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_invite_token_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_response_internal(
      request, &friend_group_invite_token_get_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_invite_token_t result = {0};
  gizclaw_rpc_v1_FriendGroupInviteTokenGetResponse decoded =
      gizclaw_rpc_v1_FriendGroupInviteTokenGetResponse_init_zero;
  rc = (h2_pal_result_t)decode_token_response(
      &arena.allocator, response,
      gizclaw_rpc_v1_FriendGroupInviteTokenGetResponse_fields, &decoded,
      &decoded.invite_token, &decoded.expires_at, &result, true);
  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}

h2_pal_result_t h2_gizclaw_rpc_friend_group_invite_token_get(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t group_name,
    uint32_t timeout_ms, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_invite_token_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_friend_group_invite_token_get(
      service, 0u, group_name, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_friend_group_invite_token_get(request, storage,
                                                             out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

static const char friend_group_invite_token_create_tag;
h2_pal_result_t h2_gizclaw_req_create_friend_group_invite_token_create(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (!(valid_group_name(group_name)))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_FriendGroupInviteTokenCreateRequest message =
      gizclaw_rpc_v1_FriendGroupInviteTokenCreateRequest_init_zero;
  social_text_encode_t text;
  set_encoder(&message.friend_group_name, &text, group_name);
  return h2_gizclaw_social_create_message_internal(
      service, identity, &friend_group_invite_token_create_tag,
      H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_INVITE_TOKEN_CREATE,
      gizclaw_rpc_v1_FriendGroupInviteTokenCreateRequest_fields, &message,
      timeout_ms, out_request);
}

h2_pal_result_t h2_gizclaw_resp_parse_friend_group_invite_token_create(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_invite_token_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_response_internal(
      request, &friend_group_invite_token_create_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_invite_token_t result = {0};
  gizclaw_rpc_v1_FriendGroupInviteTokenCreateResponse decoded =
      gizclaw_rpc_v1_FriendGroupInviteTokenCreateResponse_init_zero;
  rc = (h2_pal_result_t)decode_token_response(
      &arena.allocator, response,
      gizclaw_rpc_v1_FriendGroupInviteTokenCreateResponse_fields, &decoded,
      &decoded.invite_token, &decoded.expires_at, &result, false);
  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}

h2_pal_result_t h2_gizclaw_rpc_friend_group_invite_token_create(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t group_name,
    uint32_t timeout_ms, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_invite_token_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_friend_group_invite_token_create(
      service, 0u, group_name, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_friend_group_invite_token_create(
        request, storage, out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

static const char friend_group_invite_token_clear_tag;
h2_pal_result_t h2_gizclaw_req_create_friend_group_invite_token_clear(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (!(valid_group_name(group_name)))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_FriendGroupInviteTokenClearRequest message =
      gizclaw_rpc_v1_FriendGroupInviteTokenClearRequest_init_zero;
  social_text_encode_t text;
  set_encoder(&message.friend_group_name, &text, group_name);
  return h2_gizclaw_social_create_message_internal(
      service, identity, &friend_group_invite_token_clear_tag,
      H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_INVITE_TOKEN_CLEAR,
      gizclaw_rpc_v1_FriendGroupInviteTokenClearRequest_fields, &message,
      timeout_ms, out_request);
}

h2_pal_result_t h2_gizclaw_resp_parse_friend_group_invite_token_clear(
    const h2_gizclaw_req_t *request) {

  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_response_internal(
      request, &friend_group_invite_token_clear_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;

  gizclaw_rpc_v1_FriendGroupInviteTokenClearResponse decoded =
      gizclaw_rpc_v1_FriendGroupInviteTokenClearResponse_init_zero;
  pb_istream_t stream = pb_istream_from_buffer(response->result_payload,
                                               response->result_payload_len);
  return pb_decode(&stream,
                   gizclaw_rpc_v1_FriendGroupInviteTokenClearResponse_fields,
                   &decoded)
             ? H2_PAL_OK
             : H2_PAL_ERR_FORMAT;
}

h2_pal_result_t
h2_gizclaw_rpc_friend_group_invite_token_clear(h2_gizclaw_service_t *service,
                                               h2_gizclaw_str_t group_name,
                                               uint32_t timeout_ms) {

  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_friend_group_invite_token_clear(
      service, 0u, group_name, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_friend_group_invite_token_clear(request);
  h2_gizclaw_req_release(request);
  return rc;
}

static const char friend_group_member_list_tag;
h2_pal_result_t h2_gizclaw_req_create_friend_group_member_list(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_str_t cursor, size_t limit,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (!(valid_group_name(group_name) && valid_text(cursor, true) &&
        cursor.len <= 255u && limit > 0u && limit <= 64u))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_FriendGroupMemberListRequest message =
      gizclaw_rpc_v1_FriendGroupMemberListRequest_init_zero;
  social_text_encode_t text[2];
  set_encoder(&message.friend_group_name, &text[0], group_name);
  if (cursor.len > 0u)
    set_encoder(&message.cursor, &text[1], cursor);
  message.has_limit = true;
  message.limit = (int64_t)limit;

  return h2_gizclaw_social_create_message_internal(
      service, identity, &friend_group_member_list_tag,
      H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_MEMBERS_LIST,
      gizclaw_rpc_v1_FriendGroupMemberListRequest_fields, &message, timeout_ms,
      out_request);
}
h2_pal_result_t h2_gizclaw_resp_parse_friend_group_member_list(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_member_page_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_response_internal(
      request, &friend_group_member_list_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_rpc_bytes_t input;
  rc = h2_gizclaw_req_input_internal(request, &friend_group_member_list_tag,
                                     &input);
  if (rc != H2_PAL_OK)
    return rc;
  gizclaw_rpc_v1_FriendGroupMemberListRequest params =
      gizclaw_rpc_v1_FriendGroupMemberListRequest_init_zero;
  pb_istream_t input_stream = pb_istream_from_buffer(input.data, input.len);
  if (!pb_decode(&input_stream,
                 gizclaw_rpc_v1_FriendGroupMemberListRequest_fields, &params) ||
      !params.has_limit || params.limit <= 0 || params.limit > 64)
    return H2_PAL_ERR_FORMAT;
  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  const h2_pal_mem_api_t *allocator = &arena.allocator;
  h2_gizclaw_friend_group_member_page_t result = {0};
  gizclaw_rpc_v1_FriendGroupMemberListResponse decoded =
      gizclaw_rpc_v1_FriendGroupMemberListResponse_init_zero;
  member_page_decode_t items = {
      .allocator = allocator, .page = &result, .limit = (size_t)params.limit};
  decoded.items.funcs.decode = decode_member;
  decoded.items.arg = &items;
  social_text_decode_t cursor_decoder;
  set_bounded_decoder(&decoded.next_cursor, &cursor_decoder, allocator,
                      &result.next_cursor, 255u);
  pb_istream_t stream = pb_istream_from_buffer(response->result_payload,
                                               response->result_payload_len);
  if (!pb_decode(&stream, gizclaw_rpc_v1_FriendGroupMemberListResponse_fields,
                 &decoded) ||
      !decode_cursor(decoded.has_next, result.next_cursor))
    rc = H2_PAL_ERR_FORMAT;
  else
    result.has_next = decoded.has_next;
  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}
h2_pal_result_t h2_gizclaw_rpc_friend_group_member_list(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t group_name,
    h2_gizclaw_str_t cursor, size_t limit, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_member_page_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_friend_group_member_list(
      service, 0u, group_name, cursor, limit, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_friend_group_member_list(request, storage,
                                                        out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

static const char friend_group_member_put_tag;
h2_pal_result_t h2_gizclaw_req_create_friend_group_member_put(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_str_t member_id,
    h2_gizclaw_friend_group_role_t role, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (!(valid_group_name(group_name) && valid_text(member_id, false) &&
        member_id.len <= 4096u &&
        (role == H2_GIZCLAW_FRIEND_GROUP_ROLE_ADMIN ||
         role == H2_GIZCLAW_FRIEND_GROUP_ROLE_MEMBER)))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_FriendGroupMemberPutRequest message =
      gizclaw_rpc_v1_FriendGroupMemberPutRequest_init_zero;
  social_text_encode_t text[2];
  set_encoder(&message.friend_group_name, &text[0], group_name);
  set_encoder(&message.name, &text[1], member_id);
  message.role =
      role == H2_GIZCLAW_FRIEND_GROUP_ROLE_ADMIN
          ? gizclaw_rpc_v1_FriendGroupMemberMutableRole_FRIEND_GROUP_MEMBER_MUTABLE_ROLE_ADMIN
          : gizclaw_rpc_v1_FriendGroupMemberMutableRole_FRIEND_GROUP_MEMBER_MUTABLE_ROLE_MEMBER;

  return h2_gizclaw_social_create_message_internal(
      service, identity, &friend_group_member_put_tag,
      H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_MEMBERS_PUT,
      gizclaw_rpc_v1_FriendGroupMemberPutRequest_fields, &message, timeout_ms,
      out_request);
}
h2_pal_result_t h2_gizclaw_resp_parse_friend_group_member_put(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_member_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_response_internal(
      request, &friend_group_member_put_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;

  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  const h2_pal_mem_api_t *allocator = &arena.allocator;
  h2_gizclaw_friend_group_member_t result = {0};
  gizclaw_rpc_v1_FriendGroupMemberPutResponse decoded =
      gizclaw_rpc_v1_FriendGroupMemberPutResponse_init_zero;
  rc = (h2_pal_result_t)decode_member_response(
      allocator, response, gizclaw_rpc_v1_FriendGroupMemberPutResponse_fields,
      &decoded, &decoded.value, &decoded.has_value, &result);
  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}
h2_pal_result_t h2_gizclaw_rpc_friend_group_member_put(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t group_name,
    h2_gizclaw_str_t member_id, h2_gizclaw_friend_group_role_t role,
    uint32_t timeout_ms, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_member_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_friend_group_member_put(
      service, 0u, group_name, member_id, role, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_friend_group_member_put(request, storage,
                                                       out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

static const char friend_group_member_delete_tag;
h2_pal_result_t h2_gizclaw_req_create_friend_group_member_delete(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_str_t member_id,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (!(valid_group_name(group_name) && valid_text(member_id, false) &&
        member_id.len <= 4096u))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_FriendGroupMemberDeleteRequest message =
      gizclaw_rpc_v1_FriendGroupMemberDeleteRequest_init_zero;
  social_text_encode_t text[2];
  set_encoder(&message.friend_group_name, &text[0], group_name);
  set_encoder(&message.name, &text[1], member_id);

  return h2_gizclaw_social_create_message_internal(
      service, identity, &friend_group_member_delete_tag,
      H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_MEMBERS_DELETE,
      gizclaw_rpc_v1_FriendGroupMemberDeleteRequest_fields, &message,
      timeout_ms, out_request);
}
h2_pal_result_t h2_gizclaw_resp_parse_friend_group_member_delete(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_member_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_response_internal(
      request, &friend_group_member_delete_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;

  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  const h2_pal_mem_api_t *allocator = &arena.allocator;
  h2_gizclaw_friend_group_member_t result = {0};
  gizclaw_rpc_v1_FriendGroupMemberDeleteResponse decoded =
      gizclaw_rpc_v1_FriendGroupMemberDeleteResponse_init_zero;
  rc = (h2_pal_result_t)decode_member_response(
      allocator, response,
      gizclaw_rpc_v1_FriendGroupMemberDeleteResponse_fields, &decoded,
      &decoded.value, &decoded.has_value, &result);
  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}
h2_pal_result_t h2_gizclaw_rpc_friend_group_member_delete(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t group_name,
    h2_gizclaw_str_t member_id, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_member_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_friend_group_member_delete(
      service, 0u, group_name, member_id, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_friend_group_member_delete(request, storage,
                                                          out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

static const char friend_group_message_list_tag;
h2_pal_result_t h2_gizclaw_req_create_friend_group_message_list(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_str_t cursor, size_t limit,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (!(valid_group_name(group_name) && valid_text(cursor, true) &&
        cursor.len <= 255u && limit > 0u && limit <= 64u))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_FriendGroupMessageListRequest message =
      gizclaw_rpc_v1_FriendGroupMessageListRequest_init_zero;
  if (!copy_fixed_text(message.friend_group_name,
                       sizeof(message.friend_group_name), group_name) ||
      (cursor.len > 0u &&
       !copy_fixed_text(message.cursor, sizeof(message.cursor), cursor)))
    return H2_PAL_ERR_INVALID_ARG;
  message.has_cursor = cursor.len > 0u;
  message.has_limit = true;
  message.limit = (int64_t)limit;

  return h2_gizclaw_social_create_message_internal(
      service, identity, &friend_group_message_list_tag,
      H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_MESSAGES_LIST,
      gizclaw_rpc_v1_FriendGroupMessageListRequest_fields, &message, timeout_ms,
      out_request);
}
h2_pal_result_t h2_gizclaw_resp_parse_friend_group_message_list(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_message_page_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_response_internal(
      request, &friend_group_message_list_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_rpc_bytes_t input;
  rc = h2_gizclaw_req_input_internal(request, &friend_group_message_list_tag,
                                     &input);
  if (rc != H2_PAL_OK)
    return rc;
  gizclaw_rpc_v1_FriendGroupMessageListRequest params =
      gizclaw_rpc_v1_FriendGroupMessageListRequest_init_zero;
  pb_istream_t input_stream = pb_istream_from_buffer(input.data, input.len);
  if (!pb_decode(&input_stream,
                 gizclaw_rpc_v1_FriendGroupMessageListRequest_fields,
                 &params) ||
      !params.has_limit || params.limit <= 0 || params.limit > 64)
    return H2_PAL_ERR_FORMAT;
  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  const h2_pal_mem_api_t *allocator = &arena.allocator;
  h2_gizclaw_friend_group_message_page_t result = {0};
  gizclaw_rpc_v1_FriendGroupMessageListResponse decoded =
      gizclaw_rpc_v1_FriendGroupMessageListResponse_init_zero;
  message_page_decode_t items = {
      .allocator = allocator, .page = &result, .limit = (size_t)params.limit};
  decoded.items.funcs.decode = decode_message;
  decoded.items.arg = &items;
  pb_istream_t stream = pb_istream_from_buffer(response->result_payload,
                                               response->result_payload_len);
  if (!pb_decode(&stream, gizclaw_rpc_v1_FriendGroupMessageListResponse_fields,
                 &decoded) ||
      (decoded.has_next &&
       (!decoded.has_next_cursor || decoded.next_cursor[0] == '\0')) ||
      (decoded.has_next_cursor && !valid_owned_text(decoded.next_cursor)))
    rc = H2_PAL_ERR_FORMAT;
  else {
    result.has_next = decoded.has_next;
    if (decoded.has_next_cursor) {
      result.next_cursor = duplicate_text(allocator, decoded.next_cursor);
      if (result.next_cursor == NULL)
        rc = H2_PAL_ERR_NO_MEMORY;
    }
  }
  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}
h2_pal_result_t h2_gizclaw_rpc_friend_group_message_list(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t group_name,
    h2_gizclaw_str_t cursor, size_t limit, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_message_page_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_friend_group_message_list(
      service, 0u, group_name, cursor, limit, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_friend_group_message_list(request, storage,
                                                         out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

static const char friend_group_message_get_tag;
h2_pal_result_t h2_gizclaw_req_create_friend_group_message_get(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_str_t history_id,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (!(valid_group_name(group_name) && valid_text(history_id, false)))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_FriendGroupMessageGetRequest message =
      gizclaw_rpc_v1_FriendGroupMessageGetRequest_init_zero;
  if (!copy_fixed_text(message.friend_group_name,
                       sizeof(message.friend_group_name), group_name) ||
      !copy_fixed_text(message.history_name, sizeof(message.history_name),
                       history_id))
    return H2_PAL_ERR_INVALID_ARG;

  return h2_gizclaw_social_create_message_internal(
      service, identity, &friend_group_message_get_tag,
      H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_MESSAGES_GET,
      gizclaw_rpc_v1_FriendGroupMessageGetRequest_fields, &message, timeout_ms,
      out_request);
}
h2_pal_result_t h2_gizclaw_resp_parse_friend_group_message_get(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_message_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_response_internal(
      request, &friend_group_message_get_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;

  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  const h2_pal_mem_api_t *allocator = &arena.allocator;
  h2_gizclaw_friend_group_message_t result = {0};
  gizclaw_rpc_v1_FriendGroupMessageGetResponse decoded =
      gizclaw_rpc_v1_FriendGroupMessageGetResponse_init_zero;
  rc = (h2_pal_result_t)decode_message_response(
      allocator, response, gizclaw_rpc_v1_FriendGroupMessageGetResponse_fields,
      &decoded, &decoded.value, &decoded.has_value, &result);
  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}
h2_pal_result_t h2_gizclaw_rpc_friend_group_message_get(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t group_name,
    h2_gizclaw_str_t history_id, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_message_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_friend_group_message_get(
      service, 0u, group_name, history_id, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_friend_group_message_get(request, storage,
                                                        out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

typedef struct group_audio_download {
  const h2_pal_mem_api_t *allocator;
  gizclaw_rpc_v1_FriendGroupMessageAudioDownloadRequest input;
  gizclaw_rpc_v1_FriendGroupMessageAudioDownloadResponse metadata;
} group_audio_download_t;
typedef struct group_audio_sync_writer {
  h2_gizclaw_friend_group_message_audio_write_fn write;
  void *user;
} group_audio_sync_writer_t;
static h2_pal_result_t group_audio_sync_output(void *opaque,
                                               const uint8_t *data, size_t len,
                                               size_t *out_written) {
  group_audio_sync_writer_t *sink = opaque;
  h2_pal_result_t rc =
      (h2_pal_result_t)sink->write(sink->user, data, len);
  *out_written = rc == H2_PAL_OK ? len : 0u;
  return rc;
}
static void group_audio_destroy(void *user) {
  group_audio_download_t *context = user;
  h2_pal_mem_free(context->allocator, context);
}
static h2_pal_result_t group_audio_metadata(void *user,
                                            h2_gizclaw_rpc_bytes_t payload,
                                            uint64_t *out_size) {
  group_audio_download_t *context = user;
  pb_istream_t stream = pb_istream_from_buffer(payload.data, payload.len);
  gizclaw_rpc_v1_FriendGroupMessageAudioDownloadResponse *metadata =
      &context->metadata;
  if (!pb_decode(&stream,
                 gizclaw_rpc_v1_FriendGroupMessageAudioDownloadResponse_fields,
                 metadata) ||
      metadata->size_bytes <= 0 ||
      strcmp(metadata->friend_group_name, context->input.friend_group_name) !=
          0 ||
      strcmp(metadata->history_name, context->input.history_name) != 0 ||
      strncmp(metadata->mime_type, "audio/", 6u) != 0 ||
      !valid_owned_text(metadata->mime_type))
    return H2_PAL_ERR_FORMAT;
  *out_size = (uint64_t)metadata->size_bytes;
  return H2_PAL_OK;
}
static const h2_gizclaw_download_codec_t group_audio_codec = {
    .metadata = group_audio_metadata,
    .destroy = group_audio_destroy};
static const char friend_group_message_audio_download_tag;

h2_pal_result_t h2_gizclaw_req_create_friend_group_message_audio_download(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t group_name, h2_gizclaw_str_t history_id,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (service == NULL || out_request == NULL || !valid_group_name(group_name) ||
      !valid_text(history_id, false) ||
      timeout_ms == 0u || timeout_ms > INT32_MAX)
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_FriendGroupMessageAudioDownloadRequest input =
      gizclaw_rpc_v1_FriendGroupMessageAudioDownloadRequest_init_zero;
  if (!copy_fixed_text(input.friend_group_name, sizeof(input.friend_group_name),
                       group_name) ||
      !copy_fixed_text(input.history_name, sizeof(input.history_name),
                       history_id))
    return H2_PAL_ERR_INVALID_ARG;
  const h2_pal_mem_api_t *allocator = service->client_config.allocator;
  group_audio_download_t *context =
      h2_pal_mem_alloc(allocator, sizeof(*context));
  if (context == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(context, 0, sizeof(*context));
  context->allocator = allocator;
  context->input = input;
  uint8_t *payload = NULL;
  size_t payload_len = 0u;
  h2_pal_result_t rc = (h2_pal_result_t)encode_message(
      allocator, gizclaw_rpc_v1_FriendGroupMessageAudioDownloadRequest_fields,
      &input, &payload, &payload_len);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_create_download_internal(
        service, identity, &friend_group_message_audio_download_tag,
        H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_MESSAGES_AUDIO_DOWNLOAD,
        (h2_gizclaw_rpc_bytes_t){payload, payload_len}, timeout_ms,
        &group_audio_codec, context, out_request);
  h2_pal_mem_free(allocator, payload);
  if (rc != H2_PAL_OK)
    group_audio_destroy(context);
  return rc;
}
h2_pal_result_t h2_gizclaw_resp_parse_friend_group_message_audio_download(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_message_audio_info_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const void *user;
  uint64_t received;
  h2_pal_result_t rc = h2_gizclaw_download_result_internal(
      request, &friend_group_message_audio_download_tag, &user, &received);
  if (rc != H2_PAL_OK)
    return rc;
  const group_audio_download_t *context = user;
  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_friend_group_message_audio_info_t result = {
      .size_bytes = (uint64_t)context->metadata.size_bytes,
      .received_bytes = received};
  result.friend_group_name =
      duplicate_text(&arena.allocator, context->metadata.friend_group_name);
  result.history_id =
      duplicate_text(&arena.allocator, context->metadata.history_name);
  result.mime_type =
      duplicate_text(&arena.allocator, context->metadata.mime_type);
  if (result.friend_group_name == NULL || result.history_id == NULL ||
      result.mime_type == NULL)
    rc = H2_PAL_ERR_NO_MEMORY;
  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}
h2_pal_result_t h2_gizclaw_rpc_friend_group_message_audio_download(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t group_name,
    h2_gizclaw_str_t history_id,
    h2_gizclaw_friend_group_message_audio_write_fn write, void *write_user,
    uint32_t timeout_ms, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_message_audio_info_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  /* The synchronous adapter calls the writer for every audio frame; a
   * missing writer must fail here instead of when the first frame arrives. */
  if (write == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  group_audio_sync_writer_t writer = {.write = write, .user = write_user};
  h2_pal_result_t rc =
      h2_gizclaw_req_create_friend_group_message_audio_download(
          service, 0u, group_name, history_id, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, &writer, NULL, group_audio_sync_output, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait_dispatch_internal(request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_friend_group_message_audio_download(
        request, storage, out_result);
  h2_gizclaw_req_release(request);
  return rc;
}
