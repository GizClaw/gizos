#include "h2_gizclaw_workspace.h"

#include "h2_gizclaw_internal.h"
#include "h2_gizclaw_rpc.h"

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
} workspace_page_decode_t;

typedef struct history_page_decode {
  const h2_pal_mem_api_t *allocator;
  h2_gizclaw_workspace_history_page_t *page;
  size_t max_count;
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
  h2_gizclaw_workspace_history_entry_t *items =
      h2_pal_mem_realloc(context->allocator, context->page->items,
                         (count + 1u) * sizeof(context->page->items[0]));
  if (items == NULL)
    return false;
  context->page->items = items;
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
  h2_gizclaw_workspace_t *items =
      h2_pal_mem_realloc(context->allocator, context->page->items,
                         (count + 1u) * sizeof(context->page->items[0]));
  if (items == NULL)
    return false;
  context->page->items = items;
  memset(&items[count], 0, sizeof(items[count]));
  if (!decode_workspace_object(stream, &items[count], context->allocator))
    return false;
  context->page->count = count + 1u;
  return true;
}

static int response_status(const h2_gizclaw_rpc_response_t *response) {
  if (response == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (!response->has_error)
    return H2_PAL_OK;
  if (response->error_code == H2_GIZCLAW_RPC_ERROR_NOT_FOUND)
    return H2_PAL_ERR_NOT_FOUND;
  if (response->error_code == H2_GIZCLAW_RPC_ERROR_CONFLICT)
    return H2_PAL_ERR_INVALID_STATE;
  if (response->error_code == H2_GIZCLAW_RPC_ERROR_METHOD_NOT_FOUND)
    return H2_PAL_ERR_UNSUPPORTED;
  if (response->error_code == H2_GIZCLAW_RPC_ERROR_BAD_REQUEST ||
      response->error_code == H2_GIZCLAW_RPC_ERROR_INVALID_PARAMS)
    return H2_PAL_ERR_INVALID_ARG;
  return H2_PAL_ERR_IO;
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

static int call_message(h2_gizclaw_client_t *client,
                        h2_gizclaw_rpc_method_t method,
                        const pb_msgdesc_t *fields, const void *message,
                        h2_gizclaw_rpc_response_t *out_response) {
  const h2_pal_mem_api_t *allocator =
      h2_gizclaw_client_allocator_internal(client);
  uint8_t *payload = NULL;
  size_t payload_len = 0u;
  int rc = encode_message(allocator, fields, message, &payload, &payload_len);
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_client_rpc_call(
        client, method,
        (h2_gizclaw_rpc_bytes_t){.data = payload, .len = payload_len},
        out_response);
  }
  h2_pal_mem_free(allocator, payload);
  if (rc == H2_PAL_OK)
    rc = response_status(out_response);
  return rc;
}

int h2_gizclaw_client_workspaces_list(h2_gizclaw_client_t *client,
                                      h2_gizclaw_str_t collection,
                                      h2_gizclaw_str_t cursor, size_t limit,
                                      h2_gizclaw_workspace_page_t *out_page) {
  if (client == NULL || out_page == NULL || !valid_kebab(collection, 63u) ||
      !valid_optional_text(cursor, 255u) || limit == 0u ||
      limit > H2_GIZCLAW_WORKSPACE_PAGE_MAX_ITEMS
#if SIZE_MAX > INT64_MAX
      || limit > (size_t)INT64_MAX
#endif
  ) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_page, 0, sizeof(*out_page));
  const h2_pal_mem_api_t *allocator =
      h2_gizclaw_client_allocator_internal(client);
  if (allocator == NULL)
    return H2_PAL_ERR_INVALID_STATE;
  gizclaw_rpc_v1_WorkspaceListRequest request =
      gizclaw_rpc_v1_WorkspaceListRequest_init_zero;
  text_encode_t collection_text = {
      .data = collection.data,
      .len = collection.len,
  };
  text_encode_t cursor_text = {.data = cursor.data, .len = cursor.len};
  request.collection.funcs.encode = encode_text;
  request.collection.arg = &collection_text;
  if (cursor.len > 0u) {
    request.cursor.funcs.encode = encode_text;
    request.cursor.arg = &cursor_text;
  }
  request.has_limit = true;
  request.limit = (int64_t)limit;
  h2_gizclaw_rpc_response_t response = {0};
  int rc = call_message(client, H2_GIZCLAW_RPC_SERVER_WORKSPACE_LIST,
                        gizclaw_rpc_v1_WorkspaceListRequest_fields, &request,
                        &response);
  if (rc == H2_PAL_OK)
    rc = decode_workspace_list(allocator, response.result_payload,
                               response.result_payload_len, limit, out_page);
  if (rc == H2_PAL_OK) {
    for (size_t index = 0u; index < out_page->count; ++index) {
      out_page->items[index].collection =
          copy_owned(allocator, collection.data, collection.len);
      if (out_page->items[index].collection == NULL) {
        rc = H2_PAL_ERR_NO_MEMORY;
        break;
      }
    }
  }
  h2_gizclaw_rpc_response_deinit(client, &response);
  if (rc != H2_PAL_OK)
    h2_gizclaw_workspace_page_deinit(client, out_page);
  return rc;
}

int h2_gizclaw_client_workspace_history_list(
    h2_gizclaw_client_t *client, h2_gizclaw_str_t workspace_name,
    h2_gizclaw_str_t cursor, size_t limit,
    h2_gizclaw_workspace_history_order_t order,
    h2_gizclaw_workspace_history_page_t *out_page) {
  if (client == NULL || out_page == NULL ||
      !valid_token(workspace_name, H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES) ||
      !valid_optional_text(cursor, 255u) || limit == 0u ||
      limit > H2_GIZCLAW_WORKSPACE_HISTORY_PAGE_MAX_ITEMS ||
      (order != H2_GIZCLAW_WORKSPACE_HISTORY_ORDER_ASC &&
       order != H2_GIZCLAW_WORKSPACE_HISTORY_ORDER_DESC)
#if SIZE_MAX > INT64_MAX
      || limit > (size_t)INT64_MAX
#endif
  ) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_page, 0, sizeof(*out_page));
  const h2_pal_mem_api_t *allocator =
      h2_gizclaw_client_allocator_internal(client);
  if (allocator == NULL)
    return H2_PAL_ERR_INVALID_STATE;
  gizclaw_rpc_v1_WorkspaceHistoryListRequest request =
      gizclaw_rpc_v1_WorkspaceHistoryListRequest_init_zero;
  text_encode_t workspace_text = {
      .data = workspace_name.data,
      .len = workspace_name.len,
  };
  text_encode_t cursor_text = {.data = cursor.data, .len = cursor.len};
  request.workspace_name.funcs.encode = encode_text;
  request.workspace_name.arg = &workspace_text;
  if (cursor.len > 0u) {
    request.cursor.funcs.encode = encode_text;
    request.cursor.arg = &cursor_text;
  }
  request.has_limit = true;
  request.limit = (int64_t)limit;
  request.has_order = true;
  request.order = (gizclaw_rpc_v1_WorkspaceHistoryListRequestOrder)order;
  h2_gizclaw_rpc_response_t response = {0};
  int rc = call_message(client, H2_GIZCLAW_RPC_SERVER_WORKSPACE_HISTORY_LIST,
                        gizclaw_rpc_v1_WorkspaceHistoryListRequest_fields,
                        &request, &response);
  if (rc == H2_PAL_OK) {
    rc = decode_history_list(allocator, response.result_payload,
                             response.result_payload_len, limit, out_page);
  }
  h2_gizclaw_rpc_response_deinit(client, &response);
  if (rc != H2_PAL_OK)
    h2_gizclaw_workspace_history_page_deinit(client, out_page);
  return rc;
}

typedef struct history_audio_context {
  h2_gizclaw_client_t *client;
  h2_gizclaw_workspace_history_audio_write_fn write;
  void *user;
  h2_gizclaw_workspace_history_audio_info_t *info;
  int result;
  bool metadata_received;
} history_audio_context_t;

static void
history_audio_info_clear(const h2_pal_mem_api_t *allocator,
                         h2_gizclaw_workspace_history_audio_info_t *info) {
  if (info == NULL)
    return;
  h2_pal_mem_free(allocator, info->history_id);
  h2_pal_mem_free(allocator, info->mime_type);
  h2_pal_mem_free(allocator, info->workspace_name);
  memset(info, 0, sizeof(*info));
}

static int history_audio_event(void *user,
                               const h2_gizclaw_rpc_stream_event_t *event) {
  history_audio_context_t *context = user;
  if (event->has_error) {
    switch (event->error_code) {
    case H2_GIZCLAW_RPC_ERROR_NOT_FOUND:
      return context->result = H2_PAL_ERR_NOT_FOUND;
    case H2_GIZCLAW_RPC_ERROR_METHOD_NOT_FOUND:
      return context->result = H2_PAL_ERR_UNSUPPORTED;
    default:
      return context->result = H2_PAL_ERR_IO;
    }
  }
  if (event->kind == H2_GIZCLAW_RPC_STREAM_RESPONSE) {
    if (context->metadata_received)
      return context->result = H2_PAL_ERR_FORMAT;
    gizclaw_rpc_v1_WorkspaceHistoryAudioGetResponse decoded =
        gizclaw_rpc_v1_WorkspaceHistoryAudioGetResponse_init_zero;
    text_decode_t text[3];
    const h2_pal_mem_api_t *allocator =
        h2_gizclaw_client_allocator_internal(context->client);
    set_text_decoder(&decoded.history_name, &text[0], allocator,
                     &context->info->history_id,
                     H2_GIZCLAW_WORKSPACE_HISTORY_ID_MAX_BYTES);
    set_text_decoder(&decoded.mime_type, &text[1], allocator,
                     &context->info->mime_type, 95u);
    set_text_decoder(&decoded.workspace_name, &text[2], allocator,
                     &context->info->workspace_name,
                     H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES);
    pb_istream_t stream = pb_istream_from_buffer(event->result_payload.data,
                                                 event->result_payload.len);
    if (!pb_decode(&stream,
                   gizclaw_rpc_v1_WorkspaceHistoryAudioGetResponse_fields,
                   &decoded) ||
        decoded.size_bytes <= 0 || context->info->history_id == NULL ||
        context->info->history_id[0] == '\0' ||
        context->info->workspace_name == NULL ||
        context->info->workspace_name[0] == '\0' ||
        context->info->mime_type == NULL ||
        strncmp(context->info->mime_type, "audio/", 6u) != 0) {
      return context->result = H2_PAL_ERR_FORMAT;
    }
    context->info->size_bytes = (uint64_t)decoded.size_bytes;
    context->metadata_received = true;
  } else if (event->kind == H2_GIZCLAW_RPC_STREAM_DATA) {
    if (!context->metadata_received ||
        context->info->received_bytes > context->info->size_bytes ||
        event->data.len >
            context->info->size_bytes - context->info->received_bytes) {
      return context->result = H2_PAL_ERR_FORMAT;
    }
    const int write_rc =
        context->write(context->user, event->data.data, event->data.len);
    if (write_rc != 0)
      return context->result = write_rc;
    context->info->received_bytes += event->data.len;
  }
  return 0;
}

int h2_gizclaw_client_workspace_history_audio_get(
    h2_gizclaw_client_t *client, h2_gizclaw_str_t workspace_name,
    h2_gizclaw_str_t history_id,
    h2_gizclaw_workspace_history_audio_write_fn write, void *user,
    h2_gizclaw_workspace_history_audio_info_t *out_info) {
  if (client == NULL || write == NULL || out_info == NULL ||
      !valid_token(workspace_name, H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES) ||
      !valid_token(history_id, H2_GIZCLAW_WORKSPACE_HISTORY_ID_MAX_BYTES)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_info, 0, sizeof(*out_info));
  const h2_pal_mem_api_t *allocator =
      h2_gizclaw_client_allocator_internal(client);
  if (allocator == NULL)
    return H2_PAL_ERR_INVALID_STATE;
  gizclaw_rpc_v1_WorkspaceHistoryAudioGetRequest request =
      gizclaw_rpc_v1_WorkspaceHistoryAudioGetRequest_init_zero;
  text_encode_t text[2] = {
      {.data = history_id.data, .len = history_id.len},
      {.data = workspace_name.data, .len = workspace_name.len},
  };
  request.history_name.funcs.encode = encode_text;
  request.history_name.arg = &text[0];
  request.workspace_name.funcs.encode = encode_text;
  request.workspace_name.arg = &text[1];
  uint8_t *payload = NULL;
  size_t payload_len = 0u;
  int rc = encode_message(allocator,
                          gizclaw_rpc_v1_WorkspaceHistoryAudioGetRequest_fields,
                          &request, &payload, &payload_len);
  history_audio_context_t context = {
      .client = client,
      .write = write,
      .user = user,
      .info = out_info,
      .result = H2_PAL_OK,
  };
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_client_rpc_call_stream(
        client, H2_GIZCLAW_RPC_SERVER_WORKSPACE_HISTORY_AUDIO_GET,
        (h2_gizclaw_rpc_bytes_t){.data = payload, .len = payload_len},
        history_audio_event, &context);
  }
  h2_pal_mem_free(allocator, payload);
  if (context.result != H2_PAL_OK)
    rc = context.result;
  if (rc == H2_PAL_OK &&
      (!context.metadata_received ||
       out_info->received_bytes != out_info->size_bytes ||
       strlen(out_info->workspace_name) != workspace_name.len ||
       memcmp(out_info->workspace_name, workspace_name.data,
              workspace_name.len) != 0 ||
       strlen(out_info->history_id) != history_id.len ||
       memcmp(out_info->history_id, history_id.data, history_id.len) != 0)) {
    rc = H2_PAL_ERR_FORMAT;
  }
  if (rc != H2_PAL_OK)
    history_audio_info_clear(allocator, out_info);
  return rc;
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

int h2_gizclaw_client_workspace_get(h2_gizclaw_client_t *client,
                                    h2_gizclaw_str_t name,
                                    h2_gizclaw_workspace_t *out_workspace,
                                    char **out_runtime_profile_name,
                                    char **out_runtime_profile_revision) {
  if (client == NULL || out_workspace == NULL ||
      out_runtime_profile_name == NULL ||
      out_runtime_profile_revision == NULL ||
      !valid_token(name, H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_workspace, 0, sizeof(*out_workspace));
  *out_runtime_profile_name = NULL;
  *out_runtime_profile_revision = NULL;
  const h2_pal_mem_api_t *allocator =
      h2_gizclaw_client_allocator_internal(client);
  if (allocator == NULL)
    return H2_PAL_ERR_INVALID_STATE;
  gizclaw_rpc_v1_WorkspaceGetRequest request =
      gizclaw_rpc_v1_WorkspaceGetRequest_init_zero;
  text_encode_t name_text = {.data = name.data, .len = name.len};
  request.name.funcs.encode = encode_text;
  request.name.arg = &name_text;
  h2_gizclaw_rpc_response_t response = {0};
  int rc = call_message(client, H2_GIZCLAW_RPC_SERVER_WORKSPACE_GET,
                        gizclaw_rpc_v1_WorkspaceGetRequest_fields, &request,
                        &response);
  if (rc == H2_PAL_OK) {
    rc = decode_workspace_get(
        allocator, response.result_payload, response.result_payload_len,
        out_workspace, out_runtime_profile_name, out_runtime_profile_revision);
  }
  h2_gizclaw_rpc_response_deinit(client, &response);
  if (rc != H2_PAL_OK) {
    workspace_clear(allocator, out_workspace);
    h2_pal_mem_free(allocator, *out_runtime_profile_name);
    h2_pal_mem_free(allocator, *out_runtime_profile_revision);
    *out_runtime_profile_name = NULL;
    *out_runtime_profile_revision = NULL;
  }
  return rc;
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

static int decode_workspace_put_value(const h2_pal_mem_api_t *allocator,
                                      const uint8_t *data, size_t len,
                                      h2_gizclaw_workspace_t *out_workspace) {
  gizclaw_rpc_v1_WorkspacePutResponse decoded =
      gizclaw_rpc_v1_WorkspacePutResponse_init_zero;
  text_decode_t text[2];
  set_text_decoder(&decoded.value.name, &text[0], allocator,
                   &out_workspace->name, H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES);
  set_text_decoder(&decoded.value.workflow_name, &text[1], allocator,
                   &out_workspace->workflow_name, 63u);
  pb_istream_t stream = pb_istream_from_buffer(data, len);
  if (!pb_decode(&stream, gizclaw_rpc_v1_WorkspacePutResponse_fields,
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

int h2_gizclaw_client_workspace_create(h2_gizclaw_client_t *client,
                                       h2_gizclaw_str_t collection,
                                       h2_gizclaw_str_t workflow_name,
                                       h2_gizclaw_str_t name,
                                       h2_gizclaw_workspace_t *out_workspace) {
  if (client == NULL || out_workspace == NULL ||
      !valid_kebab(collection, 63u) ||
      !h2_gizclaw_runtime_alias_valid_internal(workflow_name) ||
      !valid_token(name, H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_workspace, 0, sizeof(*out_workspace));
  const h2_pal_mem_api_t *allocator =
      h2_gizclaw_client_allocator_internal(client);
  if (allocator == NULL)
    return H2_PAL_ERR_INVALID_STATE;
  gizclaw_rpc_v1_WorkspaceCreateRequest request =
      gizclaw_rpc_v1_WorkspaceCreateRequest_init_zero;
  text_encode_t text[3] = {
      {.data = name.data, .len = name.len},
      {.data = workflow_name.data, .len = workflow_name.len},
      {.data = collection.data, .len = collection.len},
  };
  request.has_value = true;
  request.value.name.funcs.encode = encode_text;
  request.value.name.arg = &text[0];
  request.value.workflow_name.funcs.encode = encode_text;
  request.value.workflow_name.arg = &text[1];
  request.value.collection.funcs.encode = encode_text;
  request.value.collection.arg = &text[2];
  h2_gizclaw_rpc_response_t response = {0};
  int rc = call_message(client, H2_GIZCLAW_RPC_SERVER_WORKSPACE_CREATE,
                        gizclaw_rpc_v1_WorkspaceCreateRequest_fields, &request,
                        &response);
  if (rc == H2_PAL_OK) {
    rc = decode_workspace_create_value(allocator, response.result_payload,
                                       response.result_payload_len,
                                       out_workspace);
  }
  if (rc == H2_PAL_OK) {
    out_workspace->collection =
        copy_owned(allocator, collection.data, collection.len);
    if (out_workspace->collection == NULL)
      rc = H2_PAL_ERR_NO_MEMORY;
  }
  h2_gizclaw_rpc_response_deinit(client, &response);
  if (rc != H2_PAL_OK)
    workspace_clear(allocator, out_workspace);
  return rc;
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

static int
workspace_parameters_replaceable(const uint8_t *response, size_t response_len,
                                 h2_gizclaw_workflow_driver_t driver) {
  uint32_t driver_field = 0u;
  uint32_t input_field = 0u;
  switch (driver) {
  case H2_GIZCLAW_WORKFLOW_DRIVER_FLOWCRAFT:
    driver_field = 1u;
    input_field = 4u;
    break;
  case H2_GIZCLAW_WORKFLOW_DRIVER_DOUBAO_REALTIME:
    driver_field = 2u;
    input_field = 5u;
    break;
  case H2_GIZCLAW_WORKFLOW_DRIVER_AST_TRANSLATE:
    driver_field = 3u;
    input_field = 5u;
    break;
  case H2_GIZCLAW_WORKFLOW_DRIVER_CHATROOM:
    driver_field = 4u;
    input_field = 3u;
    break;
  default:
    return H2_PAL_ERR_UNSUPPORTED;
  }

  const uint8_t *workspace = NULL;
  size_t workspace_len = 0u;
  bool found = false;
  if (!protobuf_find_bytes(response, response_len, 1u, &workspace,
                           &workspace_len, &found) ||
      !found)
    return H2_PAL_ERR_FORMAT;
  const uint8_t *parameters = NULL;
  size_t parameters_len = 0u;
  if (!protobuf_find_bytes(workspace, workspace_len, 4u, &parameters,
                           &parameters_len, &found))
    return H2_PAL_ERR_FORMAT;
  if (!found)
    return H2_PAL_OK;
  const uint8_t *typed = NULL;
  size_t typed_len = 0u;
  if (!protobuf_find_bytes(parameters, parameters_len, driver_field, &typed,
                           &typed_len, &found) ||
      !found)
    return H2_PAL_ERR_INVALID_STATE;

  size_t parameters_offset = 0u;
  while (parameters_offset < parameters_len) {
    uint64_t key = 0u;
    if (!protobuf_read_varint(parameters, parameters_len, &parameters_offset,
                              &key) ||
        key == 0u || key >> 3u > UINT32_MAX ||
        (uint32_t)(key >> 3u) != driver_field || (key & 0x07u) != 2u ||
        !protobuf_skip(parameters, parameters_len, &parameters_offset, 2u))
      return H2_PAL_ERR_FORMAT;
  }

  size_t offset = 0u;
  while (offset < typed_len) {
    uint64_t key = 0u;
    if (!protobuf_read_varint(typed, typed_len, &offset, &key) || key == 0u ||
        key >> 3u > UINT32_MAX)
      return H2_PAL_ERR_FORMAT;
    const uint32_t field = (uint32_t)(key >> 3u);
    const uint8_t wire_type = (uint8_t)(key & 0x07u);
    if ((field != 1u && field != input_field) || wire_type != 0u)
      return H2_PAL_ERR_INVALID_STATE;
    if (!protobuf_skip(typed, typed_len, &offset, wire_type))
      return H2_PAL_ERR_FORMAT;
  }
  return H2_PAL_OK;
}

static int workspace_input_preflight(h2_gizclaw_client_t *client,
                                     h2_gizclaw_str_t name,
                                     h2_gizclaw_workflow_driver_t driver) {
  gizclaw_rpc_v1_WorkspaceGetRequest request =
      gizclaw_rpc_v1_WorkspaceGetRequest_init_zero;
  text_encode_t name_text = {.data = name.data, .len = name.len};
  request.name.funcs.encode = encode_text;
  request.name.arg = &name_text;
  h2_gizclaw_rpc_response_t response = {0};
  int rc = call_message(client, H2_GIZCLAW_RPC_SERVER_WORKSPACE_GET,
                        gizclaw_rpc_v1_WorkspaceGetRequest_fields, &request,
                        &response);
  if (rc == H2_PAL_OK) {
    rc = workspace_parameters_replaceable(response.result_payload,
                                          response.result_payload_len, driver);
  }
  h2_gizclaw_rpc_response_deinit(client, &response);
  return rc;
}

static int
workspace_parameters_set_input(gizclaw_rpc_v1_WorkspaceParameters *parameters,
                               h2_gizclaw_workflow_driver_t driver,
                               h2_gizclaw_workspace_input_mode_t input_mode) {
  if (parameters == NULL || !workspace_input_mode_valid(input_mode))
    return H2_PAL_ERR_INVALID_ARG;
  const gizclaw_rpc_v1_WorkspaceInputMode wire_input =
      input_mode == H2_GIZCLAW_WORKSPACE_INPUT_REALTIME
          ? gizclaw_rpc_v1_WorkspaceInputMode_WORKSPACE_INPUT_MODE_REALTIME
          : gizclaw_rpc_v1_WorkspaceInputMode_WORKSPACE_INPUT_MODE_PUSH_TO_TALK;
  switch (driver) {
  case H2_GIZCLAW_WORKFLOW_DRIVER_FLOWCRAFT:
    parameters->which_value =
        gizclaw_rpc_v1_WorkspaceParameters_flowcraft_workspace_parameters_tag;
    parameters->value.flowcraft_workspace_parameters =
        (gizclaw_rpc_v1_FlowcraftWorkspaceParameters)
            gizclaw_rpc_v1_FlowcraftWorkspaceParameters_init_zero;
    parameters->value.flowcraft_workspace_parameters.agent_type =
        gizclaw_rpc_v1_FlowcraftWorkspaceParametersAgentType_FLOWCRAFT_WORKSPACE_PARAMETERS_AGENT_TYPE_FLOWCRAFT;
    parameters->value.flowcraft_workspace_parameters.has_input = true;
    parameters->value.flowcraft_workspace_parameters.input = wire_input;
    return H2_PAL_OK;
  case H2_GIZCLAW_WORKFLOW_DRIVER_DOUBAO_REALTIME:
    parameters->which_value =
        gizclaw_rpc_v1_WorkspaceParameters_doubao_realtime_workspace_parameters_tag;
    parameters->value.doubao_realtime_workspace_parameters =
        (gizclaw_rpc_v1_DoubaoRealtimeWorkspaceParameters)
            gizclaw_rpc_v1_DoubaoRealtimeWorkspaceParameters_init_zero;
    parameters->value.doubao_realtime_workspace_parameters.agent_type =
        gizclaw_rpc_v1_DoubaoRealtimeWorkspaceParametersAgentType_DOUBAO_REALTIME_WORKSPACE_PARAMETERS_AGENT_TYPE_DOUBAO_REALTIME;
    parameters->value.doubao_realtime_workspace_parameters.has_input = true;
    parameters->value.doubao_realtime_workspace_parameters.input = wire_input;
    return H2_PAL_OK;
  case H2_GIZCLAW_WORKFLOW_DRIVER_AST_TRANSLATE:
    parameters->which_value =
        gizclaw_rpc_v1_WorkspaceParameters_asttranslate_workspace_parameters_tag;
    parameters->value.asttranslate_workspace_parameters =
        (gizclaw_rpc_v1_ASTTranslateWorkspaceParameters)
            gizclaw_rpc_v1_ASTTranslateWorkspaceParameters_init_zero;
    parameters->value.asttranslate_workspace_parameters.agent_type =
        gizclaw_rpc_v1_ASTTranslateWorkspaceParametersAgentType_ASTTRANSLATE_WORKSPACE_PARAMETERS_AGENT_TYPE_AST_TRANSLATE;
    parameters->value.asttranslate_workspace_parameters.has_input = true;
    parameters->value.asttranslate_workspace_parameters.input = wire_input;
    return H2_PAL_OK;
  case H2_GIZCLAW_WORKFLOW_DRIVER_CHATROOM:
    parameters->which_value =
        gizclaw_rpc_v1_WorkspaceParameters_chat_room_workspace_parameters_tag;
    parameters->value.chat_room_workspace_parameters =
        (gizclaw_rpc_v1_ChatRoomWorkspaceParameters)
            gizclaw_rpc_v1_ChatRoomWorkspaceParameters_init_zero;
    parameters->value.chat_room_workspace_parameters.agent_type =
        gizclaw_rpc_v1_ChatRoomWorkspaceParametersAgentType_CHAT_ROOM_WORKSPACE_PARAMETERS_AGENT_TYPE_CHATROOM;
    parameters->value.chat_room_workspace_parameters.has_input = true;
    parameters->value.chat_room_workspace_parameters.input = wire_input;
    return H2_PAL_OK;
  case H2_GIZCLAW_WORKFLOW_DRIVER_UNSPECIFIED:
  case H2_GIZCLAW_WORKFLOW_DRIVER_PET:
  default:
    return H2_PAL_ERR_UNSUPPORTED;
  }
}

int h2_gizclaw_client_workspace_set_input(
    h2_gizclaw_client_t *client, h2_gizclaw_str_t name,
    h2_gizclaw_workflow_driver_t driver,
    h2_gizclaw_workspace_input_mode_t input_mode,
    h2_gizclaw_workspace_t *out_workspace) {
  if (out_workspace != NULL)
    memset(out_workspace, 0, sizeof(*out_workspace));
  if (client == NULL || out_workspace == NULL ||
      !valid_token(name, H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES) ||
      !workspace_input_mode_valid(input_mode)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  const h2_pal_mem_api_t *allocator =
      h2_gizclaw_client_allocator_internal(client);
  if (allocator == NULL)
    return H2_PAL_ERR_INVALID_STATE;
  gizclaw_rpc_v1_WorkspacePutRequest request =
      gizclaw_rpc_v1_WorkspacePutRequest_init_zero;
  text_encode_t name_text = {.data = name.data, .len = name.len};
  request.name.funcs.encode = encode_text;
  request.name.arg = &name_text;
  request.has_body = true;
  request.body.has_parameters = true;
  int rc = workspace_parameters_set_input(&request.body.parameters, driver,
                                          input_mode);
  if (rc != H2_PAL_OK)
    return rc;
  rc = workspace_input_preflight(client, name, driver);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_rpc_response_t response = {0};
  rc = call_message(client, H2_GIZCLAW_RPC_SERVER_WORKSPACE_PUT,
                    gizclaw_rpc_v1_WorkspacePutRequest_fields, &request,
                    &response);
  if (rc == H2_PAL_OK) {
    rc = decode_workspace_put_value(allocator, response.result_payload,
                                    response.result_payload_len, out_workspace);
  }
  h2_gizclaw_rpc_response_deinit(client, &response);
  if (rc != H2_PAL_OK)
    workspace_clear(allocator, out_workspace);
  return rc;
}

int h2_gizclaw_client_workspace_delete(h2_gizclaw_client_t *client,
                                       h2_gizclaw_str_t name,
                                       h2_gizclaw_workspace_t *out_workspace) {
  if (out_workspace == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_workspace, 0, sizeof(*out_workspace));
  if (client == NULL ||
      !valid_token(name, H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  const h2_pal_mem_api_t *allocator =
      h2_gizclaw_client_allocator_internal(client);
  if (allocator == NULL)
    return H2_PAL_ERR_INVALID_STATE;

  gizclaw_rpc_v1_WorkspaceDeleteRequest request =
      gizclaw_rpc_v1_WorkspaceDeleteRequest_init_zero;
  text_encode_t name_text = {.data = name.data, .len = name.len};
  request.name.funcs.encode = encode_text;
  request.name.arg = &name_text;
  h2_gizclaw_rpc_response_t response = {0};
  int rc = call_message(client, H2_GIZCLAW_RPC_SERVER_WORKSPACE_DELETE,
                        gizclaw_rpc_v1_WorkspaceDeleteRequest_fields, &request,
                        &response);
  if (rc == H2_PAL_OK) {
    rc = decode_workspace_delete_value(allocator, response.result_payload,
                                       response.result_payload_len,
                                       out_workspace);
  }
  h2_gizclaw_rpc_response_deinit(client, &response);
  if (rc != H2_PAL_OK)
    workspace_clear(allocator, out_workspace);
  return rc;
}

static void activation_clear(const h2_pal_mem_api_t *allocator,
                             h2_gizclaw_workspace_activation_t *activation) {
  if (activation == NULL)
    return;
  h2_pal_mem_free(allocator, activation->workspace_name);
  h2_pal_mem_free(allocator, activation->active_workspace_name);
  h2_pal_mem_free(allocator, activation->pending_workspace_name);
  h2_pal_mem_free(allocator, activation->workflow_name);
  memset(activation, 0, sizeof(*activation));
}

static int
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
      !decoded.has_value || out_activation->workspace_name == NULL) {
    return H2_PAL_ERR_FORMAT;
  }
  out_activation->runtime_state =
      (h2_gizclaw_workspace_runtime_state_t)decoded.value.runtime_state;
  return H2_PAL_OK;
}

int h2_gizclaw_client_workspace_activate(
    h2_gizclaw_client_t *client, h2_gizclaw_str_t name,
    h2_gizclaw_workspace_activation_t *out_activation) {
  if (client == NULL || out_activation == NULL ||
      !valid_token(name, H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_activation, 0, sizeof(*out_activation));
  const h2_pal_mem_api_t *allocator =
      h2_gizclaw_client_allocator_internal(client);
  if (allocator == NULL)
    return H2_PAL_ERR_INVALID_STATE;
  gizclaw_rpc_v1_ServerSetRunWorkspaceRequest request =
      gizclaw_rpc_v1_ServerSetRunWorkspaceRequest_init_zero;
  char expected_name[H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES + 1u];
  memcpy(expected_name, name.data, name.len);
  expected_name[name.len] = '\0';
  text_encode_t name_text = {.data = name.data, .len = name.len};
  request.has_value = true;
  request.value.workspace_name.funcs.encode = encode_text;
  request.value.workspace_name.arg = &name_text;
  h2_gizclaw_rpc_response_t response = {0};
  int rc = call_message(client, H2_GIZCLAW_RPC_SERVER_RUN_WORKSPACE_SET,
                        gizclaw_rpc_v1_ServerSetRunWorkspaceRequest_fields,
                        &request, &response);
  if (rc == H2_PAL_OK) {
    rc = decode_activation(allocator, response.result_payload,
                           response.result_payload_len, out_activation);
  }
  h2_gizclaw_rpc_response_deinit(client, &response);
  if (rc == H2_PAL_OK) {
    if (out_activation->workspace_name == NULL ||
        strcmp(out_activation->workspace_name, expected_name) != 0) {
      rc = H2_PAL_ERR_INVALID_STATE;
    }
  }
  if (rc == H2_PAL_OK) {
    activation_clear(allocator, out_activation);
    gizclaw_rpc_v1_ServerReloadRunWorkspaceRequest reload =
        gizclaw_rpc_v1_ServerReloadRunWorkspaceRequest_init_zero;
    memset(&response, 0, sizeof(response));
    rc = call_message(client, H2_GIZCLAW_RPC_SERVER_RUN_WORKSPACE_RELOAD,
                      gizclaw_rpc_v1_ServerReloadRunWorkspaceRequest_fields,
                      &reload, &response);
    if (rc == H2_PAL_OK) {
      rc = decode_activation(allocator, response.result_payload,
                             response.result_payload_len, out_activation);
    }
    h2_gizclaw_rpc_response_deinit(client, &response);
  }
  if (rc != H2_PAL_OK)
    activation_clear(allocator, out_activation);
  return rc;
}

bool h2_gizclaw_workspace_activation_ready(
    const h2_gizclaw_workspace_activation_t *activation,
    const char *expected_workspace_name) {
  return activation != NULL && expected_workspace_name != NULL &&
         activation->runtime_state == H2_GIZCLAW_WORKSPACE_RUNTIME_RUNNING &&
         activation->workspace_name != NULL &&
         strcmp(activation->workspace_name, expected_workspace_name) == 0 &&
         activation->active_workspace_name != NULL &&
         strcmp(activation->active_workspace_name, expected_workspace_name) ==
             0 &&
         (activation->pending_workspace_name == NULL ||
          activation->pending_workspace_name[0] == '\0');
}

void h2_gizclaw_workspace_deinit(h2_gizclaw_client_t *client,
                                 h2_gizclaw_workspace_t *workspace) {
  if (client == NULL || workspace == NULL)
    return;
  workspace_clear(h2_gizclaw_client_allocator_internal(client), workspace);
}

void h2_gizclaw_workspace_page_deinit(h2_gizclaw_client_t *client,
                                      h2_gizclaw_workspace_page_t *page) {
  if (client == NULL || page == NULL)
    return;
  const h2_pal_mem_api_t *allocator =
      h2_gizclaw_client_allocator_internal(client);
  for (size_t i = 0u; i < page->count; ++i)
    workspace_clear(allocator, &page->items[i]);
  h2_pal_mem_free(allocator, page->items);
  h2_pal_mem_free(allocator, page->next_cursor);
  h2_pal_mem_free(allocator, page->runtime_profile_name);
  h2_pal_mem_free(allocator, page->runtime_profile_revision);
  memset(page, 0, sizeof(*page));
}

void h2_gizclaw_workspace_activation_deinit(
    h2_gizclaw_client_t *client,
    h2_gizclaw_workspace_activation_t *activation) {
  if (client == NULL || activation == NULL)
    return;
  activation_clear(h2_gizclaw_client_allocator_internal(client), activation);
}

void h2_gizclaw_workspace_history_page_deinit(
    h2_gizclaw_client_t *client, h2_gizclaw_workspace_history_page_t *page) {
  if (client == NULL || page == NULL)
    return;
  const h2_pal_mem_api_t *allocator =
      h2_gizclaw_client_allocator_internal(client);
  for (size_t index = 0u; index < page->count; ++index)
    history_entry_clear(allocator, &page->items[index]);
  h2_pal_mem_free(allocator, page->items);
  h2_pal_mem_free(allocator, page->message);
  h2_pal_mem_free(allocator, page->next_cursor);
  memset(page, 0, sizeof(*page));
}

void h2_gizclaw_workspace_history_audio_info_deinit(
    h2_gizclaw_client_t *client,
    h2_gizclaw_workspace_history_audio_info_t *info) {
  if (client == NULL || info == NULL)
    return;
  history_audio_info_clear(h2_gizclaw_client_allocator_internal(client), info);
}

#if defined(H2_GIZCLAW_TESTING)
int h2_gizclaw_workspace_decode_activation_for_test(
    h2_gizclaw_client_t *client, const uint8_t *data, size_t len,
    h2_gizclaw_workspace_activation_t *out_activation) {
  if (client == NULL || data == NULL || out_activation == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_activation, 0, sizeof(*out_activation));
  const h2_pal_mem_api_t *allocator =
      h2_gizclaw_client_allocator_internal(client);
  int rc = decode_activation(allocator, data, len, out_activation);
  if (rc != H2_PAL_OK)
    activation_clear(allocator, out_activation);
  return rc;
}

int h2_gizclaw_workspace_decode_history_list_for_test(
    h2_gizclaw_client_t *client, const uint8_t *data, size_t len,
    size_t max_count, h2_gizclaw_workspace_history_page_t *out_page) {
  if (client == NULL || data == NULL || out_page == NULL || max_count == 0u ||
      max_count > H2_GIZCLAW_WORKSPACE_HISTORY_PAGE_MAX_ITEMS) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_page, 0, sizeof(*out_page));
  const h2_pal_mem_api_t *allocator =
      h2_gizclaw_client_allocator_internal(client);
  int rc = decode_history_list(allocator, data, len, max_count, out_page);
  if (rc != H2_PAL_OK)
    h2_gizclaw_workspace_history_page_deinit(client, out_page);
  return rc;
}
#endif
