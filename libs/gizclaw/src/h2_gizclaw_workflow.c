#include "h2_gizclaw_workflow.h"

#include "h2_gizclaw_internal.h"
#include "h2_gizclaw_rpc.h"
#include "h2_gizclaw_service_internal.h"

#include "payload/ai.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include <limits.h>
#include <stdatomic.h>
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
  h2_gizclaw_workflow_t *items =
      h2_pal_mem_realloc(context->allocator, context->page->items,
                         (count + 1u) * sizeof(context->page->items[0]));
  if (items == NULL)
    return false;
  context->page->items = items;
  memset(&items[count], 0, sizeof(items[count]));
  if (!decode_workflow_object(stream, &items[count], context->allocator))
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
  if (response->error_code == H2_GIZCLAW_RPC_ERROR_METHOD_NOT_FOUND)
    return H2_PAL_ERR_UNSUPPORTED;
  return H2_PAL_ERR_IO;
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
    if (!pb_decode(&metadata_stream,
                   gizclaw_rpc_v1_WorkflowGetResponse_fields, &metadata) ||
        *out_runtime_profile_name == NULL ||
        *out_runtime_profile_revision == NULL) {
      rc = H2_PAL_ERR_FORMAT;
    }
  }
  return rc;
}

int h2_gizclaw_client_workflows_list(h2_gizclaw_client_t *client,
                                     h2_gizclaw_str_t collection,
                                     h2_gizclaw_str_t cursor, size_t limit,
                                     h2_gizclaw_workflow_page_t *out_page) {
  if (client == NULL || out_page == NULL ||
      !valid_kebab(collection, H2_GIZCLAW_WORKFLOW_COLLECTION_MAX_BYTES) ||
      !valid_optional_text(cursor, 255u) || limit == 0u ||
      limit > H2_GIZCLAW_WORKFLOW_PAGE_MAX_ITEMS
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
  gizclaw_rpc_v1_WorkflowListRequest request =
      gizclaw_rpc_v1_WorkflowListRequest_init_zero;
  text_encode_t collection_text = {
      .data = collection.data,
      .len = collection.len,
  };
  text_encode_t cursor_text = {
      .data = cursor.data,
      .len = cursor.len,
  };
  request.collection.funcs.encode = encode_text;
  request.collection.arg = &collection_text;
  if (cursor.len > 0u) {
    request.cursor.funcs.encode = encode_text;
    request.cursor.arg = &cursor_text;
  }
  request.has_limit = true;
  request.limit = (int64_t)limit;
  uint8_t *payload = NULL;
  size_t payload_len = 0u;
  int rc = encode_message(allocator, gizclaw_rpc_v1_WorkflowListRequest_fields,
                          &request, &payload, &payload_len);
  h2_gizclaw_rpc_response_t response = {0};
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_client_rpc_call(
        client, H2_GIZCLAW_RPC_SERVER_WORKFLOW_LIST,
        (h2_gizclaw_rpc_bytes_t){.data = payload, .len = payload_len},
        &response);
  }
  h2_pal_mem_free(allocator, payload);
  if (rc == H2_PAL_OK)
    rc = response_status(&response);
  if (rc == H2_PAL_OK)
    rc = decode_list(allocator, response.result_payload,
                     response.result_payload_len, limit, out_page);
  h2_gizclaw_rpc_response_deinit(client, &response);
  if (rc != H2_PAL_OK)
    h2_gizclaw_workflow_page_deinit(client, out_page);
  return rc;
}

int h2_gizclaw_client_workflow_get(h2_gizclaw_client_t *client,
                                   h2_gizclaw_str_t name,
                                   h2_gizclaw_workflow_t *out_workflow,
                                   char **out_runtime_profile_name,
                                   char **out_runtime_profile_revision) {
  if (client == NULL || out_workflow == NULL ||
      out_runtime_profile_name == NULL ||
      out_runtime_profile_revision == NULL ||
      !h2_gizclaw_runtime_alias_valid_internal(name)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_workflow, 0, sizeof(*out_workflow));
  *out_runtime_profile_name = NULL;
  *out_runtime_profile_revision = NULL;
  const h2_pal_mem_api_t *allocator =
      h2_gizclaw_client_allocator_internal(client);
  if (allocator == NULL)
    return H2_PAL_ERR_INVALID_STATE;
  gizclaw_rpc_v1_WorkflowGetRequest request =
      gizclaw_rpc_v1_WorkflowGetRequest_init_zero;
  text_encode_t name_text = {.data = name.data, .len = name.len};
  request.name.funcs.encode = encode_text;
  request.name.arg = &name_text;
  uint8_t *payload = NULL;
  size_t payload_len = 0u;
  int rc = encode_message(allocator, gizclaw_rpc_v1_WorkflowGetRequest_fields,
                          &request, &payload, &payload_len);
  h2_gizclaw_rpc_response_t response = {0};
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_client_rpc_call(
        client, H2_GIZCLAW_RPC_SERVER_WORKFLOW_GET,
        (h2_gizclaw_rpc_bytes_t){.data = payload, .len = payload_len},
        &response);
  }
  h2_pal_mem_free(allocator, payload);
  if (rc == H2_PAL_OK)
    rc = response_status(&response);
  if (rc == H2_PAL_OK) {
    gizclaw_rpc_v1_WorkflowGetResponse decoded =
        gizclaw_rpc_v1_WorkflowGetResponse_init_zero;
    workflow_decode_t workflow = {
        .allocator = allocator,
        .out = out_workflow,
    };
    text_decode_t text[2];
    decoded.has_value = true;
    decoded.value.name.funcs.decode = NULL;
    decoded.value.i18n.funcs.decode = NULL;
    decoded.value.collection.funcs.decode = NULL;
    decoded.value.workspace_lang_pair.funcs.decode = NULL;
    set_text_decoder(&decoded.runtime_profile_name, &text[0], allocator,
                     out_runtime_profile_name, 255u);
    set_text_decoder(&decoded.runtime_profile_revision, &text[1], allocator,
                     out_runtime_profile_revision, 255u);
    /*
     * A submessage cannot be redirected through a callback in nanopb. Decode
     * the envelope manually so the owned Workflow projection uses the same
     * bounded decoder as list items.
     */
    (void)workflow;
    pb_istream_t stream = pb_istream_from_buffer(response.result_payload,
                                                 response.result_payload_len);
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
      /*
       * Decode the envelope once more for profile metadata. The Workflow
       * callbacks are harmless skips because their fields are not selected.
       */
      gizclaw_rpc_v1_WorkflowGetResponse metadata =
          gizclaw_rpc_v1_WorkflowGetResponse_init_zero;
      set_text_decoder(&metadata.runtime_profile_name, &text[0], allocator,
                       out_runtime_profile_name, 255u);
      set_text_decoder(&metadata.runtime_profile_revision, &text[1], allocator,
                       out_runtime_profile_revision, 255u);
      pb_istream_t metadata_stream = pb_istream_from_buffer(
          response.result_payload, response.result_payload_len);
      if (!pb_decode(&metadata_stream,
                     gizclaw_rpc_v1_WorkflowGetResponse_fields, &metadata) ||
          *out_runtime_profile_name == NULL ||
          *out_runtime_profile_revision == NULL) {
        rc = H2_PAL_ERR_FORMAT;
      }
    }
  }
  h2_gizclaw_rpc_response_deinit(client, &response);
  if (rc != H2_PAL_OK) {
    workflow_clear(allocator, out_workflow);
    h2_pal_mem_free(allocator, *out_runtime_profile_name);
    h2_pal_mem_free(allocator, *out_runtime_profile_revision);
    *out_runtime_profile_name = NULL;
    *out_runtime_profile_revision = NULL;
  }
  return rc;
}

typedef enum workflow_request_kind {
  WORKFLOW_REQUEST_LIST = 0,
  WORKFLOW_REQUEST_GET,
} workflow_request_kind_t;

typedef union workflow_completion {
  h2_gizclaw_workflows_list_completion_fn list;
  h2_gizclaw_workflow_get_completion_fn get;
} workflow_completion_t;

struct h2_gizclaw_workflow_request {
  h2_gizclaw_async_rpc_t *rpc;
  const h2_pal_mem_api_t *allocator;
  workflow_request_kind_t kind;
  size_t limit;
  workflow_completion_t completion;
  void *completion_user;
  h2_gizclaw_operation_result_t operation_result;
  union {
    h2_gizclaw_workflow_page_t page;
    struct {
      h2_gizclaw_workflow_t workflow;
      char *profile_name;
      char *profile_revision;
    } get;
  } result;
  atomic_bool terminal;
};

static void workflow_page_clear(const h2_pal_mem_api_t *allocator,
                                h2_gizclaw_workflow_page_t *page) {
  for (size_t index = 0u; index < page->count; ++index)
    workflow_clear(allocator, &page->items[index]);
  h2_pal_mem_free(allocator, page->items);
  h2_pal_mem_free(allocator, page->next_cursor);
  h2_pal_mem_free(allocator, page->runtime_profile_name);
  h2_pal_mem_free(allocator, page->runtime_profile_revision);
  memset(page, 0, sizeof(*page));
}

static void workflow_rpc_complete(void *user, h2_gizclaw_async_rpc_t *rpc) {
  const h2_gizclaw_operation_result_t *operation_result =
      h2_gizclaw_async_rpc_operation_result(rpc);
  const h2_gizclaw_rpc_response_t *response =
      h2_gizclaw_async_rpc_response(rpc);
  h2_gizclaw_workflow_request_t *request = user;
  h2_gizclaw_operation_result_t result = *operation_result;
  if (result.result == H2_PAL_OK)
    result.result = (h2_pal_result_t)response_status(response);
  if (result.result == H2_PAL_OK) {
    if (request->kind == WORKFLOW_REQUEST_LIST) {
      result.result = (h2_pal_result_t)decode_list(
          request->allocator, response->result_payload,
          response->result_payload_len, request->limit, &request->result.page);
    } else {
      result.result = (h2_pal_result_t)decode_get(
          request->allocator, response->result_payload,
          response->result_payload_len, &request->result.get.workflow,
          &request->result.get.profile_name,
          &request->result.get.profile_revision);
    }
  }
  if (result.result != H2_PAL_OK) {
    if (request->kind == WORKFLOW_REQUEST_LIST) {
      workflow_page_clear(request->allocator, &request->result.page);
    } else {
      workflow_clear(request->allocator, &request->result.get.workflow);
      h2_pal_mem_free(request->allocator, request->result.get.profile_name);
      h2_pal_mem_free(request->allocator,
                      request->result.get.profile_revision);
      request->result.get.profile_name = NULL;
      request->result.get.profile_revision = NULL;
    }
  }
  request->operation_result = result;
  atomic_store_explicit(&request->terminal, true, memory_order_release);
  if (request->kind == WORKFLOW_REQUEST_LIST)
    request->completion.list(request->completion_user, request);
  else
    request->completion.get(request->completion_user, request);
}

static h2_pal_result_t submit_workflow_request(
    h2_gizclaw_service_t *service, uint64_t identity,
    workflow_request_kind_t kind, h2_gizclaw_rpc_method_t method,
    h2_gizclaw_rpc_bytes_t payload, size_t limit, uint32_t timeout_ms,
    workflow_completion_t completion, void *user,
    h2_gizclaw_workflow_request_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (service == NULL || timeout_ms == 0u ||
      (kind == WORKFLOW_REQUEST_LIST ? completion.list == NULL
                                     : completion.get == NULL) ||
      out_request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  const h2_pal_mem_api_t *allocator = service->config.client_config->allocator;
  h2_gizclaw_workflow_request_t *request =
      h2_pal_mem_alloc(allocator, sizeof(*request));
  if (request == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(request, 0, sizeof(*request));
  request->allocator = allocator;
  request->kind = kind;
  request->limit = limit;
  request->completion_user = user;
  request->completion = completion;
  const h2_pal_result_t rc = h2_gizclaw_service_rpc_call_async(
      service, identity, method, payload, timeout_ms, workflow_rpc_complete,
      request, &request->rpc);
  if (rc != H2_PAL_OK) {
    h2_pal_mem_free(allocator, request);
    return rc;
  }
  *out_request = request;
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_service_workflows_list_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    h2_gizclaw_str_t collection, h2_gizclaw_str_t cursor, size_t limit,
    uint32_t timeout_ms, h2_gizclaw_workflows_list_completion_fn completion,
    void *user, h2_gizclaw_workflow_request_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (service == NULL ||
      !valid_kebab(collection, H2_GIZCLAW_WORKFLOW_COLLECTION_MAX_BYTES) ||
      !valid_optional_text(cursor, 255u) || limit == 0u ||
      limit > H2_GIZCLAW_WORKFLOW_PAGE_MAX_ITEMS
#if SIZE_MAX > INT64_MAX
      || limit > (size_t)INT64_MAX
#endif
  ) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  const h2_pal_mem_api_t *allocator = service->config.client_config->allocator;
  gizclaw_rpc_v1_WorkflowListRequest message =
      gizclaw_rpc_v1_WorkflowListRequest_init_zero;
  text_encode_t collection_text = {
      .data = collection.data, .len = collection.len};
  text_encode_t cursor_text = {.data = cursor.data, .len = cursor.len};
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
      allocator, gizclaw_rpc_v1_WorkflowListRequest_fields, &message,
      &payload, &payload_len);
  if (rc == H2_PAL_OK) {
    rc = submit_workflow_request(
        service, identity, WORKFLOW_REQUEST_LIST,
        H2_GIZCLAW_RPC_SERVER_WORKFLOW_LIST,
        (h2_gizclaw_rpc_bytes_t){.data = payload, .len = payload_len}, limit,
        timeout_ms, (workflow_completion_t){.list = completion}, user,
        out_request);
  }
  h2_pal_mem_free(allocator, payload);
  return rc;
}

h2_pal_result_t h2_gizclaw_service_workflow_get_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t name,
    uint32_t timeout_ms, h2_gizclaw_workflow_get_completion_fn completion,
    void *user, h2_gizclaw_workflow_request_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (service == NULL || !h2_gizclaw_runtime_alias_valid_internal(name))
    return H2_PAL_ERR_INVALID_ARG;
  const h2_pal_mem_api_t *allocator = service->config.client_config->allocator;
  gizclaw_rpc_v1_WorkflowGetRequest message =
      gizclaw_rpc_v1_WorkflowGetRequest_init_zero;
  text_encode_t name_text = {.data = name.data, .len = name.len};
  message.name.funcs.encode = encode_text;
  message.name.arg = &name_text;
  uint8_t *payload = NULL;
  size_t payload_len = 0u;
  h2_pal_result_t rc = (h2_pal_result_t)encode_message(
      allocator, gizclaw_rpc_v1_WorkflowGetRequest_fields, &message, &payload,
      &payload_len);
  if (rc == H2_PAL_OK) {
    rc = submit_workflow_request(
        service, identity, WORKFLOW_REQUEST_GET,
        H2_GIZCLAW_RPC_SERVER_WORKFLOW_GET,
        (h2_gizclaw_rpc_bytes_t){.data = payload, .len = payload_len}, 0u,
        timeout_ms, (workflow_completion_t){.get = completion}, user,
        out_request);
  }
  h2_pal_mem_free(allocator, payload);
  return rc;
}

h2_pal_result_t
h2_gizclaw_workflow_request_cancel(h2_gizclaw_workflow_request_t *request) {
  return request == NULL ? H2_PAL_ERR_INVALID_ARG
                         : h2_gizclaw_async_rpc_cancel(request->rpc);
}

h2_pal_result_t h2_gizclaw_workflow_request_wait(
    h2_gizclaw_workflow_request_t *request, uint32_t timeout_ms) {
  return request == NULL ? H2_PAL_ERR_INVALID_ARG
                         : h2_gizclaw_async_rpc_wait(request->rpc, timeout_ms);
}

const h2_gizclaw_operation_result_t *
h2_gizclaw_workflow_request_operation_result(
    const h2_gizclaw_workflow_request_t *request) {
  return request != NULL &&
                 atomic_load_explicit(&request->terminal, memory_order_acquire)
             ? &request->operation_result
             : NULL;
}

const h2_gizclaw_workflow_page_t *h2_gizclaw_workflow_request_page(
    const h2_gizclaw_workflow_request_t *request) {
  const h2_gizclaw_operation_result_t *result =
      h2_gizclaw_workflow_request_operation_result(request);
  return result != NULL && result->result == H2_PAL_OK &&
                 request->kind == WORKFLOW_REQUEST_LIST
             ? &request->result.page
             : NULL;
}

const h2_gizclaw_workflow_t *h2_gizclaw_workflow_request_workflow(
    const h2_gizclaw_workflow_request_t *request) {
  const h2_gizclaw_operation_result_t *result =
      h2_gizclaw_workflow_request_operation_result(request);
  return result != NULL && result->result == H2_PAL_OK &&
                 request->kind == WORKFLOW_REQUEST_GET
             ? &request->result.get.workflow
             : NULL;
}

const char *h2_gizclaw_workflow_request_runtime_profile_name(
    const h2_gizclaw_workflow_request_t *request) {
  return h2_gizclaw_workflow_request_workflow(request) != NULL
             ? request->result.get.profile_name
             : NULL;
}

const char *h2_gizclaw_workflow_request_runtime_profile_revision(
    const h2_gizclaw_workflow_request_t *request) {
  return h2_gizclaw_workflow_request_workflow(request) != NULL
             ? request->result.get.profile_revision
             : NULL;
}

void h2_gizclaw_workflow_request_release(
    h2_gizclaw_workflow_request_t *request) {
  if (request == NULL ||
      !atomic_load_explicit(&request->terminal, memory_order_acquire))
    return;
  h2_gizclaw_async_rpc_release(request->rpc);
  if (request->kind == WORKFLOW_REQUEST_LIST) {
    workflow_page_clear(request->allocator, &request->result.page);
  } else {
    workflow_clear(request->allocator, &request->result.get.workflow);
    h2_pal_mem_free(request->allocator, request->result.get.profile_name);
    h2_pal_mem_free(request->allocator, request->result.get.profile_revision);
  }
  h2_pal_mem_free(request->allocator, request);
}

const char *
h2_gizclaw_workflow_display_name(const h2_gizclaw_workflow_t *workflow,
                                 const char *locale) {
  if (workflow == NULL)
    return NULL;
  if (locale != NULL) {
    for (size_t i = 0u; i < workflow->i18n_count; ++i) {
      if (workflow->i18n[i].locale != NULL &&
          strcmp(workflow->i18n[i].locale, locale) == 0) {
        return workflow->i18n[i].display_name;
      }
    }
  }
  for (size_t i = 0u; i < workflow->i18n_count; ++i) {
    if (workflow->i18n[i].locale != NULL &&
        strcmp(workflow->i18n[i].locale, "en") == 0) {
      return workflow->i18n[i].display_name;
    }
  }
  return workflow->i18n_count > 0u ? workflow->i18n[0].display_name
                                   : workflow->name;
}

const char *
h2_gizclaw_workflow_description(const h2_gizclaw_workflow_t *workflow,
                                const char *locale) {
  if (workflow == NULL)
    return NULL;
  if (locale != NULL) {
    for (size_t i = 0u; i < workflow->i18n_count; ++i) {
      if (workflow->i18n[i].locale != NULL &&
          strcmp(workflow->i18n[i].locale, locale) == 0) {
        return workflow->i18n[i].description;
      }
    }
  }
  for (size_t i = 0u; i < workflow->i18n_count; ++i) {
    if (workflow->i18n[i].locale != NULL &&
        strcmp(workflow->i18n[i].locale, "en") == 0) {
      return workflow->i18n[i].description;
    }
  }
  return workflow->i18n_count > 0u ? workflow->i18n[0].description : NULL;
}

void h2_gizclaw_workflow_deinit(h2_gizclaw_client_t *client,
                                h2_gizclaw_workflow_t *workflow) {
  if (client == NULL || workflow == NULL)
    return;
  workflow_clear(h2_gizclaw_client_allocator_internal(client), workflow);
}

void h2_gizclaw_workflow_get_deinit(h2_gizclaw_client_t *client,
                                    h2_gizclaw_workflow_t *workflow,
                                    char *runtime_profile_name,
                                    char *runtime_profile_revision) {
  if (client == NULL)
    return;
  const h2_pal_mem_api_t *allocator =
      h2_gizclaw_client_allocator_internal(client);
  workflow_clear(allocator, workflow);
  h2_pal_mem_free(allocator, runtime_profile_name);
  h2_pal_mem_free(allocator, runtime_profile_revision);
}

void h2_gizclaw_workflow_page_deinit(h2_gizclaw_client_t *client,
                                     h2_gizclaw_workflow_page_t *page) {
  if (client == NULL || page == NULL)
    return;
  const h2_pal_mem_api_t *allocator =
      h2_gizclaw_client_allocator_internal(client);
  for (size_t i = 0u; i < page->count; ++i)
    workflow_clear(allocator, &page->items[i]);
  h2_pal_mem_free(allocator, page->items);
  h2_pal_mem_free(allocator, page->next_cursor);
  h2_pal_mem_free(allocator, page->runtime_profile_name);
  h2_pal_mem_free(allocator, page->runtime_profile_revision);
  memset(page, 0, sizeof(*page));
}

#if defined(H2_GIZCLAW_TESTING)
int h2_gizclaw_workflow_decode_list_for_test(
    h2_gizclaw_client_t *client, const uint8_t *data, size_t len,
    size_t max_count, h2_gizclaw_workflow_page_t *out_page) {
  if (client == NULL || data == NULL || out_page == NULL || max_count == 0u ||
      max_count > H2_GIZCLAW_WORKFLOW_PAGE_MAX_ITEMS) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_page, 0, sizeof(*out_page));
  int rc = decode_list(h2_gizclaw_client_allocator_internal(client), data, len,
                       max_count, out_page);
  if (rc != H2_PAL_OK)
    h2_gizclaw_workflow_page_deinit(client, out_page);
  return rc;
}
#endif
