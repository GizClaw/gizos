#include "h2_gizclaw_workflow.h"

#include "h2_gizclaw_internal.h"
#include "h2_gizclaw_response_internal.h"
#include "h2_gizclaw_rpc.h"
#include "h2_gizclaw_service_internal.h"

#include "payload/ai.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

typedef struct text_decode {
  const h2_pal_mem_api_t *allocator;
  char **out;
  size_t max_len;
  size_t decoded_len;
} text_decode_t;

typedef struct text_encode {
  const char *data;
  size_t len;
} text_encode_t;

typedef struct workflow_decode {
  const h2_pal_mem_api_t *allocator;
  h2_gizclaw_workflow_t *out;
} workflow_decode_t;

typedef struct workflow_page_decode {
  const h2_pal_mem_api_t *allocator;
  h2_gizclaw_workflow_page_t *page;
  size_t max_count;
  size_t capacity;
} workflow_page_decode_t;

static bool valid_kebab(h2_gizclaw_str_t value, size_t max_len) {
  if (value.data == NULL || value.len == 0u || value.len > max_len)
    return false;
  if (value.data[0] == '-' || value.data[value.len - 1u] == '-')
    return false;
  for (size_t i = 0u; i < value.len; ++i) {
    const char ch = value.data[i];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
          (ch == '-' && i > 0u && value.data[i - 1u] != '-'))) {
      return false;
    }
  }
  return true;
}

static bool valid_optional_text(h2_gizclaw_str_t value, size_t max_len) {
  return value.len <= max_len &&
         (value.len == 0u ||
          (value.data != NULL && memchr(value.data, '\0', value.len) == NULL));
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
      memchr(text, '\0', len) != NULL) {
    h2_pal_mem_free(context->allocator, text);
    return false;
  }
  text[len] = '\0';
  *context->out = text;
  context->decoded_len = len;
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

static void workflow_clear(const h2_pal_mem_api_t *allocator,
                           h2_gizclaw_workflow_t *workflow) {
  if (workflow == NULL)
    return;
  h2_pal_mem_free(allocator, workflow->collection);
  h2_pal_mem_free(allocator, workflow->name);
  h2_pal_mem_free(allocator, workflow->workspace_lang_pair);
  for (size_t i = 0u; i < workflow->i18n_count; ++i) {
    h2_pal_mem_free(allocator, workflow->i18n[i].locale);
    h2_pal_mem_free(allocator, workflow->i18n[i].display_name);
    h2_pal_mem_free(allocator, workflow->i18n[i].description);
  }
  h2_pal_mem_free(allocator, workflow->i18n);
  memset(workflow, 0, sizeof(*workflow));
}

static bool decode_i18n(pb_istream_t *stream, const pb_field_t *field,
                        void **arg) {
  (void)field;
  workflow_decode_t *context = *arg;
  h2_gizclaw_workflow_t *workflow = context == NULL ? NULL : context->out;
  if (context == NULL || context->allocator == NULL || workflow == NULL ||
      workflow->i18n_count >= H2_GIZCLAW_WORKFLOW_I18N_MAX_ITEMS) {
    return false;
  }
  const size_t index = workflow->i18n_count;
  h2_gizclaw_workflow_i18n_t *items =
      h2_pal_mem_realloc(context->allocator, workflow->i18n,
                         (index + 1u) * sizeof(workflow->i18n[0]));
  if (items == NULL)
    return false;
  workflow->i18n = items;
  memset(&items[index], 0, sizeof(items[index]));

  gizclaw_rpc_v1_Workflow_I18nEntry entry =
      gizclaw_rpc_v1_Workflow_I18nEntry_init_zero;
  text_decode_t key;
  text_decode_t display;
  text_decode_t description;
  set_text_decoder(&entry.key, &key, context->allocator, &items[index].locale,
                   H2_GIZCLAW_WORKFLOW_LOCALE_MAX_BYTES);
  entry.has_value = true;
  set_text_decoder(&entry.value.display_name, &display, context->allocator,
                   &items[index].display_name,
                   H2_GIZCLAW_WORKFLOW_DISPLAY_NAME_MAX_BYTES);
  set_text_decoder(&entry.value.description, &description, context->allocator,
                   &items[index].description,
                   H2_GIZCLAW_WORKFLOW_DESCRIPTION_MAX_BYTES);
  if (!pb_decode(stream, gizclaw_rpc_v1_Workflow_I18nEntry_fields, &entry) ||
      items[index].locale == NULL || items[index].locale[0] == '\0' ||
      items[index].display_name == NULL ||
      items[index].display_name[0] == '\0') {
    h2_pal_mem_free(context->allocator, items[index].locale);
    h2_pal_mem_free(context->allocator, items[index].display_name);
    h2_pal_mem_free(context->allocator, items[index].description);
    memset(&items[index], 0, sizeof(items[index]));
    return false;
  }
  workflow->i18n_count = index + 1u;
  return true;
}

static bool decode_workflow_object(pb_istream_t *stream,
                                   h2_gizclaw_workflow_t *out,
                                   const h2_pal_mem_api_t *allocator) {
  gizclaw_rpc_v1_Workflow decoded = gizclaw_rpc_v1_Workflow_init_zero;
  workflow_decode_t workflow_context = {
      .allocator = allocator,
      .out = out,
  };
  text_decode_t text[3];
  set_text_decoder(&decoded.name, &text[0], allocator, &out->name,
                   H2_GIZCLAW_WORKFLOW_NAME_MAX_BYTES);
  set_text_decoder(&decoded.collection, &text[1], allocator, &out->collection,
                   H2_GIZCLAW_WORKFLOW_COLLECTION_MAX_BYTES);
  set_text_decoder(&decoded.workspace_lang_pair, &text[2], allocator,
                   &out->workspace_lang_pair,
                   H2_GIZCLAW_WORKFLOW_LANG_PAIR_MAX_BYTES);
  decoded.i18n.funcs.decode = decode_i18n;
  decoded.i18n.arg = &workflow_context;
  if (!pb_decode(stream, gizclaw_rpc_v1_Workflow_fields, &decoded) ||
      out->name == NULL || out->collection == NULL ||
      !h2_gizclaw_runtime_alias_valid_internal((h2_gizclaw_str_t){
          .data = out->name,
          .len = text[0].decoded_len,
      }) ||
      !valid_kebab(
          (h2_gizclaw_str_t){
              .data = out->collection,
              .len = text[1].decoded_len,
          },
          H2_GIZCLAW_WORKFLOW_COLLECTION_MAX_BYTES)) {
    workflow_clear(allocator, out);
    return false;
  }
  return true;
}

static bool decode_workflow(pb_istream_t *stream, const pb_field_t *field,
                            void **arg) {
  (void)field;
  workflow_page_decode_t *context = *arg;
  if (context == NULL || context->allocator == NULL || context->page == NULL ||
      context->page->count >= context->max_count) {
    return false;
  }
  const size_t count = context->page->count;
  h2_gizclaw_workflow_t *items = context->page->items;
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
  if (!decode_workflow_object(stream, &items[count], context->allocator))
    return false;
  context->page->count = count + 1u;
  return true;
}

static int decode_list(const h2_pal_mem_api_t *allocator, const uint8_t *data,
                       size_t len, size_t max_count,
                       h2_gizclaw_workflow_page_t *out_page) {
  gizclaw_rpc_v1_WorkflowListResponse decoded =
      gizclaw_rpc_v1_WorkflowListResponse_init_zero;
  workflow_page_decode_t items = {
      .allocator = allocator,
      .page = out_page,
      .max_count = max_count,
  };
  text_decode_t text[3];
  decoded.items.funcs.decode = decode_workflow;
  decoded.items.arg = &items;
  set_text_decoder(&decoded.next_cursor, &text[0], allocator,
                   &out_page->next_cursor, 255u);
  set_text_decoder(&decoded.runtime_profile_name, &text[1], allocator,
                   &out_page->runtime_profile_name, 255u);
  set_text_decoder(&decoded.runtime_profile_revision, &text[2], allocator,
                   &out_page->runtime_profile_revision, 255u);
  pb_istream_t stream = pb_istream_from_buffer(data, len);
  if (!pb_decode(&stream, gizclaw_rpc_v1_WorkflowListResponse_fields,
                 &decoded) ||
      out_page->runtime_profile_name == NULL ||
      out_page->runtime_profile_revision == NULL) {
    return H2_PAL_ERR_FORMAT;
  }
  out_page->has_next = decoded.has_next;
  return H2_PAL_OK;
}

static int decode_get(const h2_pal_mem_api_t *allocator, const uint8_t *data,
                      size_t len, h2_gizclaw_workflow_t *out_workflow,
                      char **out_runtime_profile_name,
                      char **out_runtime_profile_revision) {
  int rc = H2_PAL_OK;
  pb_istream_t stream = pb_istream_from_buffer(data, len);
  bool found_value = false;
  while (stream.bytes_left > 0u && rc == H2_PAL_OK) {
    pb_wire_type_t wire_type;
    uint32_t tag;
    bool eof;
    if (!pb_decode_tag(&stream, &wire_type, &tag, &eof)) {
      rc = H2_PAL_ERR_FORMAT;
      break;
    }
    if (eof)
      break;
    if (tag == gizclaw_rpc_v1_WorkflowGetResponse_value_tag &&
        wire_type == PB_WT_STRING) {
      pb_istream_t substream;
      if (!pb_make_string_substream(&stream, &substream) ||
          !decode_workflow_object(&substream, out_workflow, allocator) ||
          !pb_close_string_substream(&stream, &substream)) {
        rc = H2_PAL_ERR_FORMAT;
      } else {
        found_value = true;
      }
    } else if (!pb_skip_field(&stream, wire_type)) {
      rc = H2_PAL_ERR_FORMAT;
    }
  }
  if (rc == H2_PAL_OK && !found_value)
    rc = H2_PAL_ERR_FORMAT;
  if (rc == H2_PAL_OK) {
    gizclaw_rpc_v1_WorkflowGetResponse metadata =
        gizclaw_rpc_v1_WorkflowGetResponse_init_zero;
    text_decode_t text[2];
    set_text_decoder(&metadata.runtime_profile_name, &text[0], allocator,
                     out_runtime_profile_name, 255u);
    set_text_decoder(&metadata.runtime_profile_revision, &text[1], allocator,
                     out_runtime_profile_revision, 255u);
    pb_istream_t metadata_stream = pb_istream_from_buffer(data, len);
    if (!pb_decode(&metadata_stream, gizclaw_rpc_v1_WorkflowGetResponse_fields,
                   &metadata) ||
        *out_runtime_profile_name == NULL ||
        *out_runtime_profile_revision == NULL) {
      rc = H2_PAL_ERR_FORMAT;
    }
  }
  return rc;
}

static const char workflow_list_tag;
static const char workflow_get_tag;

h2_pal_result_t h2_gizclaw_req_create_workflow_list(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t collection, h2_gizclaw_str_t cursor, size_t limit,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (service == NULL || out_request == NULL ||
      !valid_kebab(collection, H2_GIZCLAW_WORKFLOW_COLLECTION_MAX_BYTES) ||
      !valid_optional_text(cursor, 255u) || limit == 0u ||
      limit > H2_GIZCLAW_WORKFLOW_PAGE_MAX_ITEMS)
    return H2_PAL_ERR_INVALID_ARG;
  const h2_pal_mem_api_t *allocator = service->client_config.allocator;
  gizclaw_rpc_v1_WorkflowListRequest message =
      gizclaw_rpc_v1_WorkflowListRequest_init_zero;
  text_encode_t collection_text = {collection.data, collection.len};
  text_encode_t cursor_text = {cursor.data, cursor.len};
  message.collection.funcs.encode = encode_text;
  message.collection.arg = &collection_text;
  if (cursor.len > 0u) {
    message.cursor.funcs.encode = encode_text;
    message.cursor.arg = &cursor_text;
  }
  message.has_limit = true;
  message.limit = (int64_t)limit;
  uint8_t *payload = NULL;
  size_t payload_len = 0u;
  h2_pal_result_t rc = (h2_pal_result_t)encode_message(
      allocator, gizclaw_rpc_v1_WorkflowListRequest_fields, &message, &payload,
      &payload_len);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_create_rpc_internal(
        service, identity, H2_GIZCLAW_RPC_SERVER_WORKFLOW_LIST,
        &workflow_list_tag, (h2_gizclaw_rpc_bytes_t){payload, payload_len},
        timeout_ms, out_request);
  h2_pal_mem_free(allocator, payload);
  return rc;
}

h2_pal_result_t h2_gizclaw_req_create_workflow_get(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (service == NULL || out_request == NULL ||
      !h2_gizclaw_runtime_alias_valid_internal(name))
    return H2_PAL_ERR_INVALID_ARG;
  const h2_pal_mem_api_t *allocator = service->client_config.allocator;
  gizclaw_rpc_v1_WorkflowGetRequest message =
      gizclaw_rpc_v1_WorkflowGetRequest_init_zero;
  text_encode_t text = {name.data, name.len};
  message.name.funcs.encode = encode_text;
  message.name.arg = &text;
  uint8_t *payload = NULL;
  size_t payload_len = 0u;
  h2_pal_result_t rc = (h2_pal_result_t)encode_message(
      allocator, gizclaw_rpc_v1_WorkflowGetRequest_fields, &message, &payload,
      &payload_len);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_create_rpc_internal(
        service, identity, H2_GIZCLAW_RPC_SERVER_WORKFLOW_GET,
        &workflow_get_tag, (h2_gizclaw_rpc_bytes_t){payload, payload_len},
        timeout_ms, out_request);
  h2_pal_mem_free(allocator, payload);
  return rc;
}

static h2_pal_result_t
parse_workflow_list(h2_gizclaw_resp_storage_t *storage, const uint8_t *data,
                    size_t len, size_t limit,
                    h2_gizclaw_workflow_page_t *out_page) {
  h2_gizclaw_resp_arena_t arena;
  h2_pal_result_t rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_workflow_page_t page = {0};
  rc = h2_gizclaw_resp_arena_end(
      &arena,
      (h2_pal_result_t)decode_list(&arena.allocator, data, len, limit, &page));
  if (rc == H2_PAL_OK)
    *out_page = page;
  return rc;
}

h2_pal_result_t
h2_gizclaw_resp_parse_workflow_list(const h2_gizclaw_req_t *request,
                                    h2_gizclaw_resp_storage_t *storage,
                                    h2_gizclaw_workflow_page_t *out_page) {
  if (out_page == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_page, 0, sizeof(*out_page));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc =
      h2_gizclaw_req_response_internal(request, &workflow_list_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_rpc_bytes_t params;
  rc = h2_gizclaw_req_input_internal(request, &workflow_list_tag, &params);
  if (rc != H2_PAL_OK)
    return rc;
  gizclaw_rpc_v1_WorkflowListRequest input =
      gizclaw_rpc_v1_WorkflowListRequest_init_zero;
  pb_istream_t stream = pb_istream_from_buffer(params.data, params.len);
  if (!pb_decode(&stream, gizclaw_rpc_v1_WorkflowListRequest_fields, &input) ||
      !input.has_limit || input.limit <= 0 ||
      input.limit > H2_GIZCLAW_WORKFLOW_PAGE_MAX_ITEMS)
    return H2_PAL_ERR_FORMAT;
  return parse_workflow_list(storage, response->result_payload,
                             response->result_payload_len, (size_t)input.limit,
                             out_page);
}

h2_pal_result_t h2_gizclaw_resp_parse_workflow_get(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_workflow_get_result_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc =
      h2_gizclaw_req_response_internal(request, &workflow_get_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_workflow_get_result_t result = {0};
  rc = h2_gizclaw_resp_arena_end(
      &arena,
      (h2_pal_result_t)decode_get(
          &arena.allocator, response->result_payload,
          response->result_payload_len, &result.workflow,
          &result.runtime_profile_name, &result.runtime_profile_revision));
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}

h2_pal_result_t h2_gizclaw_rpc_workflow_list(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t collection,
    h2_gizclaw_str_t cursor, size_t limit, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_workflow_page_t *out_page) {
  if (out_page == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_page, 0, sizeof(*out_page));
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_workflow_list(
      service, 0u, collection, cursor, limit, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_workflow_list(request, storage, out_page);
  h2_gizclaw_req_release(request);
  return rc;
}

h2_pal_result_t
h2_gizclaw_rpc_workflow_get(h2_gizclaw_service_t *service,
                            h2_gizclaw_str_t name, uint32_t timeout_ms,
                            h2_gizclaw_resp_storage_t *storage,
                            h2_gizclaw_workflow_get_result_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_workflow_get(service, 0u, name,
                                                          timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_workflow_get(request, storage, out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

#ifdef H2_GIZCLAW_TESTING
int h2_gizclaw_workflow_decode_list_for_test(
    h2_gizclaw_resp_storage_t *storage, const uint8_t *data, size_t len,
    size_t max_count, h2_gizclaw_workflow_page_t *out_page) {
  if (out_page == NULL || data == NULL || max_count == 0u ||
      max_count > H2_GIZCLAW_WORKFLOW_PAGE_MAX_ITEMS)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_page, 0, sizeof(*out_page));
  return parse_workflow_list(storage, data, len, max_count, out_page);
}
#endif
