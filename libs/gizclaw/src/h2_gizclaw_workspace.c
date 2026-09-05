#include "h2_gizclaw_workspace.h"

#include "h2_gizclaw_internal.h"
#include "h2_gizclaw_response_internal.h"
#include "h2_gizclaw_rpc.h"
#include "h2_gizclaw_service_internal.h"

#include "payload/ai.pb.h"
#include "payload/workspace.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

typedef struct text_decode {
  const h2_pal_mem_api_t *allocator;
  char **out;
  size_t max_len;
} text_decode_t;

typedef struct text_encode {
  const char *data;
  size_t len;
} text_encode_t;

typedef struct workspace_page_decode {
  const h2_pal_mem_api_t *allocator;
  h2_gizclaw_workspace_page_t *page;
  size_t max_count;
  size_t capacity;
} workspace_page_decode_t;

typedef struct history_page_decode {
  const h2_pal_mem_api_t *allocator;
  h2_gizclaw_workspace_history_page_t *page;
  size_t max_count;
  size_t capacity;
} history_page_decode_t;

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

static bool valid_token(h2_gizclaw_str_t value, size_t max_len) {
  if (value.data == NULL || value.len == 0u || value.len > max_len ||
      memchr(value.data, '\0', value.len) != NULL ||
      !valid_utf8_span(value.data, value.len)) {
    return false;
  }
  return true;
}

static bool valid_kebab(h2_gizclaw_str_t value, size_t max_len) {
  if (!valid_token(value, max_len) || value.data[0] == '-' ||
      value.data[value.len - 1u] == '-') {
    return false;
  }
  for (size_t index = 0u; index < value.len; ++index) {
    const char ch = value.data[index];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
          (ch == '-' && index > 0u && value.data[index - 1u] != '-'))) {
      return false;
    }
  }
  return true;
}

static bool valid_optional_text(h2_gizclaw_str_t value, size_t max_len) {
  return value.len <= max_len &&
         (value.len == 0u ||
          (value.data != NULL && memchr(value.data, '\0', value.len) == NULL &&
           valid_utf8_span(value.data, value.len)));
}

static bool decode_text(pb_istream_t *stream, const pb_field_t *field,
                        void **arg) {
  (void)field;
  text_decode_t *context = *arg;
  if (context == NULL || context->allocator == NULL || context->out == NULL ||
      *context->out != NULL || stream->bytes_left > context->max_len) {
    return false;
  }
  char *text = h2_pal_mem_alloc(context->allocator, stream->bytes_left + 1u);
  if (text == NULL)
    return false;
  const size_t len = stream->bytes_left;
  if (!pb_read(stream, (pb_byte_t *)text, len) ||
      memchr(text, '\0', len) != NULL || !valid_utf8_span(text, len)) {
    h2_pal_mem_free(context->allocator, text);
    return false;
  }
  text[len] = '\0';
  *context->out = text;
  return true;
}

static void set_text_decoder(pb_callback_t *callback, text_decode_t *context,
                             const h2_pal_mem_api_t *allocator, char **out,
                             size_t max_len) {
  *context = (text_decode_t){
      .allocator = allocator,
      .out = out,
      .max_len = max_len,
  };
  callback->funcs.decode = decode_text;
  callback->arg = context;
}

static bool encode_text(pb_ostream_t *stream, const pb_field_t *field,
                        void *const *arg) {
  const text_encode_t *text = *arg;
  return text != NULL && text->data != NULL &&
         pb_encode_tag_for_field(stream, field) &&
         pb_encode_string(stream, (const pb_byte_t *)text->data, text->len);
}

static int encode_message(const h2_pal_mem_api_t *allocator,
                          const pb_msgdesc_t *fields, const void *message,
                          uint8_t **out, size_t *out_len) {
  *out = NULL;
  *out_len = 0u;
  pb_ostream_t sizing = PB_OSTREAM_SIZING;
  if (!pb_encode(&sizing, fields, message))
    return H2_PAL_ERR_FORMAT;
  uint8_t *data = h2_pal_mem_alloc(
      allocator, sizing.bytes_written == 0u ? 1u : sizing.bytes_written);
  if (data == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  pb_ostream_t stream = pb_ostream_from_buffer(data, sizing.bytes_written);
  if (!pb_encode(&stream, fields, message)) {
    h2_pal_mem_free(allocator, data);
    return H2_PAL_ERR_FORMAT;
  }
  *out = data;
  *out_len = stream.bytes_written;
  return H2_PAL_OK;
}

static char *copy_owned(const h2_pal_mem_api_t *allocator, const char *data,
                        size_t len) {
  char *copy = h2_pal_mem_alloc(allocator, len + 1u);
  if (copy == NULL)
    return NULL;
  memcpy(copy, data, len);
  copy[len] = '\0';
  return copy;
}

static int ensure_owned_empty(const h2_pal_mem_api_t *allocator, char **text) {
  if (*text != NULL)
    return H2_PAL_OK;
  *text = copy_owned(allocator, "", 0u);
  return *text == NULL ? H2_PAL_ERR_NO_MEMORY : H2_PAL_OK;
}

static void workspace_clear(const h2_pal_mem_api_t *allocator,
                            h2_gizclaw_workspace_t *workspace) {
  if (workspace == NULL)
    return;
  h2_pal_mem_free(allocator, workspace->name);
  h2_pal_mem_free(allocator, workspace->collection);
  h2_pal_mem_free(allocator, workspace->workflow_name);
  memset(workspace, 0, sizeof(*workspace));
}

static void history_entry_clear(const h2_pal_mem_api_t *allocator,
                                h2_gizclaw_workspace_history_entry_t *entry) {
  if (entry == NULL)
    return;
  h2_pal_mem_free(allocator, entry->created_at);
  h2_pal_mem_free(allocator, entry->gear_id);
  h2_pal_mem_free(allocator, entry->id);
  h2_pal_mem_free(allocator, entry->name);
  h2_pal_mem_free(allocator, entry->text);
  memset(entry, 0, sizeof(*entry));
}

static bool
decode_history_entry_object(pb_istream_t *stream,
                            h2_gizclaw_workspace_history_entry_t *out,
                            const h2_pal_mem_api_t *allocator) {
  gizclaw_rpc_v1_PeerRunHistoryEntry decoded =
      gizclaw_rpc_v1_PeerRunHistoryEntry_init_zero;
  text_decode_t text[5];
  set_text_decoder(&decoded.created_at, &text[0], allocator, &out->created_at,
                   63u);
  set_text_decoder(&decoded.gear_id, &text[1], allocator, &out->gear_id,
                   H2_GIZCLAW_WORKSPACE_HISTORY_ID_MAX_BYTES);
  set_text_decoder(&decoded.name, &text[2], allocator, &out->id,
                   H2_GIZCLAW_WORKSPACE_HISTORY_ID_MAX_BYTES);
  set_text_decoder(&decoded.actor_name, &text[3], allocator, &out->name, 255u);
  set_text_decoder(&decoded.text, &text[4], allocator, &out->text,
                   H2_GIZCLAW_WORKSPACE_HISTORY_TEXT_MAX_BYTES);
  if (!pb_decode(stream, gizclaw_rpc_v1_PeerRunHistoryEntry_fields, &decoded) ||
      out->id == NULL || out->id[0] == '\0' ||
      ensure_owned_empty(allocator, &out->created_at) != H2_PAL_OK ||
      ensure_owned_empty(allocator, &out->name) != H2_PAL_OK ||
      ensure_owned_empty(allocator, &out->text) != H2_PAL_OK) {
    history_entry_clear(allocator, out);
    return false;
  }
  if ((int)decoded.type != H2_GIZCLAW_WORKSPACE_HISTORY_GEAR &&
      (int)decoded.type != H2_GIZCLAW_WORKSPACE_HISTORY_AGENT) {
    history_entry_clear(allocator, out);
    return false;
  }
  out->type = (h2_gizclaw_workspace_history_type_t)decoded.type;
  out->replay_available = decoded.replay_available;
  return true;
}

static bool decode_history_entry(pb_istream_t *stream, const pb_field_t *field,
                                 void **arg) {
  (void)field;
  history_page_decode_t *context = *arg;
  if (context == NULL || context->allocator == NULL || context->page == NULL ||
      context->page->count >= context->max_count) {
    return false;
  }
  const size_t count = context->page->count;
  h2_gizclaw_workspace_history_entry_t *items = context->page->items;
  if (count >= context->capacity) {
    size_t capacity = context->capacity == 0u ? 1u : context->capacity * 2u;
    if (capacity <= count)
      capacity = count + 1u;
    if (capacity > context->max_count)
      capacity = context->max_count;
    items = h2_pal_mem_realloc(context->allocator, items,
                               capacity * sizeof(*items));
    if (items == NULL)
      return false;
    context->page->items = items;
    context->capacity = capacity;
  }
  memset(&items[count], 0, sizeof(items[count]));
  if (!decode_history_entry_object(stream, &items[count], context->allocator)) {
    return false;
  }
  context->page->count = count + 1u;
  return true;
}

static bool decode_workspace_object(pb_istream_t *stream,
                                    h2_gizclaw_workspace_t *out,
                                    const h2_pal_mem_api_t *allocator) {
  gizclaw_rpc_v1_Workspace decoded = gizclaw_rpc_v1_Workspace_init_zero;
  text_decode_t text[2];
  set_text_decoder(&decoded.name, &text[0], allocator, &out->name,
                   H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES);
  set_text_decoder(&decoded.workflow_name, &text[1], allocator,
                   &out->workflow_name, 63u);
  if (!pb_decode(stream, gizclaw_rpc_v1_Workspace_fields, &decoded) ||
      out->name == NULL || out->name[0] == '\0' || out->workflow_name == NULL ||
      out->workflow_name[0] == '\0') {
    workspace_clear(allocator, out);
    return false;
  }
  out->system = decoded.system;
  out->available = decoded.available;
  return true;
}

static bool decode_workspace(pb_istream_t *stream, const pb_field_t *field,
                             void **arg) {
  (void)field;
  workspace_page_decode_t *context = *arg;
  if (context == NULL || context->allocator == NULL || context->page == NULL ||
      context->page->count >= context->max_count) {
    return false;
  }
  const size_t count = context->page->count;
  h2_gizclaw_workspace_t *items = context->page->items;
  if (count >= context->capacity) {
    size_t capacity = context->capacity == 0u ? 1u : context->capacity * 2u;
    if (capacity <= count)
      capacity = count + 1u;
    if (capacity > context->max_count)
      capacity = context->max_count;
    items = h2_pal_mem_realloc(context->allocator, items,
                               capacity * sizeof(*items));
    if (items == NULL)
      return false;
    context->page->items = items;
    context->capacity = capacity;
  }
  memset(&items[count], 0, sizeof(items[count]));
  if (!decode_workspace_object(stream, &items[count], context->allocator))
    return false;
  context->page->count = count + 1u;
  return true;
}

static int decode_workspace_list(const h2_pal_mem_api_t *allocator,
                                 const uint8_t *data, size_t len,
                                 size_t max_count,
                                 h2_gizclaw_workspace_page_t *out_page) {
  gizclaw_rpc_v1_WorkspaceListResponse decoded =
      gizclaw_rpc_v1_WorkspaceListResponse_init_zero;
  workspace_page_decode_t items = {
      .allocator = allocator,
      .page = out_page,
      .max_count = max_count,
  };
  text_decode_t text[3];
  decoded.items.funcs.decode = decode_workspace;
  decoded.items.arg = &items;
  set_text_decoder(&decoded.next_cursor, &text[0], allocator,
                   &out_page->next_cursor, 255u);
  set_text_decoder(&decoded.runtime_profile_name, &text[1], allocator,
                   &out_page->runtime_profile_name, 255u);
  set_text_decoder(&decoded.runtime_profile_revision, &text[2], allocator,
                   &out_page->runtime_profile_revision, 255u);
  pb_istream_t stream = pb_istream_from_buffer(data, len);
  if (!pb_decode(&stream, gizclaw_rpc_v1_WorkspaceListResponse_fields,
                 &decoded) ||
      out_page->runtime_profile_name == NULL ||
      out_page->runtime_profile_revision == NULL) {
    return H2_PAL_ERR_FORMAT;
  }
  out_page->has_next = decoded.has_next;
  return H2_PAL_OK;
}

static int decode_history_list(const h2_pal_mem_api_t *allocator,
                               const uint8_t *data, size_t len,
                               size_t max_count,
                               h2_gizclaw_workspace_history_page_t *out_page) {
  gizclaw_rpc_v1_WorkspaceHistoryListResponse decoded =
      gizclaw_rpc_v1_WorkspaceHistoryListResponse_init_zero;
  history_page_decode_t items = {
      .allocator = allocator,
      .page = out_page,
      .max_count = max_count,
  };
  text_decode_t text[2];
  decoded.value.items.funcs.decode = decode_history_entry;
  decoded.value.items.arg = &items;
  set_text_decoder(&decoded.value.message, &text[0], allocator,
                   &out_page->message, 1023u);
  set_text_decoder(&decoded.value.next_cursor, &text[1], allocator,
                   &out_page->next_cursor, 255u);
  pb_istream_t stream = pb_istream_from_buffer(data, len);
  if (!pb_decode(&stream, gizclaw_rpc_v1_WorkspaceHistoryListResponse_fields,
                 &decoded) ||
      !decoded.has_value) {
    return H2_PAL_ERR_FORMAT;
  }
  out_page->available = decoded.value.available;
  out_page->has_next = decoded.value.has_next;
  if (out_page->has_next &&
      (out_page->next_cursor == NULL || out_page->next_cursor[0] == '\0')) {
    return H2_PAL_ERR_FORMAT;
  }
  return H2_PAL_OK;
}

static int decode_workspace_get(const h2_pal_mem_api_t *allocator,
                                const uint8_t *data, size_t len,
                                h2_gizclaw_workspace_t *out_workspace,
                                char **out_profile_name,
                                char **out_profile_revision) {
  gizclaw_rpc_v1_WorkspaceGetResponse decoded =
      gizclaw_rpc_v1_WorkspaceGetResponse_init_zero;
  text_decode_t text[4];
  set_text_decoder(&decoded.value.name, &text[0], allocator,
                   &out_workspace->name, H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES);
  set_text_decoder(&decoded.value.workflow_name, &text[1], allocator,
                   &out_workspace->workflow_name, 63u);
  set_text_decoder(&decoded.runtime_profile_name, &text[2], allocator,
                   out_profile_name, 255u);
  set_text_decoder(&decoded.runtime_profile_revision, &text[3], allocator,
                   out_profile_revision, 255u);
  pb_istream_t stream = pb_istream_from_buffer(data, len);
  if (!pb_decode(&stream, gizclaw_rpc_v1_WorkspaceGetResponse_fields,
                 &decoded) ||
      !decoded.has_value || out_workspace->name == NULL ||
      out_workspace->workflow_name == NULL || *out_profile_name == NULL ||
      *out_profile_revision == NULL) {
    return H2_PAL_ERR_FORMAT;
  }
  out_workspace->system = decoded.value.system;
  out_workspace->available = decoded.value.available;
  return H2_PAL_OK;
}

static int
decode_workspace_create_value(const h2_pal_mem_api_t *allocator,
                              const uint8_t *data, size_t len,
                              h2_gizclaw_workspace_t *out_workspace) {
  gizclaw_rpc_v1_WorkspaceCreateResponse decoded =
      gizclaw_rpc_v1_WorkspaceCreateResponse_init_zero;
  text_decode_t text[2];
  set_text_decoder(&decoded.value.name, &text[0], allocator,
                   &out_workspace->name, H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES);
  set_text_decoder(&decoded.value.workflow_name, &text[1], allocator,
                   &out_workspace->workflow_name, 63u);
  pb_istream_t stream = pb_istream_from_buffer(data, len);
  if (!pb_decode(&stream, gizclaw_rpc_v1_WorkspaceCreateResponse_fields,
                 &decoded) ||
      !decoded.has_value || out_workspace->name == NULL ||
      out_workspace->workflow_name == NULL) {
    return H2_PAL_ERR_FORMAT;
  }
  out_workspace->system = decoded.value.system;
  out_workspace->available = decoded.value.available;
  return H2_PAL_OK;
}

static int
decode_workspace_input_put_value(const h2_pal_mem_api_t *allocator,
                                 const uint8_t *data, size_t len,
                                 h2_gizclaw_workspace_t *out_workspace) {
  gizclaw_rpc_v1_WorkspaceInputPutResponse decoded =
      gizclaw_rpc_v1_WorkspaceInputPutResponse_init_zero;
  text_decode_t text[2];
  set_text_decoder(&decoded.value.name, &text[0], allocator,
                   &out_workspace->name, H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES);
  set_text_decoder(&decoded.value.workflow_name, &text[1], allocator,
                   &out_workspace->workflow_name, 63u);
  pb_istream_t stream = pb_istream_from_buffer(data, len);
  if (!pb_decode(&stream, gizclaw_rpc_v1_WorkspaceInputPutResponse_fields,
                 &decoded) ||
      !decoded.has_value || out_workspace->name == NULL ||
      out_workspace->workflow_name == NULL) {
    return H2_PAL_ERR_FORMAT;
  }
  out_workspace->system = decoded.value.system;
  out_workspace->available = decoded.value.available;
  return H2_PAL_OK;
}

static int
decode_workspace_delete_value(const h2_pal_mem_api_t *allocator,
                              const uint8_t *data, size_t len,
                              h2_gizclaw_workspace_t *out_workspace) {
  gizclaw_rpc_v1_WorkspaceDeleteResponse decoded =
      gizclaw_rpc_v1_WorkspaceDeleteResponse_init_zero;
  text_decode_t text[2];
  set_text_decoder(&decoded.value.name, &text[0], allocator,
                   &out_workspace->name, H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES);
  set_text_decoder(&decoded.value.workflow_name, &text[1], allocator,
                   &out_workspace->workflow_name, 63u);
  pb_istream_t stream = pb_istream_from_buffer(data, len);
  if (!pb_decode(&stream, gizclaw_rpc_v1_WorkspaceDeleteResponse_fields,
                 &decoded) ||
      !decoded.has_value || out_workspace->name == NULL ||
      out_workspace->workflow_name == NULL) {
    return H2_PAL_ERR_FORMAT;
  }
  out_workspace->system = decoded.value.system;
  out_workspace->available = decoded.value.available;
  return H2_PAL_OK;
}

static bool
workspace_input_mode_valid(h2_gizclaw_workspace_input_mode_t input_mode) {
  return input_mode == H2_GIZCLAW_WORKSPACE_INPUT_PUSH_TO_TALK ||
         input_mode == H2_GIZCLAW_WORKSPACE_INPUT_REALTIME;
}

static bool protobuf_read_varint(const uint8_t *data, size_t len,
                                 size_t *offset, uint64_t *out_value) {
  uint64_t value = 0u;
  for (unsigned int shift = 0u; shift < 64u && *offset < len; shift += 7u) {
    const uint8_t byte = data[(*offset)++];
    if (shift == 63u && (byte & 0xfeu) != 0u)
      return false;
    value |= (uint64_t)(byte & 0x7fu) << shift;
    if ((byte & 0x80u) == 0u) {
      *out_value = value;
      return true;
    }
  }
  return false;
}

static bool protobuf_skip(const uint8_t *data, size_t len, size_t *offset,
                          uint8_t wire_type) {
  uint64_t value = 0u;
  switch (wire_type) {
  case 0u:
    return protobuf_read_varint(data, len, offset, &value);
  case 1u:
    if (len - *offset < 8u)
      return false;
    *offset += 8u;
    return true;
  case 2u:
    if (!protobuf_read_varint(data, len, offset, &value) || value > SIZE_MAX ||
        (size_t)value > len - *offset)
      return false;
    *offset += (size_t)value;
    return true;
  case 5u:
    if (len - *offset < 4u)
      return false;
    *offset += 4u;
    return true;
  default:
    return false;
  }
}

static bool protobuf_find_bytes(const uint8_t *data, size_t len,
                                uint32_t wanted_field, const uint8_t **out_data,
                                size_t *out_len, bool *out_found) {
  size_t offset = 0u;
  *out_data = NULL;
  *out_len = 0u;
  *out_found = false;
  while (offset < len) {
    uint64_t key = 0u;
    if (!protobuf_read_varint(data, len, &offset, &key) || key == 0u ||
        key >> 3u > UINT32_MAX)
      return false;
    const uint32_t field = (uint32_t)(key >> 3u);
    const uint8_t wire_type = (uint8_t)(key & 0x07u);
    if (field == wanted_field) {
      uint64_t value_len = 0u;
      if (wire_type != 2u ||
          !protobuf_read_varint(data, len, &offset, &value_len) ||
          value_len > SIZE_MAX || (size_t)value_len > len - offset)
        return false;
      *out_data = data + offset;
      *out_len = (size_t)value_len;
      *out_found = true;
      offset += (size_t)value_len;
    } else if (!protobuf_skip(data, len, &offset, wire_type)) {
      return false;
    }
  }
  return true;
}


typedef enum workspace_kind {
  WS_LIST,
  WS_GET,
  WS_CREATE,
  WS_SET_INPUT,
  WS_DELETE,
  WS_HISTORY_LIST,
  WS_KIND_COUNT,
} workspace_kind_t;

static const char workspace_tags[WS_KIND_COUNT];
static const char workspace_activate_tag;
static const char workspace_reload_tag;

typedef struct workspace_context {
  const h2_pal_mem_api_t *allocator;
  workspace_kind_t kind;
  char *first;
  char *second;
  char *third;
  size_t limit;
  h2_gizclaw_workspace_input_mode_t input_mode;
  h2_gizclaw_workspace_history_order_t history_order;
  h2_gizclaw_rpc_method_t method;
  uint8_t *payload;
  size_t payload_len;
} workspace_context_t;

static h2_gizclaw_str_t workspace_request_span(const char *value) {
  return (h2_gizclaw_str_t){.data = value, .len = strlen(value)};
}

static h2_pal_result_t
decode_activation(const h2_pal_mem_api_t *allocator, const uint8_t *data,
                  size_t len,
                  h2_gizclaw_workspace_activation_t *out_activation) {
  gizclaw_rpc_v1_ServerSetRunWorkspaceResponse decoded =
      gizclaw_rpc_v1_ServerSetRunWorkspaceResponse_init_zero;
  text_decode_t text[4];
  set_text_decoder(&decoded.value.workspace_name, &text[0], allocator,
                   &out_activation->workspace_name,
                   H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES);
  set_text_decoder(&decoded.value.active_workspace_name, &text[1], allocator,
                   &out_activation->active_workspace_name,
                   H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES);
  set_text_decoder(&decoded.value.pending_workspace_name, &text[2], allocator,
                   &out_activation->pending_workspace_name,
                   H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES);
  set_text_decoder(&decoded.value.workflow_name, &text[3], allocator,
                   &out_activation->workflow_name, 63u);
  pb_istream_t stream = pb_istream_from_buffer(data, len);
  if (!pb_decode(&stream, gizclaw_rpc_v1_ServerSetRunWorkspaceResponse_fields,
                 &decoded) ||
      !decoded.has_value || out_activation->workspace_name == NULL)
    return H2_PAL_ERR_FORMAT;
  out_activation->runtime_state =
      (h2_gizclaw_workspace_runtime_state_t)decoded.value.runtime_state;
  return H2_PAL_OK;
}

static h2_pal_result_t
workspace_request_start_payload(workspace_context_t *context,
                                h2_gizclaw_rpc_method_t method,
                                const uint8_t *payload, size_t len) {
  uint8_t *copy = NULL;
  if (len != 0u) {
    copy = h2_pal_mem_alloc(context->allocator, len);
    if (copy == NULL)
      return H2_PAL_ERR_NO_MEMORY;
    memcpy(copy, payload, len);
  }
  h2_pal_mem_free(context->allocator, context->payload);
  context->payload = copy;
  context->payload_len = len;
  context->method = method;
  return H2_PAL_OK;
}

static h2_pal_result_t workspace_request_start_message(
    workspace_context_t *context, h2_gizclaw_rpc_method_t method,
    const pb_msgdesc_t *fields, const void *message) {
  uint8_t *payload = NULL;
  size_t len = 0u;
  h2_pal_result_t rc = (h2_pal_result_t)encode_message(
      context->allocator, fields, message, &payload, &len);
  if (rc == H2_PAL_OK)
    rc = workspace_request_start_payload(context, method, payload, len);
  h2_pal_mem_free(context->allocator, payload);
  return rc;
}
static h2_pal_result_t workspace_request_start(workspace_context_t *request) {
  const h2_gizclaw_str_t first = workspace_request_span(request->first);
  const h2_gizclaw_str_t second = workspace_request_span(request->second);
  const h2_gizclaw_str_t third = workspace_request_span(request->third);
  switch (request->kind) {
  case WS_LIST: {
    gizclaw_rpc_v1_WorkspaceListRequest message =
        gizclaw_rpc_v1_WorkspaceListRequest_init_zero;
    text_encode_t text[2] = {{.data = first.data, .len = first.len},
                             {.data = second.data, .len = second.len}};
    message.collection.funcs.encode = encode_text;
    message.collection.arg = &text[0];
    if (second.len > 0u) {
      message.cursor.funcs.encode = encode_text;
      message.cursor.arg = &text[1];
    }
    message.has_limit = true;
    message.limit = (int64_t)request->limit;
    return workspace_request_start_message(
        request, H2_GIZCLAW_RPC_SERVER_WORKSPACE_LIST,
        gizclaw_rpc_v1_WorkspaceListRequest_fields, &message);
  }
  case WS_GET: {
    gizclaw_rpc_v1_WorkspaceGetRequest message =
        gizclaw_rpc_v1_WorkspaceGetRequest_init_zero;
    text_encode_t text = {.data = first.data, .len = first.len};
    message.name.funcs.encode = encode_text;
    message.name.arg = &text;
    return workspace_request_start_message(
        request, H2_GIZCLAW_RPC_SERVER_WORKSPACE_GET,
        gizclaw_rpc_v1_WorkspaceGetRequest_fields, &message);
  }
  case WS_SET_INPUT: {
    gizclaw_rpc_v1_WorkspaceInputPutRequest message =
        gizclaw_rpc_v1_WorkspaceInputPutRequest_init_zero;
    if (first.len >= sizeof(message.name))
      return H2_PAL_ERR_INVALID_ARG;
    memcpy(message.name, first.data, first.len);
    message.name[first.len] = '\0';
    message.input =
        request->input_mode == H2_GIZCLAW_WORKSPACE_INPUT_REALTIME
            ? gizclaw_rpc_v1_WorkspaceInputMode_WORKSPACE_INPUT_MODE_REALTIME
            : gizclaw_rpc_v1_WorkspaceInputMode_WORKSPACE_INPUT_MODE_PUSH_TO_TALK;
    return workspace_request_start_message(
        request, H2_GIZCLAW_RPC_SERVER_WORKSPACE_INPUT_PUT,
        gizclaw_rpc_v1_WorkspaceInputPutRequest_fields, &message);
  }
  case WS_CREATE: {
    gizclaw_rpc_v1_WorkspaceCreateRequest message =
        gizclaw_rpc_v1_WorkspaceCreateRequest_init_zero;
    text_encode_t text[3] = {{.data = third.data, .len = third.len},
                             {.data = second.data, .len = second.len},
                             {.data = first.data, .len = first.len}};
    message.has_value = true;
    message.value.name.funcs.encode = encode_text;
    message.value.name.arg = &text[0];
    message.value.workflow_name.funcs.encode = encode_text;
    message.value.workflow_name.arg = &text[1];
    message.value.collection.funcs.encode = encode_text;
    message.value.collection.arg = &text[2];
    return workspace_request_start_message(
        request, H2_GIZCLAW_RPC_SERVER_WORKSPACE_CREATE,
        gizclaw_rpc_v1_WorkspaceCreateRequest_fields, &message);
  }
  case WS_DELETE: {
    gizclaw_rpc_v1_WorkspaceDeleteRequest message =
        gizclaw_rpc_v1_WorkspaceDeleteRequest_init_zero;
    text_encode_t text = {.data = first.data, .len = first.len};
    message.name.funcs.encode = encode_text;
    message.name.arg = &text;
    return workspace_request_start_message(
        request, H2_GIZCLAW_RPC_SERVER_WORKSPACE_DELETE,
        gizclaw_rpc_v1_WorkspaceDeleteRequest_fields, &message);
  }
  case WS_HISTORY_LIST: {
    gizclaw_rpc_v1_WorkspaceHistoryListRequest message =
        gizclaw_rpc_v1_WorkspaceHistoryListRequest_init_zero;
    text_encode_t text[2] = {{.data = first.data, .len = first.len},
                             {.data = second.data, .len = second.len}};
    message.workspace_name.funcs.encode = encode_text;
    message.workspace_name.arg = &text[0];
    if (second.len > 0u) {
      message.cursor.funcs.encode = encode_text;
      message.cursor.arg = &text[1];
    }
    message.has_limit = true;
    message.limit = (int64_t)request->limit;
    message.has_order = true;
    message.order =
        (gizclaw_rpc_v1_WorkspaceHistoryListRequestOrder)request->history_order;
    return workspace_request_start_message(
        request, H2_GIZCLAW_RPC_SERVER_WORKSPACE_HISTORY_LIST,
        gizclaw_rpc_v1_WorkspaceHistoryListRequest_fields, &message);
  }
  case WS_KIND_COUNT:
    break;
  }
  return H2_PAL_ERR_INVALID_STATE;
}

static void workspace_destroy(void *user) {
  workspace_context_t *context = user;
  h2_pal_mem_free(context->allocator, context->payload);
  h2_pal_mem_free(context->allocator, context->first);
  h2_pal_mem_free(context->allocator, context->second);
  h2_pal_mem_free(context->allocator, context->third);
  h2_pal_mem_free(context->allocator, context);
}

static h2_pal_result_t workspace_create_request(
    h2_gizclaw_service_t *service, uint64_t identity, workspace_kind_t kind,
    h2_gizclaw_str_t first, h2_gizclaw_str_t second, h2_gizclaw_str_t third,
    size_t limit, h2_gizclaw_workspace_input_mode_t input_mode,
    h2_gizclaw_workspace_history_order_t order, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (service == NULL || out_request == NULL || timeout_ms == 0u ||
      timeout_ms > INT32_MAX)
    return H2_PAL_ERR_INVALID_ARG;
  const h2_pal_mem_api_t *allocator = service->client_config.allocator;
  workspace_context_t *context = h2_pal_mem_alloc(allocator, sizeof(*context));
  if (context == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  *context = (workspace_context_t){.allocator = allocator,
                                   .kind = kind,
                                   .limit = limit,
                                   .input_mode = input_mode,
                                   .history_order = order};
  context->first =
      copy_owned(allocator, first.len == 0u ? "" : first.data, first.len);
  context->second =
      copy_owned(allocator, second.len == 0u ? "" : second.data, second.len);
  context->third =
      copy_owned(allocator, third.len == 0u ? "" : third.data, third.len);
  h2_pal_result_t rc = H2_PAL_ERR_NO_MEMORY;
  if (context->first != NULL && context->second != NULL &&
      context->third != NULL)
    rc = workspace_request_start(
        context); /* Encode only; no transport at create. */
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_req_create_rpc_context_internal(
        service, identity, context->method, &workspace_tags[kind],
        (h2_gizclaw_rpc_bytes_t){context->payload, context->payload_len},
        timeout_ms, workspace_destroy, context, out_request);
    if (rc == H2_PAL_OK) {
      /* The common request owns its wire payload. Keep only parser metadata
       * here, not a second copy of the encoded request and input strings. */
      h2_pal_mem_free(allocator, context->payload);
      context->payload = NULL;
      context->payload_len = 0u;
      h2_pal_mem_free(allocator, context->second);
      context->second = NULL;
      h2_pal_mem_free(allocator, context->third);
      context->third = NULL;
    }
  }
  if (rc != H2_PAL_OK)
    workspace_destroy(context);
  return rc;
}

static h2_pal_result_t
workspace_request_response(const h2_gizclaw_req_t *request,
                           workspace_kind_t kind,
                           const workspace_context_t **out_context,
                           const h2_gizclaw_rpc_response_t **out_response) {
  const void *raw = NULL;
  h2_pal_result_t rc =
      h2_gizclaw_req_context_internal(request, &workspace_tags[kind], &raw);
  if (rc != H2_PAL_OK)
    return rc;
  *out_context = raw;
  return h2_gizclaw_req_response_internal(request, &workspace_tags[kind],
                                          out_response);
}

h2_pal_result_t h2_gizclaw_req_create_workspace_list(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t collection, h2_gizclaw_str_t cursor, size_t limit,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (!(valid_kebab(collection, 63u) && valid_optional_text(cursor, 255u) &&
        limit > 0u && limit <= H2_GIZCLAW_WORKSPACE_PAGE_MAX_ITEMS))
    return H2_PAL_ERR_INVALID_ARG;
  return workspace_create_request(service, identity, WS_LIST, collection,
                                  cursor, (h2_gizclaw_str_t){0}, limit, 0, 0,
                                  timeout_ms, out_request);
}

h2_pal_result_t
h2_gizclaw_resp_parse_workspace_list(const h2_gizclaw_req_t *request,
                                     h2_gizclaw_resp_storage_t *storage,
                                     h2_gizclaw_workspace_page_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const workspace_context_t *context = NULL;
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc =
      workspace_request_response(request, WS_LIST, &context, &response);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  const h2_pal_mem_api_t *allocator = &arena.allocator;
  const uint8_t *data = response->result_payload;
  const size_t len = response->result_payload_len;
  h2_gizclaw_workspace_page_t result = {0};
  rc = (h2_pal_result_t)decode_workspace_list(allocator, data, len,
                                              context->limit, &result);
  if (rc == H2_PAL_OK) {
    for (size_t i = 0; i < result.count; ++i) {
      result.items[i].collection =
          copy_owned(allocator, context->first, strlen(context->first));
      if (result.items[i].collection == NULL) {
        rc = H2_PAL_ERR_NO_MEMORY;
        break;
      }
    }
  }
  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}

h2_pal_result_t h2_gizclaw_rpc_workspace_list(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t collection,
    h2_gizclaw_str_t cursor, size_t limit, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_workspace_page_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_workspace_list(
      service, 0u, collection, cursor, limit, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_workspace_list(request, storage, out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

h2_pal_result_t h2_gizclaw_req_create_workspace_get(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (!(valid_token(name, H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES)))
    return H2_PAL_ERR_INVALID_ARG;
  return workspace_create_request(service, identity, WS_GET, name,
                                  (h2_gizclaw_str_t){0}, (h2_gizclaw_str_t){0},
                                  0u, 0, 0, timeout_ms, out_request);
}

h2_pal_result_t h2_gizclaw_resp_parse_workspace_get(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_workspace_get_result_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const workspace_context_t *context = NULL;
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc =
      workspace_request_response(request, WS_GET, &context, &response);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  const h2_pal_mem_api_t *allocator = &arena.allocator;
  const uint8_t *data = response->result_payload;
  const size_t len = response->result_payload_len;
  h2_gizclaw_workspace_get_result_t result = {0};
  rc = (h2_pal_result_t)decode_workspace_get(
      allocator, data, len, &result.workspace, &result.runtime_profile_name,
      &result.runtime_profile_revision);

  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}

h2_pal_result_t
h2_gizclaw_rpc_workspace_get(h2_gizclaw_service_t *service,
                             h2_gizclaw_str_t name, uint32_t timeout_ms,
                             h2_gizclaw_resp_storage_t *storage,
                             h2_gizclaw_workspace_get_result_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_workspace_get(
      service, 0u, name, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_workspace_get(request, storage, out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

h2_pal_result_t h2_gizclaw_req_create_workspace_create(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t collection, h2_gizclaw_str_t workflow_name,
    h2_gizclaw_str_t name, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (!(valid_kebab(collection, 63u) &&
        h2_gizclaw_runtime_alias_valid_internal(workflow_name) &&
        valid_token(name, H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES)))
    return H2_PAL_ERR_INVALID_ARG;
  return workspace_create_request(service, identity, WS_CREATE, collection,
                                  workflow_name, name, 0u, 0, 0, timeout_ms,
                                  out_request);
}

h2_pal_result_t
h2_gizclaw_resp_parse_workspace_create(const h2_gizclaw_req_t *request,
                                       h2_gizclaw_resp_storage_t *storage,
                                       h2_gizclaw_workspace_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const workspace_context_t *context = NULL;
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc =
      workspace_request_response(request, WS_CREATE, &context, &response);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  const h2_pal_mem_api_t *allocator = &arena.allocator;
  const uint8_t *data = response->result_payload;
  const size_t len = response->result_payload_len;
  h2_gizclaw_workspace_t result = {0};
  rc = (h2_pal_result_t)decode_workspace_create_value(allocator, data, len,
                                                      &result);
  if (rc == H2_PAL_OK) {
    result.collection =
        copy_owned(allocator, context->first, strlen(context->first));
    if (result.collection == NULL)
      rc = H2_PAL_ERR_NO_MEMORY;
  }
  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}

h2_pal_result_t h2_gizclaw_rpc_workspace_create(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t collection,
    h2_gizclaw_str_t workflow_name, h2_gizclaw_str_t name, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_workspace_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_workspace_create(
      service, 0u, collection, workflow_name, name, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_workspace_create(request, storage, out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

h2_pal_result_t h2_gizclaw_req_create_workspace_set_input(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    h2_gizclaw_workspace_input_mode_t input_mode, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (!(valid_token(name, H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES) &&
        workspace_input_mode_valid(input_mode)))
    return H2_PAL_ERR_INVALID_ARG;
  return workspace_create_request(service, identity, WS_SET_INPUT, name,
                                  (h2_gizclaw_str_t){0}, (h2_gizclaw_str_t){0},
                                  0u, input_mode, 0, timeout_ms, out_request);
}

h2_pal_result_t
h2_gizclaw_resp_parse_workspace_set_input(const h2_gizclaw_req_t *request,
                                          h2_gizclaw_resp_storage_t *storage,
                                          h2_gizclaw_workspace_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const workspace_context_t *context = NULL;
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc =
      workspace_request_response(request, WS_SET_INPUT, &context, &response);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  const h2_pal_mem_api_t *allocator = &arena.allocator;
  const uint8_t *data = response->result_payload;
  const size_t len = response->result_payload_len;
  h2_gizclaw_workspace_t result = {0};
  rc = (h2_pal_result_t)decode_workspace_input_put_value(
      allocator, data, len, &result);

  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}

h2_pal_result_t h2_gizclaw_rpc_workspace_set_input(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t name,
    h2_gizclaw_workspace_input_mode_t input_mode, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_workspace_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_workspace_set_input(
      service, 0u, name, input_mode, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc =
        h2_gizclaw_resp_parse_workspace_set_input(request, storage, out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

h2_pal_result_t h2_gizclaw_req_create_workspace_delete(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (!(valid_token(name, H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES)))
    return H2_PAL_ERR_INVALID_ARG;
  return workspace_create_request(service, identity, WS_DELETE, name,
                                  (h2_gizclaw_str_t){0}, (h2_gizclaw_str_t){0},
                                  0u, 0, 0, timeout_ms, out_request);
}

h2_pal_result_t
h2_gizclaw_resp_parse_workspace_delete(const h2_gizclaw_req_t *request,
                                       h2_gizclaw_resp_storage_t *storage,
                                       h2_gizclaw_workspace_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const workspace_context_t *context = NULL;
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc =
      workspace_request_response(request, WS_DELETE, &context, &response);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  const h2_pal_mem_api_t *allocator = &arena.allocator;
  const uint8_t *data = response->result_payload;
  const size_t len = response->result_payload_len;
  h2_gizclaw_workspace_t result = {0};
  rc = (h2_pal_result_t)decode_workspace_delete_value(allocator, data, len,
                                                      &result);

  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}

h2_pal_result_t h2_gizclaw_rpc_workspace_delete(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t name, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_workspace_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_workspace_delete(
      service, 0u, name, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_workspace_delete(request, storage, out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

h2_pal_result_t h2_gizclaw_req_create_workspace_activate(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (service == NULL || out_request == NULL || timeout_ms == 0u ||
      timeout_ms > INT32_MAX ||
      !valid_token(name, H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_ServerSetRunWorkspaceRequest message =
      gizclaw_rpc_v1_ServerSetRunWorkspaceRequest_init_zero;
  text_encode_t text = {name.data, name.len};
  message.has_value = true;
  message.value.workspace_name.funcs.encode = encode_text;
  message.value.workspace_name.arg = &text;
  uint8_t *payload = NULL;
  size_t len = 0u;
  h2_pal_result_t rc = (h2_pal_result_t)encode_message(
      service->client_config.allocator,
      gizclaw_rpc_v1_ServerSetRunWorkspaceRequest_fields, &message, &payload,
      &len);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_create_rpc_internal(
        service, identity, H2_GIZCLAW_RPC_SERVER_RUN_WORKSPACE_SET,
        &workspace_activate_tag, (h2_gizclaw_rpc_bytes_t){payload, len},
        timeout_ms, out_request);
  h2_pal_mem_free(service->client_config.allocator, payload);
  return rc;
}

h2_pal_result_t
h2_gizclaw_req_create_workspace_reload(h2_gizclaw_service_t *service,
                                       uint64_t identity, uint32_t timeout_ms,
                                       h2_gizclaw_req_t **out_request) {
  return h2_gizclaw_req_create_rpc_internal(
      service, identity, H2_GIZCLAW_RPC_SERVER_RUN_WORKSPACE_RELOAD,
      &workspace_reload_tag, (h2_gizclaw_rpc_bytes_t){0}, timeout_ms,
      out_request);
}

static h2_pal_result_t
parse_workspace_activation(const h2_gizclaw_req_t *request, const void *tag,
                           h2_gizclaw_resp_storage_t *storage,
                           h2_gizclaw_workspace_activation_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc =
      h2_gizclaw_req_response_internal(request, tag, &response);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_workspace_activation_t result = {0};
  /* SET and RELOAD both return value: PeerRunWorkspaceState, field 1. */
  rc = (h2_pal_result_t)decode_activation(
      &arena.allocator, response->result_payload, response->result_payload_len,
      &result);
  if (rc == H2_PAL_OK && tag == &workspace_activate_tag) {
    h2_gizclaw_rpc_bytes_t input = {0};
    const uint8_t *selection = NULL, *name = NULL;
    size_t selection_len = 0u, name_len = 0u;
    bool present = false;
    rc = h2_gizclaw_req_input_internal(request, tag, &input);
    if (rc == H2_PAL_OK &&
        (!protobuf_find_bytes(input.data, input.len, 1u, &selection,
                              &selection_len, &present) ||
         !present ||
         !protobuf_find_bytes(selection, selection_len, 1u, &name, &name_len,
                              &present) ||
         !present || strlen(result.workspace_name) != name_len ||
         memcmp(result.workspace_name, name, name_len) != 0))
      rc = H2_PAL_ERR_INVALID_STATE;
  }
  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}

h2_pal_result_t h2_gizclaw_resp_parse_workspace_activate(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_workspace_activation_t *out_result) {
  return parse_workspace_activation(request, &workspace_activate_tag, storage,
                                    out_result);
}

h2_pal_result_t h2_gizclaw_resp_parse_workspace_reload(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_workspace_activation_t *out_result) {
  return parse_workspace_activation(request, &workspace_reload_tag, storage,
                                    out_result);
}

h2_pal_result_t h2_gizclaw_rpc_workspace_activate(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t name, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_workspace_activation_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_workspace_activate(
      service, 0u, name, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_workspace_activate(request, storage, out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

h2_pal_result_t
h2_gizclaw_rpc_workspace_reload(h2_gizclaw_service_t *service,
                                uint32_t timeout_ms,
                                h2_gizclaw_resp_storage_t *storage,
                                h2_gizclaw_workspace_activation_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc =
      h2_gizclaw_req_create_workspace_reload(service, 0u, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_workspace_reload(request, storage, out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

h2_pal_result_t h2_gizclaw_req_create_workspace_history_list(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t workspace_name, h2_gizclaw_str_t cursor, size_t limit,
    h2_gizclaw_workspace_history_order_t order, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (!(valid_token(workspace_name, H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES) &&
        valid_optional_text(cursor, 255u) && limit > 0u &&
        limit <= H2_GIZCLAW_WORKSPACE_HISTORY_PAGE_MAX_ITEMS &&
        (order == H2_GIZCLAW_WORKSPACE_HISTORY_ORDER_ASC ||
         order == H2_GIZCLAW_WORKSPACE_HISTORY_ORDER_DESC)))
    return H2_PAL_ERR_INVALID_ARG;
  return workspace_create_request(service, identity, WS_HISTORY_LIST,
                                  workspace_name, cursor, (h2_gizclaw_str_t){0},
                                  limit, 0, order, timeout_ms, out_request);
}

h2_pal_result_t h2_gizclaw_resp_parse_workspace_history_list(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_workspace_history_page_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const workspace_context_t *context = NULL;
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc =
      workspace_request_response(request, WS_HISTORY_LIST, &context, &response);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  const h2_pal_mem_api_t *allocator = &arena.allocator;
  const uint8_t *data = response->result_payload;
  const size_t len = response->result_payload_len;
  h2_gizclaw_workspace_history_page_t result = {0};
  rc = (h2_pal_result_t)decode_history_list(allocator, data, len,
                                            context->limit, &result);

  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}

h2_pal_result_t h2_gizclaw_rpc_workspace_history_list(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t workspace_name,
    h2_gizclaw_str_t cursor, size_t limit,
    h2_gizclaw_workspace_history_order_t order, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_workspace_history_page_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_workspace_history_list(
      service, 0u, workspace_name, cursor, limit, order, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_workspace_history_list(request, storage,
                                                      out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

h2_pal_result_t h2_gizclaw_req_create_audio_play(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t workspace_name, h2_gizclaw_str_t history_name,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (service == NULL || out_request == NULL || timeout_ms == 0 ||
      timeout_ms > INT32_MAX ||
      !valid_token(workspace_name, H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES) ||
      !valid_token(history_name, H2_GIZCLAW_WORKSPACE_HISTORY_ID_MAX_BYTES))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_WorkspaceHistoryAudioDownloadRequest message =
      gizclaw_rpc_v1_WorkspaceHistoryAudioDownloadRequest_init_zero;
  text_encode_t workspace = {workspace_name.data, workspace_name.len};
  text_encode_t history = {history_name.data, history_name.len};
  message.workspace_name =
      (pb_callback_t){.funcs.encode = encode_text, .arg = &workspace};
  message.history_name =
      (pb_callback_t){.funcs.encode = encode_text, .arg = &history};
  uint8_t *payload = NULL;
  size_t payload_len = 0;
  const h2_pal_mem_api_t *allocator = service->client_config.allocator;
  h2_pal_result_t rc = (h2_pal_result_t)encode_message(
      allocator, gizclaw_rpc_v1_WorkspaceHistoryAudioDownloadRequest_fields,
      &message, &payload, &payload_len);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_audio_play_create_internal(
        service, identity, workspace_name, history_name,
        (h2_gizclaw_rpc_bytes_t){payload, payload_len}, timeout_ms,
        out_request);
  h2_pal_mem_free(allocator, payload);
  return rc;
}

#ifdef H2_GIZCLAW_TESTING
int h2_gizclaw_workspace_decode_activation_for_test(
    h2_gizclaw_resp_storage_t *storage, const uint8_t *data, size_t len,
    h2_gizclaw_workspace_activation_t *out_result) {
  if (out_result == NULL || data == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  h2_gizclaw_resp_arena_t arena;
  h2_pal_result_t rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_workspace_activation_t result = {0};
  rc = h2_gizclaw_resp_arena_end(
      &arena,
      (h2_pal_result_t)decode_activation(&arena.allocator, data, len, &result));
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}

int h2_gizclaw_workspace_decode_history_list_for_test(
    h2_gizclaw_resp_storage_t *storage, const uint8_t *data, size_t len,
    size_t max_count, h2_gizclaw_workspace_history_page_t *out_result) {
  if (out_result == NULL || data == NULL || max_count == 0u ||
      max_count > H2_GIZCLAW_WORKSPACE_HISTORY_PAGE_MAX_ITEMS)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  h2_gizclaw_resp_arena_t arena;
  h2_pal_result_t rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_workspace_history_page_t result = {0};
  rc = h2_gizclaw_resp_arena_end(
      &arena, (h2_pal_result_t)decode_history_list(&arena.allocator, data, len,
                                                   max_count, &result));
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}
#endif
