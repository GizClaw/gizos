#define H2_GIZCLAW_INTERNAL_SYNC_API
#include "h2_gizclaw_social.h"
#undef H2_GIZCLAW_INTERNAL_SYNC_API
#include "h2_gizclaw_internal.h"
#include "h2_gizclaw_rpc.h"

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
} h2_gizclaw_social_page_decode_t;

typedef struct h2_gizclaw_social_text_encode {
  const char *data;
  size_t len;
} h2_gizclaw_social_text_encode_t;

typedef struct h2_gizclaw_contact_page_decode {
  const h2_pal_mem_api_t *allocator;
  h2_gizclaw_contact_page_t *page;
  size_t max_count;
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
  h2_gizclaw_friend_group_t *items =
      h2_pal_mem_realloc(context->allocator, context->page->items,
                         (count + 1u) * sizeof(context->page->items[0]));
  if (items == NULL)
    return false;
  context->page->items = items;
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
  set_text_decoder(&decoded.workspace_name, &text[3], context->allocator,
                   &out->workspace_name);
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
  h2_gizclaw_contact_t *items =
      h2_pal_mem_realloc(context->allocator, context->page->items,
                         (count + 1u) * sizeof(context->page->items[0]));
  if (items == NULL)
    return false;
  context->page->items = items;
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

static int encode_request(const h2_pal_mem_api_t *allocator,
                          const gizclaw_rpc_v1_FriendGroupListRequest *request,
                          uint8_t **out_data, size_t *out_len) {
  *out_data = NULL;
  *out_len = 0u;
  pb_ostream_t sizing = PB_OSTREAM_SIZING;
  if (!pb_encode(&sizing, gizclaw_rpc_v1_FriendGroupListRequest_fields,
                 request)) {
    return H2_PAL_ERR_FORMAT;
  }
  uint8_t *data = h2_pal_mem_alloc(
      allocator, sizing.bytes_written == 0u ? 1u : sizing.bytes_written);
  if (data == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  pb_ostream_t stream = pb_ostream_from_buffer(data, sizing.bytes_written);
  if (!pb_encode(&stream, gizclaw_rpc_v1_FriendGroupListRequest_fields,
                 request)) {
    h2_pal_mem_free(allocator, data);
    return H2_PAL_ERR_FORMAT;
  }
  *out_data = data;
  *out_len = stream.bytes_written;
  return H2_PAL_OK;
}

static int response_status(const h2_gizclaw_rpc_response_t *response) {
  if (response == NULL || !response->has_error)
    return response == NULL ? H2_PAL_ERR_INVALID_ARG : H2_PAL_OK;
  if (response->error_code == H2_GIZCLAW_RPC_ERROR_NOT_FOUND)
    return H2_PAL_ERR_NOT_FOUND;
  return response->error_code == H2_GIZCLAW_RPC_ERROR_METHOD_NOT_FOUND
             ? H2_PAL_ERR_UNSUPPORTED
             : H2_PAL_ERR_IO;
}

static int
encode_contact_list_request(const h2_pal_mem_api_t *allocator,
                            const gizclaw_rpc_v1_ContactListRequest *request,
                            uint8_t **out_data, size_t *out_len) {
  *out_data = NULL;
  *out_len = 0u;
  pb_ostream_t sizing = PB_OSTREAM_SIZING;
  if (!pb_encode(&sizing, gizclaw_rpc_v1_ContactListRequest_fields, request))
    return H2_PAL_ERR_FORMAT;
  uint8_t *data = h2_pal_mem_alloc(
      allocator, sizing.bytes_written == 0u ? 1u : sizing.bytes_written);
  if (data == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  pb_ostream_t stream = pb_ostream_from_buffer(data, sizing.bytes_written);
  if (!pb_encode(&stream, gizclaw_rpc_v1_ContactListRequest_fields, request)) {
    h2_pal_mem_free(allocator, data);
    return H2_PAL_ERR_FORMAT;
  }
  *out_data = data;
  *out_len = stream.bytes_written;
  return H2_PAL_OK;
}

int h2_gizclaw_client_contacts_list(h2_gizclaw_client_t *client,
                                    h2_gizclaw_str_t cursor, size_t limit,
                                    h2_gizclaw_contact_page_t *out_page) {
  if (client == NULL || out_page == NULL || limit == 0u ||
      limit > H2_GIZCLAW_CONTACT_PAGE_MAX_ITEMS ||
#if SIZE_MAX > INT64_MAX
      limit > (size_t)INT64_MAX ||
#endif
      (cursor.len != 0u && cursor.data == NULL) ||
      cursor.len > H2_GIZCLAW_CONTACT_CURSOR_MAX_BYTES ||
      !valid_utf8_span(cursor.data, cursor.len)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_page, 0, sizeof(*out_page));
  const h2_pal_mem_api_t *allocator =
      h2_gizclaw_client_allocator_internal(client);
  if (allocator == NULL)
    return H2_PAL_ERR_INVALID_STATE;

  gizclaw_rpc_v1_ContactListRequest request =
      gizclaw_rpc_v1_ContactListRequest_init_zero;
  h2_gizclaw_social_text_encode_t cursor_text = {
      .data = cursor.data,
      .len = cursor.len,
  };
  if (cursor.len != 0u) {
    request.cursor.funcs.encode = encode_text;
    request.cursor.arg = &cursor_text;
  }
  request.has_limit = true;
  request.limit = (int64_t)limit;

  uint8_t *payload = NULL;
  size_t payload_len = 0u;
  int rc =
      encode_contact_list_request(allocator, &request, &payload, &payload_len);
  h2_gizclaw_rpc_response_t response = {0};
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_client_rpc_call(
        client, H2_GIZCLAW_RPC_SERVER_CONTACT_LIST,
        (h2_gizclaw_rpc_bytes_t){.data = payload, .len = payload_len},
        &response);
  }
  h2_pal_mem_free(allocator, payload);
  if (rc == H2_PAL_OK)
    rc = response_status(&response);
  if (rc == H2_PAL_OK) {
    gizclaw_rpc_v1_ContactListResponse decoded =
        gizclaw_rpc_v1_ContactListResponse_init_zero;
    h2_gizclaw_contact_page_decode_t items = {
        .allocator = allocator,
        .page = out_page,
        .max_count = limit,
    };
    h2_gizclaw_social_text_decode_t next_cursor;
    decoded.items.funcs.decode = decode_contact;
    decoded.items.arg = &items;
    set_bounded_text_decoder(&decoded.next_cursor, &next_cursor, allocator,
                             &out_page->next_cursor,
                             H2_GIZCLAW_CONTACT_CURSOR_MAX_BYTES);
    pb_istream_t stream = pb_istream_from_buffer(response.result_payload,
                                                 response.result_payload_len);
    if (!pb_decode(&stream, gizclaw_rpc_v1_ContactListResponse_fields,
                   &decoded) ||
        (decoded.has_next &&
         (out_page->next_cursor == NULL || out_page->next_cursor[0] == '\0' ||
          !valid_utf8(out_page->next_cursor)))) {
      rc = H2_PAL_ERR_FORMAT;
    } else {
      out_page->has_next = decoded.has_next;
    }
  }
  h2_gizclaw_rpc_response_deinit(client, &response);
  if (rc != H2_PAL_OK)
    h2_gizclaw_contact_page_deinit(client, out_page);
  return rc;
}

int h2_gizclaw_client_contact_create(h2_gizclaw_client_t *client,
                                     h2_gizclaw_str_t name,
                                     h2_gizclaw_str_t display_name,
                                     h2_gizclaw_str_t phone_number,
                                     h2_gizclaw_contact_t *out_contact) {
  if (client == NULL || out_contact == NULL || name.data == NULL ||
      name.len == 0u || name.len > H2_GIZCLAW_CONTACT_NAME_MAX_BYTES ||
      display_name.len > H2_GIZCLAW_CONTACT_DISPLAY_NAME_MAX_BYTES ||
      phone_number.len > H2_GIZCLAW_CONTACT_PHONE_NUMBER_MAX_BYTES ||
      !valid_utf8_span(name.data, name.len) ||
      !valid_utf8_span(display_name.data, display_name.len) ||
      !valid_utf8_span(phone_number.data, phone_number.len)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_contact, 0, sizeof(*out_contact));
  const h2_pal_mem_api_t *allocator =
      h2_gizclaw_client_allocator_internal(client);
  if (allocator == NULL)
    return H2_PAL_ERR_INVALID_STATE;
  gizclaw_rpc_v1_ContactCreateRequest request =
      gizclaw_rpc_v1_ContactCreateRequest_init_zero;
  h2_gizclaw_social_text_encode_t text[3] = {
      {.data = name.data, .len = name.len},
      {.data = display_name.data, .len = display_name.len},
      {.data = phone_number.data, .len = phone_number.len},
  };
  request.name.funcs.encode = encode_text;
  request.name.arg = &text[0];
  if (display_name.len != 0u) {
    request.display_name.funcs.encode = encode_text;
    request.display_name.arg = &text[1];
  }
  if (phone_number.len != 0u) {
    request.phone_number.funcs.encode = encode_text;
    request.phone_number.arg = &text[2];
  }
  pb_ostream_t sizing = PB_OSTREAM_SIZING;
  if (!pb_encode(&sizing, gizclaw_rpc_v1_ContactCreateRequest_fields,
                 &request)) {
    return H2_PAL_ERR_FORMAT;
  }
  uint8_t *payload = h2_pal_mem_alloc(
      allocator, sizing.bytes_written == 0u ? 1u : sizing.bytes_written);
  if (payload == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  pb_ostream_t output = pb_ostream_from_buffer(payload, sizing.bytes_written);
  int rc = H2_PAL_OK;
  if (!pb_encode(&output, gizclaw_rpc_v1_ContactCreateRequest_fields,
                 &request)) {
    rc = H2_PAL_ERR_FORMAT;
  }
  h2_gizclaw_rpc_response_t response = {0};
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_client_rpc_call(
        client, H2_GIZCLAW_RPC_SERVER_CONTACT_CREATE,
        (h2_gizclaw_rpc_bytes_t){.data = payload, .len = output.bytes_written},
        &response);
  }
  h2_pal_mem_free(allocator, payload);
  if (rc == H2_PAL_OK)
    rc = response_status(&response);
  if (rc == H2_PAL_OK) {
    gizclaw_rpc_v1_ContactCreateResponse decoded =
        gizclaw_rpc_v1_ContactCreateResponse_init_zero;
    h2_gizclaw_social_text_decode_t decoded_text[5];
    set_bounded_text_decoder(&decoded.value.name, &decoded_text[0], allocator,
                             &out_contact->name,
                             H2_GIZCLAW_CONTACT_NAME_MAX_BYTES);
    set_bounded_text_decoder(&decoded.value.display_name, &decoded_text[1],
                             allocator, &out_contact->display_name,
                             H2_GIZCLAW_CONTACT_DISPLAY_NAME_MAX_BYTES);
    set_bounded_text_decoder(&decoded.value.phone_number, &decoded_text[2],
                             allocator, &out_contact->phone_number,
                             H2_GIZCLAW_CONTACT_PHONE_NUMBER_MAX_BYTES);
    set_bounded_text_decoder(&decoded.value.created_at, &decoded_text[3],
                             allocator, &out_contact->created_at,
                             H2_GIZCLAW_CONTACT_TIMESTAMP_MAX_BYTES);
    set_bounded_text_decoder(&decoded.value.updated_at, &decoded_text[4],
                             allocator, &out_contact->updated_at,
                             H2_GIZCLAW_CONTACT_TIMESTAMP_MAX_BYTES);
    pb_istream_t stream = pb_istream_from_buffer(response.result_payload,
                                                 response.result_payload_len);
    if (!pb_decode(&stream, gizclaw_rpc_v1_ContactCreateResponse_fields,
                   &decoded) ||
        !decoded.has_value || out_contact->name == NULL ||
        out_contact->name[0] == '\0' || !valid_utf8(out_contact->name) ||
        !valid_utf8(out_contact->display_name) ||
        !valid_utf8(out_contact->phone_number) ||
        !valid_utf8(out_contact->created_at) ||
        !valid_utf8(out_contact->updated_at)) {
      rc = H2_PAL_ERR_FORMAT;
    }
  }
  h2_gizclaw_rpc_response_deinit(client, &response);
  if (rc != H2_PAL_OK)
    contact_deinit(allocator, out_contact);
  return rc;
}

static int decode_contact_mutation_response(h2_gizclaw_client_t *client,
                                            const pb_msgdesc_t *fields,
                                            void *response, bool *has_value,
                                            gizclaw_rpc_v1_ContactObject *value,
                                            h2_gizclaw_contact_t *out_contact,
                                            const uint8_t *payload,
                                            size_t payload_len) {
  const h2_pal_mem_api_t *allocator =
      h2_gizclaw_client_allocator_internal(client);
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

int h2_gizclaw_client_contact_get(h2_gizclaw_client_t *client,
                                  h2_gizclaw_str_t name,
                                  h2_gizclaw_contact_t *out_contact) {
  if (client == NULL || out_contact == NULL || name.data == NULL ||
      name.len == 0u || name.len > H2_GIZCLAW_CONTACT_NAME_MAX_BYTES ||
      !valid_utf8_span(name.data, name.len)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_contact, 0, sizeof(*out_contact));
  const h2_pal_mem_api_t *allocator =
      h2_gizclaw_client_allocator_internal(client);
  if (allocator == NULL)
    return H2_PAL_ERR_INVALID_STATE;

  gizclaw_rpc_v1_ContactGetRequest request =
      gizclaw_rpc_v1_ContactGetRequest_init_zero;
  h2_gizclaw_social_text_encode_t name_text = {.data = name.data,
                                               .len = name.len};
  request.name.funcs.encode = encode_text;
  request.name.arg = &name_text;
  pb_ostream_t sizing = PB_OSTREAM_SIZING;
  if (!pb_encode(&sizing, gizclaw_rpc_v1_ContactGetRequest_fields, &request))
    return H2_PAL_ERR_FORMAT;
  uint8_t *encoded = h2_pal_mem_alloc(
      allocator, sizing.bytes_written == 0u ? 1u : sizing.bytes_written);
  if (encoded == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  pb_ostream_t output = pb_ostream_from_buffer(encoded, sizing.bytes_written);
  int rc = pb_encode(&output, gizclaw_rpc_v1_ContactGetRequest_fields, &request)
               ? H2_PAL_OK
               : H2_PAL_ERR_FORMAT;
  h2_gizclaw_rpc_response_t response = {0};
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_client_rpc_call(
        client, H2_GIZCLAW_RPC_SERVER_CONTACT_GET,
        (h2_gizclaw_rpc_bytes_t){.data = encoded, .len = output.bytes_written},
        &response);
  }
  h2_pal_mem_free(allocator, encoded);
  if (rc == H2_PAL_OK)
    rc = response_status(&response);
  if (rc == H2_PAL_OK) {
    gizclaw_rpc_v1_ContactGetResponse decoded =
        gizclaw_rpc_v1_ContactGetResponse_init_zero;
    rc = decode_contact_mutation_response(
        client, gizclaw_rpc_v1_ContactGetResponse_fields, &decoded,
        &decoded.has_value, &decoded.value, out_contact,
        response.result_payload, response.result_payload_len);
  }
  h2_gizclaw_rpc_response_deinit(client, &response);
  if (rc != H2_PAL_OK)
    contact_deinit(allocator, out_contact);
  return rc;
}

int h2_gizclaw_client_contact_put(h2_gizclaw_client_t *client,
                                  h2_gizclaw_str_t name,
                                  h2_gizclaw_str_t display_name,
                                  h2_gizclaw_str_t phone_number,
                                  h2_gizclaw_contact_t *out_contact) {
  if (client == NULL || out_contact == NULL || name.data == NULL ||
      name.len == 0u || name.len > H2_GIZCLAW_CONTACT_NAME_MAX_BYTES ||
      display_name.len > H2_GIZCLAW_CONTACT_DISPLAY_NAME_MAX_BYTES ||
      phone_number.len > H2_GIZCLAW_CONTACT_PHONE_NUMBER_MAX_BYTES ||
      !valid_utf8_span(name.data, name.len) ||
      !valid_utf8_span(display_name.data, display_name.len) ||
      !valid_utf8_span(phone_number.data, phone_number.len)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_contact, 0, sizeof(*out_contact));
  const h2_pal_mem_api_t *allocator =
      h2_gizclaw_client_allocator_internal(client);
  if (allocator == NULL)
    return H2_PAL_ERR_INVALID_STATE;
  gizclaw_rpc_v1_ContactPutRequest request =
      gizclaw_rpc_v1_ContactPutRequest_init_zero;
  h2_gizclaw_social_text_encode_t name_text = {.data = name.data,
                                               .len = name.len};
  h2_gizclaw_social_text_encode_t display = {.data = display_name.data,
                                             .len = display_name.len};
  h2_gizclaw_social_text_encode_t phone = {.data = phone_number.data,
                                           .len = phone_number.len};
  request.name.funcs.encode = encode_text;
  request.name.arg = &name_text;
  if (display_name.len != 0u) {
    request.display_name.funcs.encode = encode_text;
    request.display_name.arg = &display;
  }
  if (phone_number.len != 0u) {
    request.phone_number.funcs.encode = encode_text;
    request.phone_number.arg = &phone;
  }
  pb_ostream_t sizing = PB_OSTREAM_SIZING;
  if (!pb_encode(&sizing, gizclaw_rpc_v1_ContactPutRequest_fields, &request))
    return H2_PAL_ERR_FORMAT;
  uint8_t *encoded = h2_pal_mem_alloc(
      allocator, sizing.bytes_written == 0u ? 1u : sizing.bytes_written);
  if (encoded == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  pb_ostream_t output = pb_ostream_from_buffer(encoded, sizing.bytes_written);
  int rc = pb_encode(&output, gizclaw_rpc_v1_ContactPutRequest_fields, &request)
               ? H2_PAL_OK
               : H2_PAL_ERR_FORMAT;
  h2_gizclaw_rpc_response_t response = {0};
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_client_rpc_call(
        client, H2_GIZCLAW_RPC_SERVER_CONTACT_PUT,
        (h2_gizclaw_rpc_bytes_t){.data = encoded, .len = output.bytes_written},
        &response);
  }
  h2_pal_mem_free(allocator, encoded);
  if (rc == H2_PAL_OK)
    rc = response_status(&response);
  if (rc == H2_PAL_OK) {
    gizclaw_rpc_v1_ContactPutResponse decoded =
        gizclaw_rpc_v1_ContactPutResponse_init_zero;
    rc = decode_contact_mutation_response(
        client, gizclaw_rpc_v1_ContactPutResponse_fields, &decoded,
        &decoded.has_value, &decoded.value, out_contact,
        response.result_payload, response.result_payload_len);
  }
  h2_gizclaw_rpc_response_deinit(client, &response);
  if (rc != H2_PAL_OK)
    contact_deinit(allocator, out_contact);
  return rc;
}

int h2_gizclaw_client_contact_delete(h2_gizclaw_client_t *client,
                                     h2_gizclaw_str_t name,
                                     h2_gizclaw_contact_t *out_contact) {
  if (client == NULL || out_contact == NULL || name.data == NULL ||
      name.len == 0u || name.len > H2_GIZCLAW_CONTACT_NAME_MAX_BYTES ||
      !valid_utf8_span(name.data, name.len)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_contact, 0, sizeof(*out_contact));
  const h2_pal_mem_api_t *allocator =
      h2_gizclaw_client_allocator_internal(client);
  if (allocator == NULL)
    return H2_PAL_ERR_INVALID_STATE;
  gizclaw_rpc_v1_ContactDeleteRequest request =
      gizclaw_rpc_v1_ContactDeleteRequest_init_zero;
  h2_gizclaw_social_text_encode_t name_text = {.data = name.data,
                                               .len = name.len};
  request.name.funcs.encode = encode_text;
  request.name.arg = &name_text;
  pb_ostream_t sizing = PB_OSTREAM_SIZING;
  if (!pb_encode(&sizing, gizclaw_rpc_v1_ContactDeleteRequest_fields,
                 &request)) {
    return H2_PAL_ERR_FORMAT;
  }
  uint8_t *encoded = h2_pal_mem_alloc(
      allocator, sizing.bytes_written == 0u ? 1u : sizing.bytes_written);
  if (encoded == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  pb_ostream_t output = pb_ostream_from_buffer(encoded, sizing.bytes_written);
  int rc =
      pb_encode(&output, gizclaw_rpc_v1_ContactDeleteRequest_fields, &request)
          ? H2_PAL_OK
          : H2_PAL_ERR_FORMAT;
  h2_gizclaw_rpc_response_t response = {0};
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_client_rpc_call(
        client, H2_GIZCLAW_RPC_SERVER_CONTACT_DELETE,
        (h2_gizclaw_rpc_bytes_t){.data = encoded, .len = output.bytes_written},
        &response);
  }
  h2_pal_mem_free(allocator, encoded);
  if (rc == H2_PAL_OK)
    rc = response_status(&response);
  if (rc == H2_PAL_OK) {
    gizclaw_rpc_v1_ContactDeleteResponse decoded =
        gizclaw_rpc_v1_ContactDeleteResponse_init_zero;
    rc = decode_contact_mutation_response(
        client, gizclaw_rpc_v1_ContactDeleteResponse_fields, &decoded,
        &decoded.has_value, &decoded.value, out_contact,
        response.result_payload, response.result_payload_len);
  }
  h2_gizclaw_rpc_response_deinit(client, &response);
  if (rc != H2_PAL_OK)
    contact_deinit(allocator, out_contact);
  return rc;
}

void h2_gizclaw_contact_deinit(h2_gizclaw_client_t *client,
                               h2_gizclaw_contact_t *contact) {
  if (client == NULL || contact == NULL)
    return;
  const h2_pal_mem_api_t *allocator =
      h2_gizclaw_client_allocator_internal(client);
  if (allocator != NULL)
    contact_deinit(allocator, contact);
}

void h2_gizclaw_contact_page_deinit(h2_gizclaw_client_t *client,
                                    h2_gizclaw_contact_page_t *page) {
  if (client == NULL || page == NULL)
    return;
  const h2_pal_mem_api_t *allocator =
      h2_gizclaw_client_allocator_internal(client);
  if (allocator != NULL) {
    for (size_t index = 0u; index < page->count; ++index)
      contact_deinit(allocator, &page->items[index]);
    h2_pal_mem_free(allocator, page->items);
    h2_pal_mem_free(allocator, page->next_cursor);
  }
  memset(page, 0, sizeof(*page));
}

int h2_gizclaw_client_friend_groups_list(
    h2_gizclaw_client_t *client, h2_gizclaw_str_t cursor, size_t limit,
    h2_gizclaw_friend_group_page_t *out_page) {
  if (client == NULL || out_page == NULL || limit == 0u ||
#if SIZE_MAX > INT64_MAX
      limit > (size_t)INT64_MAX ||
#endif
      (cursor.len != 0u && cursor.data == NULL)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_page, 0, sizeof(*out_page));
  const h2_pal_mem_api_t *allocator =
      h2_gizclaw_client_allocator_internal(client);
  if (allocator == NULL)
    return H2_PAL_ERR_INVALID_STATE;

  gizclaw_rpc_v1_FriendGroupListRequest request =
      gizclaw_rpc_v1_FriendGroupListRequest_init_zero;
  h2_gizclaw_social_text_encode_t cursor_text = {
      .data = cursor.data,
      .len = cursor.len,
  };
  if (cursor.len != 0u) {
    request.cursor.funcs.encode = encode_text;
    request.cursor.arg = &cursor_text;
  }
  request.has_limit = true;
  request.limit = (int64_t)limit;

  uint8_t *payload = NULL;
  size_t payload_len = 0u;
  int rc = encode_request(allocator, &request, &payload, &payload_len);
  h2_gizclaw_rpc_response_t response = {0};
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_client_rpc_call(
        client, H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_LIST,
        (h2_gizclaw_rpc_bytes_t){.data = payload, .len = payload_len},
        &response);
  }
  h2_pal_mem_free(allocator, payload);
  if (rc == H2_PAL_OK)
    rc = response_status(&response);
  if (rc == H2_PAL_OK) {
    gizclaw_rpc_v1_FriendGroupListResponse decoded =
        gizclaw_rpc_v1_FriendGroupListResponse_init_zero;
    h2_gizclaw_social_page_decode_t items = {
        .allocator = allocator,
        .page = out_page,
        .max_count = limit,
    };
    h2_gizclaw_social_text_decode_t next_cursor;
    decoded.items.funcs.decode = decode_group;
    decoded.items.arg = &items;
    set_text_decoder(&decoded.next_cursor, &next_cursor, allocator,
                     &out_page->next_cursor);
    pb_istream_t stream = pb_istream_from_buffer(response.result_payload,
                                                 response.result_payload_len);
    if (!pb_decode(&stream, gizclaw_rpc_v1_FriendGroupListResponse_fields,
                   &decoded)) {
      rc = H2_PAL_ERR_FORMAT;
    } else {
      out_page->has_next = decoded.has_next;
    }
  }
  h2_gizclaw_rpc_response_deinit(client, &response);
  if (rc != H2_PAL_OK)
    h2_gizclaw_friend_group_page_deinit(client, out_page);
  return rc;
}

void h2_gizclaw_friend_group_page_deinit(h2_gizclaw_client_t *client,
                                         h2_gizclaw_friend_group_page_t *page) {
  if (client == NULL || page == NULL)
    return;
  const h2_pal_mem_api_t *allocator =
      h2_gizclaw_client_allocator_internal(client);
  if (allocator != NULL) {
    for (size_t index = 0u; index < page->count; ++index)
      friend_group_deinit(allocator, &page->items[index]);
    h2_pal_mem_free(allocator, page->items);
    h2_pal_mem_free(allocator, page->next_cursor);
  }
  memset(page, 0, sizeof(*page));
}
