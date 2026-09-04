#include "h2_gizclaw_internal.h"
#include "h2_gizclaw_response_internal.h"
#include "h2_gizclaw_rpc.h"
#include "h2_gizclaw_service_internal.h"
#include "h2_gizclaw_social_internal.h"

#include "payload/social.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

typedef struct h2_gizclaw_social_text_decode {
  const h2_pal_mem_api_t *allocator;
  char **out_text;
  size_t max_len;
} h2_gizclaw_social_text_decode_t;

typedef struct h2_gizclaw_social_page_decode {
  const h2_pal_mem_api_t *allocator;
  h2_gizclaw_friend_group_page_t *page;
  size_t max_count;
  size_t capacity;
} h2_gizclaw_social_page_decode_t;

typedef struct h2_gizclaw_social_text_encode {
  const char *data;
  size_t len;
} h2_gizclaw_social_text_encode_t;

typedef struct h2_gizclaw_contact_page_decode {
  const h2_pal_mem_api_t *allocator;
  h2_gizclaw_contact_page_t *page;
  size_t max_count;
  size_t capacity;
} h2_gizclaw_contact_page_decode_t;

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

static bool valid_utf8(const char *text) {
  return text == NULL || valid_utf8_span(text, strlen(text));
}

static void contact_deinit(const h2_pal_mem_api_t *allocator,
                           h2_gizclaw_contact_t *contact) {
  if (contact == NULL)
    return;
  h2_pal_mem_free(allocator, contact->name);
  h2_pal_mem_free(allocator, contact->display_name);
  h2_pal_mem_free(allocator, contact->phone_number);
  h2_pal_mem_free(allocator, contact->created_at);
  h2_pal_mem_free(allocator, contact->updated_at);
  memset(contact, 0, sizeof(*contact));
}

static void friend_group_deinit(const h2_pal_mem_api_t *allocator,
                                h2_gizclaw_friend_group_t *group) {
  if (group == NULL)
    return;
  h2_pal_mem_free(allocator, group->name);
  h2_pal_mem_free(allocator, group->display_name);
  h2_pal_mem_free(allocator, group->description);
  h2_pal_mem_free(allocator, group->workspace_name);
  memset(group, 0, sizeof(*group));
}

static bool decode_text(pb_istream_t *stream, const pb_field_t *field,
                        void **arg) {
  (void)field;
  h2_gizclaw_social_text_decode_t *context = *arg;
  if (context == NULL || context->allocator == NULL ||
      context->out_text == NULL || *context->out_text != NULL ||
      stream->bytes_left == SIZE_MAX || stream->bytes_left > context->max_len) {
    return false;
  }
  char *text = h2_pal_mem_alloc(context->allocator, stream->bytes_left + 1u);
  if (text == NULL)
    return false;
  const size_t len = stream->bytes_left;
  if (!pb_read(stream, (pb_byte_t *)text, len)) {
    h2_pal_mem_free(context->allocator, text);
    return false;
  }
  if (memchr(text, '\0', len) != NULL) {
    h2_pal_mem_free(context->allocator, text);
    return false;
  }
  text[len] = '\0';
  *context->out_text = text;
  return true;
}

static void set_text_decoder(pb_callback_t *callback,
                             h2_gizclaw_social_text_decode_t *context,
                             const h2_pal_mem_api_t *allocator,
                             char **out_text) {
  *context = (h2_gizclaw_social_text_decode_t){
      .allocator = allocator,
      .out_text = out_text,
      .max_len = SIZE_MAX - 1u,
  };
  callback->funcs.decode = decode_text;
  callback->arg = context;
}

static void set_bounded_text_decoder(pb_callback_t *callback,
                                     h2_gizclaw_social_text_decode_t *context,
                                     const h2_pal_mem_api_t *allocator,
                                     char **out_text, size_t max_len) {
  set_text_decoder(callback, context, allocator, out_text);
  context->max_len = max_len;
}

static bool decode_group(pb_istream_t *stream, const pb_field_t *field,
                         void **arg) {
  (void)field;
  h2_gizclaw_social_page_decode_t *context = *arg;
  if (context == NULL || context->allocator == NULL || context->page == NULL ||
      context->page->count >= context->max_count ||
      context->page->count == SIZE_MAX ||
      context->page->count + 1u > SIZE_MAX / sizeof(context->page->items[0])) {
    return false;
  }
  const size_t count = context->page->count;
  h2_gizclaw_friend_group_t *items = context->page->items;
  if (count == context->capacity) {
    size_t capacity = context->capacity == 0u ? 1u : context->capacity * 2u;
    if (capacity > context->max_count)
      capacity = context->max_count;
    items = h2_pal_mem_realloc(context->allocator, items,
                               capacity * sizeof(*items));
    if (items == NULL)
      return false;
    context->page->items = items;
    context->capacity = capacity;
  }
  h2_gizclaw_friend_group_t *out = &items[count];
  memset(out, 0, sizeof(*out));

  gizclaw_rpc_v1_FriendGroupObject decoded =
      gizclaw_rpc_v1_FriendGroupObject_init_zero;
  h2_gizclaw_social_text_decode_t text[4];
  set_bounded_text_decoder(&decoded.name, &text[0], context->allocator,
                           &out->name, H2_GIZCLAW_FRIEND_GROUP_NAME_MAX_BYTES);
  set_bounded_text_decoder(&decoded.display_name, &text[1], context->allocator,
                           &out->display_name,
                           H2_GIZCLAW_FRIEND_GROUP_DISPLAY_NAME_MAX_BYTES);
  set_bounded_text_decoder(&decoded.description, &text[2], context->allocator,
                           &out->description,
                           H2_GIZCLAW_FRIEND_GROUP_DESCRIPTION_MAX_BYTES);
  set_bounded_text_decoder(&decoded.workspace_name, &text[3],
                           context->allocator, &out->workspace_name,
                           H2_GIZCLAW_FRIEND_GROUP_NAME_MAX_BYTES);
  if (!pb_decode(stream, gizclaw_rpc_v1_FriendGroupObject_fields, &decoded) ||
      out->name == NULL || out->name[0] == '\0' || !valid_utf8(out->name) ||
      !valid_utf8(out->display_name) || !valid_utf8(out->description) ||
      !valid_utf8(out->workspace_name)) {
    friend_group_deinit(context->allocator, out);
    return false;
  }
  if (decoded.has_my_role) {
    out->my_role = (h2_gizclaw_friend_group_role_t)decoded.my_role;
  }
  context->page->count = count + 1u;
  return true;
}

static bool decode_contact_object(pb_istream_t *stream,
                                  h2_gizclaw_contact_t *out,
                                  const h2_pal_mem_api_t *allocator) {
  gizclaw_rpc_v1_ContactObject decoded = gizclaw_rpc_v1_ContactObject_init_zero;
  h2_gizclaw_social_text_decode_t text[5];
  set_bounded_text_decoder(&decoded.name, &text[0], allocator, &out->name,
                           H2_GIZCLAW_CONTACT_NAME_MAX_BYTES);
  set_bounded_text_decoder(&decoded.display_name, &text[1], allocator,
                           &out->display_name,
                           H2_GIZCLAW_CONTACT_DISPLAY_NAME_MAX_BYTES);
  set_bounded_text_decoder(&decoded.phone_number, &text[2], allocator,
                           &out->phone_number,
                           H2_GIZCLAW_CONTACT_PHONE_NUMBER_MAX_BYTES);
  set_bounded_text_decoder(&decoded.created_at, &text[3], allocator,
                           &out->created_at,
                           H2_GIZCLAW_CONTACT_TIMESTAMP_MAX_BYTES);
  set_bounded_text_decoder(&decoded.updated_at, &text[4], allocator,
                           &out->updated_at,
                           H2_GIZCLAW_CONTACT_TIMESTAMP_MAX_BYTES);
  if (!pb_decode(stream, gizclaw_rpc_v1_ContactObject_fields, &decoded) ||
      out->name == NULL || out->name[0] == '\0' || !valid_utf8(out->name) ||
      !valid_utf8(out->display_name) || !valid_utf8(out->phone_number) ||
      !valid_utf8(out->created_at) || !valid_utf8(out->updated_at)) {
    contact_deinit(allocator, out);
    return false;
  }
  return true;
}

static bool decode_contact(pb_istream_t *stream, const pb_field_t *field,
                           void **arg) {
  (void)field;
  h2_gizclaw_contact_page_decode_t *context = *arg;
  if (context == NULL || context->allocator == NULL || context->page == NULL ||
      context->page->count >= context->max_count ||
      context->page->count == SIZE_MAX ||
      context->page->count + 1u > SIZE_MAX / sizeof(context->page->items[0])) {
    return false;
  }
  const size_t count = context->page->count;
  h2_gizclaw_contact_t *items = context->page->items;
  if (count == context->capacity) {
    size_t capacity = context->capacity == 0u ? 1u : context->capacity * 2u;
    if (capacity > context->max_count)
      capacity = context->max_count;
    items = h2_pal_mem_realloc(context->allocator, items,
                               capacity * sizeof(*items));
    if (items == NULL)
      return false;
    context->page->items = items;
    context->capacity = capacity;
  }
  h2_gizclaw_contact_t *out = &items[count];
  memset(out, 0, sizeof(*out));
  if (!decode_contact_object(stream, out, context->allocator))
    return false;
  context->page->count = count + 1u;
  return true;
}

static bool encode_text(pb_ostream_t *stream, const pb_field_t *field,
                        void *const *arg) {
  const h2_gizclaw_social_text_encode_t *text = *arg;
  return text != NULL && (text->len == 0u || text->data != NULL) &&
         pb_encode_tag_for_field(stream, field) &&
         pb_encode_string(stream, (const pb_byte_t *)text->data, text->len);
}

static int decode_contact_mutation_response(const h2_pal_mem_api_t *allocator,
                                            const pb_msgdesc_t *fields,
                                            void *response, bool *has_value,
                                            gizclaw_rpc_v1_ContactObject *value,
                                            h2_gizclaw_contact_t *out_contact,
                                            const uint8_t *payload,
                                            size_t payload_len) {
  if (allocator == NULL || fields == NULL || response == NULL ||
      has_value == NULL || value == NULL || out_contact == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_gizclaw_social_text_decode_t text[5];
  set_bounded_text_decoder(&value->name, &text[0], allocator,
                           &out_contact->name,
                           H2_GIZCLAW_CONTACT_NAME_MAX_BYTES);
  set_bounded_text_decoder(&value->display_name, &text[1], allocator,
                           &out_contact->display_name,
                           H2_GIZCLAW_CONTACT_DISPLAY_NAME_MAX_BYTES);
  set_bounded_text_decoder(&value->phone_number, &text[2], allocator,
                           &out_contact->phone_number,
                           H2_GIZCLAW_CONTACT_PHONE_NUMBER_MAX_BYTES);
  set_bounded_text_decoder(&value->created_at, &text[3], allocator,
                           &out_contact->created_at,
                           H2_GIZCLAW_CONTACT_TIMESTAMP_MAX_BYTES);
  set_bounded_text_decoder(&value->updated_at, &text[4], allocator,
                           &out_contact->updated_at,
                           H2_GIZCLAW_CONTACT_TIMESTAMP_MAX_BYTES);
  pb_istream_t stream = pb_istream_from_buffer(payload, payload_len);
  if (!pb_decode(&stream, fields, response) || !*has_value ||
      out_contact->name == NULL || out_contact->name[0] == '\0' ||
      !valid_utf8(out_contact->name) ||
      !valid_utf8(out_contact->display_name) ||
      !valid_utf8(out_contact->phone_number) ||
      !valid_utf8(out_contact->created_at) ||
      !valid_utf8(out_contact->updated_at)) {
    contact_deinit(allocator, out_contact);
    return H2_PAL_ERR_FORMAT;
  }
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_social_create_message_internal(
    h2_gizclaw_service_t *service, uint64_t identity, const void *tag,
    h2_gizclaw_rpc_method_t method, const pb_msgdesc_t *fields,
    const void *message, uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (service == NULL || out_request == NULL || timeout_ms == 0u ||
      timeout_ms > INT32_MAX)
    return H2_PAL_ERR_INVALID_ARG;
  pb_ostream_t sizing = PB_OSTREAM_SIZING;
  if (!pb_encode(&sizing, fields, message))
    return H2_PAL_ERR_FORMAT;
  const h2_pal_mem_api_t *allocator = service->client_config.allocator;
  uint8_t *data = h2_pal_mem_alloc(
      allocator, sizing.bytes_written == 0u ? 1u : sizing.bytes_written);
  if (data == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  pb_ostream_t output = pb_ostream_from_buffer(data, sizing.bytes_written);
  h2_pal_result_t rc = H2_PAL_ERR_FORMAT;
  if (pb_encode(&output, fields, message))
    rc = h2_gizclaw_req_create_rpc_internal(
        service, identity, method, tag,
        (h2_gizclaw_rpc_bytes_t){data, output.bytes_written}, timeout_ms,
        out_request);
  h2_pal_mem_free(allocator, data);
  return rc;
}

static const char contact_list_tag;
h2_pal_result_t h2_gizclaw_req_create_contact_list(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t cursor,
    size_t limit, uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (limit == 0u || limit > H2_GIZCLAW_CONTACT_PAGE_MAX_ITEMS ||
      cursor.len > H2_GIZCLAW_CONTACT_CURSOR_MAX_BYTES ||
      !valid_utf8_span(cursor.data, cursor.len))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_ContactListRequest message =
      gizclaw_rpc_v1_ContactListRequest_init_zero;
  h2_gizclaw_social_text_encode_t text = {cursor.data, cursor.len};
  if (cursor.len != 0u) {
    message.cursor.funcs.encode = encode_text;
    message.cursor.arg = &text;
  }
  message.has_limit = true;
  message.limit = (int64_t)limit;
  return h2_gizclaw_social_create_message_internal(
      service, identity, &contact_list_tag, H2_GIZCLAW_RPC_SERVER_CONTACT_LIST,
      gizclaw_rpc_v1_ContactListRequest_fields, &message, timeout_ms,
      out_request);
}

h2_pal_result_t
h2_gizclaw_resp_parse_contact_list(const h2_gizclaw_req_t *request,
                                   h2_gizclaw_resp_storage_t *storage,
                                   h2_gizclaw_contact_page_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc =
      h2_gizclaw_req_response_internal(request, &contact_list_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_rpc_bytes_t input;
  rc = h2_gizclaw_req_input_internal(request, &contact_list_tag, &input);
  if (rc != H2_PAL_OK)
    return rc;
  gizclaw_rpc_v1_ContactListRequest params =
      gizclaw_rpc_v1_ContactListRequest_init_zero;
  pb_istream_t input_stream = pb_istream_from_buffer(input.data, input.len);
  if (!pb_decode(&input_stream, gizclaw_rpc_v1_ContactListRequest_fields,
                 &params) ||
      !params.has_limit || params.limit <= 0 ||
      params.limit > H2_GIZCLAW_CONTACT_PAGE_MAX_ITEMS)
    return H2_PAL_ERR_FORMAT;
  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_contact_page_t result = {0};
  gizclaw_rpc_v1_ContactListResponse decoded =
      gizclaw_rpc_v1_ContactListResponse_init_zero;
  h2_gizclaw_contact_page_decode_t items = {.allocator = &arena.allocator,
                                            .page = &result,
                                            .max_count = (size_t)params.limit};
  decoded.items.funcs.decode = decode_contact;
  decoded.items.arg = &items;
  h2_gizclaw_social_text_decode_t cursor;
  set_bounded_text_decoder(&decoded.next_cursor, &cursor, &arena.allocator,
                           &result.next_cursor,
                           H2_GIZCLAW_CONTACT_CURSOR_MAX_BYTES);
  pb_istream_t stream = pb_istream_from_buffer(response->result_payload,
                                               response->result_payload_len);
  if (!pb_decode(&stream, gizclaw_rpc_v1_ContactListResponse_fields,
                 &decoded) ||
      (decoded.has_next &&
       (result.next_cursor == NULL || result.next_cursor[0] == '\0')) ||
      !valid_utf8(result.next_cursor))
    rc = H2_PAL_ERR_FORMAT;
  else
    result.has_next = decoded.has_next;
  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}

h2_pal_result_t h2_gizclaw_rpc_contact_list(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t cursor, size_t limit,
    uint32_t timeout_ms, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_contact_page_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_contact_list(
      service, 0u, cursor, limit, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_contact_list(request, storage, out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

static const char contact_get_tag;
h2_pal_result_t h2_gizclaw_req_create_contact_get(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (name.len == 0u || name.len > H2_GIZCLAW_CONTACT_NAME_MAX_BYTES ||
      !valid_utf8_span(name.data, name.len))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_ContactGetRequest message =
      gizclaw_rpc_v1_ContactGetRequest_init_zero;
  h2_gizclaw_social_text_encode_t text = {name.data, name.len};
  message.name.funcs.encode = encode_text;
  message.name.arg = &text;
  return h2_gizclaw_social_create_message_internal(
      service, identity, &contact_get_tag, H2_GIZCLAW_RPC_SERVER_CONTACT_GET,
      gizclaw_rpc_v1_ContactGetRequest_fields, &message, timeout_ms,
      out_request);
}

h2_pal_result_t
h2_gizclaw_resp_parse_contact_get(const h2_gizclaw_req_t *request,
                                  h2_gizclaw_resp_storage_t *storage,
                                  h2_gizclaw_contact_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc =
      h2_gizclaw_req_response_internal(request, &contact_get_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;

  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_contact_t result = {0};
  gizclaw_rpc_v1_ContactGetResponse decoded =
      gizclaw_rpc_v1_ContactGetResponse_init_zero;
  rc = (h2_pal_result_t)decode_contact_mutation_response(
      &arena.allocator, gizclaw_rpc_v1_ContactGetResponse_fields, &decoded,
      &decoded.has_value, &decoded.value, &result, response->result_payload,
      response->result_payload_len);
  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}

h2_pal_result_t h2_gizclaw_rpc_contact_get(h2_gizclaw_service_t *service,
                                           h2_gizclaw_str_t name,
                                           uint32_t timeout_ms,
                                           h2_gizclaw_resp_storage_t *storage,
                                           h2_gizclaw_contact_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_contact_get(service, 0u, name,
                                                         timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_contact_get(request, storage, out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

static const char contact_create_tag;
h2_pal_result_t h2_gizclaw_req_create_contact_create(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    h2_gizclaw_str_t display_name, h2_gizclaw_str_t phone_number,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (name.len == 0u || name.len > H2_GIZCLAW_CONTACT_NAME_MAX_BYTES ||
      !valid_utf8_span(name.data, name.len) ||
      display_name.len > H2_GIZCLAW_CONTACT_DISPLAY_NAME_MAX_BYTES ||
      phone_number.len > H2_GIZCLAW_CONTACT_PHONE_NUMBER_MAX_BYTES ||
      !valid_utf8_span(display_name.data, display_name.len) ||
      !valid_utf8_span(phone_number.data, phone_number.len))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_ContactCreateRequest message =
      gizclaw_rpc_v1_ContactCreateRequest_init_zero;
  h2_gizclaw_social_text_encode_t text = {name.data, name.len};
  message.name.funcs.encode = encode_text;
  message.name.arg = &text;
  h2_gizclaw_social_text_encode_t display = {display_name.data,
                                             display_name.len};
  h2_gizclaw_social_text_encode_t phone = {phone_number.data, phone_number.len};
  if (display_name.len != 0u) {
    message.display_name.funcs.encode = encode_text;
    message.display_name.arg = &display;
  }
  if (phone_number.len != 0u) {
    message.phone_number.funcs.encode = encode_text;
    message.phone_number.arg = &phone;
  }
  return h2_gizclaw_social_create_message_internal(
      service, identity, &contact_create_tag,
      H2_GIZCLAW_RPC_SERVER_CONTACT_CREATE,
      gizclaw_rpc_v1_ContactCreateRequest_fields, &message, timeout_ms,
      out_request);
}

h2_pal_result_t
h2_gizclaw_resp_parse_contact_create(const h2_gizclaw_req_t *request,
                                     h2_gizclaw_resp_storage_t *storage,
                                     h2_gizclaw_contact_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc =
      h2_gizclaw_req_response_internal(request, &contact_create_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;

  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_contact_t result = {0};
  gizclaw_rpc_v1_ContactCreateResponse decoded =
      gizclaw_rpc_v1_ContactCreateResponse_init_zero;
  rc = (h2_pal_result_t)decode_contact_mutation_response(
      &arena.allocator, gizclaw_rpc_v1_ContactCreateResponse_fields, &decoded,
      &decoded.has_value, &decoded.value, &result, response->result_payload,
      response->result_payload_len);
  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}

h2_pal_result_t h2_gizclaw_rpc_contact_create(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t name,
    h2_gizclaw_str_t display_name, h2_gizclaw_str_t phone_number,
    uint32_t timeout_ms, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_contact_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_contact_create(
      service, 0u, name, display_name, phone_number, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_contact_create(request, storage, out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

static const char contact_put_tag;
h2_pal_result_t h2_gizclaw_req_create_contact_put(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    h2_gizclaw_str_t display_name, h2_gizclaw_str_t phone_number,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (name.len == 0u || name.len > H2_GIZCLAW_CONTACT_NAME_MAX_BYTES ||
      !valid_utf8_span(name.data, name.len) ||
      display_name.len > H2_GIZCLAW_CONTACT_DISPLAY_NAME_MAX_BYTES ||
      phone_number.len > H2_GIZCLAW_CONTACT_PHONE_NUMBER_MAX_BYTES ||
      !valid_utf8_span(display_name.data, display_name.len) ||
      !valid_utf8_span(phone_number.data, phone_number.len))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_ContactPutRequest message =
      gizclaw_rpc_v1_ContactPutRequest_init_zero;
  h2_gizclaw_social_text_encode_t text = {name.data, name.len};
  message.name.funcs.encode = encode_text;
  message.name.arg = &text;
  h2_gizclaw_social_text_encode_t display = {display_name.data,
                                             display_name.len};
  h2_gizclaw_social_text_encode_t phone = {phone_number.data, phone_number.len};
  if (display_name.len != 0u) {
    message.display_name.funcs.encode = encode_text;
    message.display_name.arg = &display;
  }
  if (phone_number.len != 0u) {
    message.phone_number.funcs.encode = encode_text;
    message.phone_number.arg = &phone;
  }
  return h2_gizclaw_social_create_message_internal(
      service, identity, &contact_put_tag, H2_GIZCLAW_RPC_SERVER_CONTACT_PUT,
      gizclaw_rpc_v1_ContactPutRequest_fields, &message, timeout_ms,
      out_request);
}

h2_pal_result_t
h2_gizclaw_resp_parse_contact_put(const h2_gizclaw_req_t *request,
                                  h2_gizclaw_resp_storage_t *storage,
                                  h2_gizclaw_contact_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc =
      h2_gizclaw_req_response_internal(request, &contact_put_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;

  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_contact_t result = {0};
  gizclaw_rpc_v1_ContactPutResponse decoded =
      gizclaw_rpc_v1_ContactPutResponse_init_zero;
  rc = (h2_pal_result_t)decode_contact_mutation_response(
      &arena.allocator, gizclaw_rpc_v1_ContactPutResponse_fields, &decoded,
      &decoded.has_value, &decoded.value, &result, response->result_payload,
      response->result_payload_len);
  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}

h2_pal_result_t h2_gizclaw_rpc_contact_put(h2_gizclaw_service_t *service,
                                           h2_gizclaw_str_t name,
                                           h2_gizclaw_str_t display_name,
                                           h2_gizclaw_str_t phone_number,
                                           uint32_t timeout_ms,
                                           h2_gizclaw_resp_storage_t *storage,
                                           h2_gizclaw_contact_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_contact_put(
      service, 0u, name, display_name, phone_number, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_contact_put(request, storage, out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

static const char contact_delete_tag;
h2_pal_result_t h2_gizclaw_req_create_contact_delete(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (name.len == 0u || name.len > H2_GIZCLAW_CONTACT_NAME_MAX_BYTES ||
      !valid_utf8_span(name.data, name.len))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_ContactDeleteRequest message =
      gizclaw_rpc_v1_ContactDeleteRequest_init_zero;
  h2_gizclaw_social_text_encode_t text = {name.data, name.len};
  message.name.funcs.encode = encode_text;
  message.name.arg = &text;
  return h2_gizclaw_social_create_message_internal(
      service, identity, &contact_delete_tag,
      H2_GIZCLAW_RPC_SERVER_CONTACT_DELETE,
      gizclaw_rpc_v1_ContactDeleteRequest_fields, &message, timeout_ms,
      out_request);
}

h2_pal_result_t
h2_gizclaw_resp_parse_contact_delete(const h2_gizclaw_req_t *request,
                                     h2_gizclaw_resp_storage_t *storage,
                                     h2_gizclaw_contact_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc =
      h2_gizclaw_req_response_internal(request, &contact_delete_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;

  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_contact_t result = {0};
  gizclaw_rpc_v1_ContactDeleteResponse decoded =
      gizclaw_rpc_v1_ContactDeleteResponse_init_zero;
  rc = (h2_pal_result_t)decode_contact_mutation_response(
      &arena.allocator, gizclaw_rpc_v1_ContactDeleteResponse_fields, &decoded,
      &decoded.has_value, &decoded.value, &result, response->result_payload,
      response->result_payload_len);
  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}

h2_pal_result_t h2_gizclaw_rpc_contact_delete(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t name, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_contact_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_contact_delete(
      service, 0u, name, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_contact_delete(request, storage, out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

static const char friend_group_list_tag;
h2_pal_result_t h2_gizclaw_req_create_friend_group_list(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t cursor,
    size_t limit, uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (limit == 0u || limit > H2_GIZCLAW_CONTACT_PAGE_MAX_ITEMS ||
      cursor.len > H2_GIZCLAW_CONTACT_CURSOR_MAX_BYTES ||
      !valid_utf8_span(cursor.data, cursor.len))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_FriendGroupListRequest message =
      gizclaw_rpc_v1_FriendGroupListRequest_init_zero;
  h2_gizclaw_social_text_encode_t text = {cursor.data, cursor.len};
  if (cursor.len != 0u) {
    message.cursor.funcs.encode = encode_text;
    message.cursor.arg = &text;
  }
  message.has_limit = true;
  message.limit = (int64_t)limit;
  return h2_gizclaw_social_create_message_internal(
      service, identity, &friend_group_list_tag,
      H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_LIST,
      gizclaw_rpc_v1_FriendGroupListRequest_fields, &message, timeout_ms,
      out_request);
}

h2_pal_result_t h2_gizclaw_resp_parse_friend_group_list(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_page_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_response_internal(
      request, &friend_group_list_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_rpc_bytes_t input;
  rc = h2_gizclaw_req_input_internal(request, &friend_group_list_tag, &input);
  if (rc != H2_PAL_OK)
    return rc;
  gizclaw_rpc_v1_FriendGroupListRequest params =
      gizclaw_rpc_v1_FriendGroupListRequest_init_zero;
  pb_istream_t input_stream = pb_istream_from_buffer(input.data, input.len);
  if (!pb_decode(&input_stream, gizclaw_rpc_v1_FriendGroupListRequest_fields,
                 &params) ||
      !params.has_limit || params.limit <= 0 ||
      params.limit > H2_GIZCLAW_CONTACT_PAGE_MAX_ITEMS)
    return H2_PAL_ERR_FORMAT;
  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_friend_group_page_t result = {0};
  gizclaw_rpc_v1_FriendGroupListResponse decoded =
      gizclaw_rpc_v1_FriendGroupListResponse_init_zero;
  h2_gizclaw_social_page_decode_t items = {.allocator = &arena.allocator,
                                           .page = &result,
                                           .max_count = (size_t)params.limit};
  decoded.items.funcs.decode = decode_group;
  decoded.items.arg = &items;
  h2_gizclaw_social_text_decode_t cursor;
  set_bounded_text_decoder(&decoded.next_cursor, &cursor, &arena.allocator,
                           &result.next_cursor,
                           H2_GIZCLAW_CONTACT_CURSOR_MAX_BYTES);
  pb_istream_t stream = pb_istream_from_buffer(response->result_payload,
                                               response->result_payload_len);
  if (!pb_decode(&stream, gizclaw_rpc_v1_FriendGroupListResponse_fields,
                 &decoded) ||
      (decoded.has_next &&
       (result.next_cursor == NULL || result.next_cursor[0] == '\0')) ||
      !valid_utf8(result.next_cursor))
    rc = H2_PAL_ERR_FORMAT;
  else
    result.has_next = decoded.has_next;
  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}

h2_pal_result_t h2_gizclaw_rpc_friend_group_list(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t cursor, size_t limit,
    uint32_t timeout_ms, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_page_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_friend_group_list(
      service, 0u, cursor, limit, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_friend_group_list(request, storage, out_result);
  h2_gizclaw_req_release(request);
  return rc;
}
