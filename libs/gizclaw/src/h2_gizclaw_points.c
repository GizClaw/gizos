#include "h2_gizclaw_points.h"
#include "h2_gizclaw_internal.h"
#include "h2_gizclaw_response_internal.h"
#include "h2_gizclaw_service_internal.h"

#include "payload/gameplay.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

typedef struct h2_gizclaw_points_page_decode_context {
  const h2_pal_mem_api_t *allocator;
  h2_gizclaw_points_transaction_page_t *page;
  size_t max_count;
  size_t capacity;
} h2_gizclaw_points_page_decode_context_t;

typedef struct h2_gizclaw_points_text_decode_context {
  const h2_pal_mem_api_t *allocator;
  h2_gizclaw_owned_text_t *text;
} h2_gizclaw_points_text_decode_context_t;

typedef struct h2_gizclaw_points_encode_text {
  const char *data;
  size_t len;
} h2_gizclaw_points_encode_text_t;

static void owned_text_deinit(const h2_pal_mem_api_t *allocator,
                              h2_gizclaw_owned_text_t *text) {
  if (text == NULL)
    return;
  if (allocator != NULL)
    h2_pal_mem_free(allocator, text->data);
  *text = (h2_gizclaw_owned_text_t){0};
}

static bool decode_owned_text(pb_istream_t *stream, const pb_field_t *field,
                              void **arg) {
  (void)field;
  h2_gizclaw_points_text_decode_context_t *context = (*arg);
  if (context == NULL || context->allocator == NULL || context->text == NULL ||
      context->text->data != NULL) {
    return false;
  }
  if (stream->bytes_left == SIZE_MAX) {
    return false;
  }
  char *data = h2_pal_mem_alloc(context->allocator, stream->bytes_left + 1u);
  if (data == NULL) {
    return false;
  }
  const size_t len = stream->bytes_left;
  if (!pb_read(stream, (pb_byte_t *)data, len)) {
    h2_pal_mem_free(context->allocator, data);
    return false;
  }
  data[len] = '\0';
  context->text->data = data;
  context->text->len = len;
  return true;
}

static void set_text_decoder(pb_callback_t *callback,
                             const h2_pal_mem_api_t *allocator,
                             h2_gizclaw_owned_text_t *out_text,
                             h2_gizclaw_points_text_decode_context_t *context) {
  context->allocator = allocator;
  context->text = out_text;
  callback->funcs.decode = decode_owned_text;
  callback->arg = context;
}

static void transaction_deinit(const h2_pal_mem_api_t *allocator,
                               h2_gizclaw_points_transaction_t *transaction) {
  if (transaction == NULL)
    return;
  owned_text_deinit(allocator, &transaction->created_at);
  owned_text_deinit(allocator, &transaction->id);
  owned_text_deinit(allocator, &transaction->reason);
  owned_text_deinit(allocator, &transaction->source_type);
  owned_text_deinit(allocator, &transaction->source_id);
  owned_text_deinit(allocator, &transaction->game_result_id);
  owned_text_deinit(allocator, &transaction->pet_name);
  owned_text_deinit(allocator, &transaction->reward_grant_id);
  memset(transaction, 0, sizeof(*transaction));
}

static bool decode_transaction(pb_istream_t *stream, const pb_field_t *field,
                               void **arg) {
  (void)field;
  h2_gizclaw_points_page_decode_context_t *context = (*arg);
  if (context == NULL || context->allocator == NULL || context->page == NULL ||
      context->page->count >= context->max_count ||
      context->page->count == SIZE_MAX) {
    return false;
  }
  const size_t count = context->page->count;
  if (count + 1u > SIZE_MAX / sizeof(context->page->items[0])) {
    return false;
  }
  if (count == context->capacity) {
    size_t capacity = context->capacity == 0u ? 1u : context->capacity;
    if (capacity <= SIZE_MAX / 2u)
      capacity *= 2u;
    if (capacity > context->max_count)
      capacity = context->max_count;
    if (capacity < count + 1u ||
        capacity > SIZE_MAX / sizeof(context->page->items[0]))
      return false;
    h2_gizclaw_points_transaction_t *items = h2_pal_mem_realloc(
        context->allocator, context->page->items, capacity * sizeof(*items));
    if (items == NULL)
      return false;
    context->page->items = items;
    context->capacity = capacity;
  }
  h2_gizclaw_points_transaction_t *out = &context->page->items[count];
  memset(out, 0, sizeof(*out));

  gizclaw_rpc_v1_PointsTransaction decoded =
      gizclaw_rpc_v1_PointsTransaction_init_zero;
  h2_gizclaw_points_text_decode_context_t text_contexts[8];
  set_text_decoder(&decoded.created_at, context->allocator, &out->created_at,
                   &text_contexts[0]);
  set_text_decoder(&decoded.name, context->allocator, &out->id,
                   &text_contexts[1]);
  set_text_decoder(&decoded.reason, context->allocator, &out->reason,
                   &text_contexts[2]);
  set_text_decoder(&decoded.source_type, context->allocator, &out->source_type,
                   &text_contexts[3]);
  set_text_decoder(&decoded.source_name, context->allocator, &out->source_id,
                   &text_contexts[4]);
  set_text_decoder(&decoded.game_result_name, context->allocator,
                   &out->game_result_id, &text_contexts[5]);
  set_text_decoder(&decoded.pet_name, context->allocator, &out->pet_name,
                   &text_contexts[6]);
  set_text_decoder(&decoded.reward_grant_name, context->allocator,
                   &out->reward_grant_id, &text_contexts[7]);
  if (!pb_decode(stream, gizclaw_rpc_v1_PointsTransaction_fields, &decoded)) {
    transaction_deinit(context->allocator, out);
    return false;
  }
  out->balance_after = decoded.balance_after;
  out->delta = decoded.delta;
  context->page->count = count + 1u;
  return true;
}

static bool encode_text(pb_ostream_t *stream, const pb_field_t *field,
                        void *const *arg) {
  const h2_gizclaw_points_encode_text_t *text = (*arg);
  if (text == NULL || (text->len > 0u && text->data == NULL)) {
    return false;
  }
  return pb_encode_tag_for_field(stream, field) &&
         pb_encode_string(stream, (const pb_byte_t *)text->data, text->len);
}

static int encode_message(const h2_pal_mem_api_t *allocator,
                          const pb_msgdesc_t *fields, const void *message,
                          uint8_t **out_data, size_t *out_len) {
  if (allocator == NULL || fields == NULL || message == NULL ||
      out_data == NULL || out_len == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_data = NULL;
  *out_len = 0u;
  pb_ostream_t sizing = PB_OSTREAM_SIZING;
  if (!pb_encode(&sizing, fields, message)) {
    return H2_PAL_ERR_FORMAT;
  }
  uint8_t *data = h2_pal_mem_alloc(
      allocator, sizing.bytes_written == 0u ? 1u : sizing.bytes_written);
  if (data == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  pb_ostream_t stream = pb_ostream_from_buffer(data, sizing.bytes_written);
  if (!pb_encode(&stream, fields, message)) {
    h2_pal_mem_free(allocator, data);
    return H2_PAL_ERR_FORMAT;
  }
  *out_data = data;
  *out_len = stream.bytes_written;
  return H2_PAL_OK;
}

typedef struct points_decode_context {
  const h2_pal_mem_api_t *allocator;
  size_t limit;
  union {
    h2_gizclaw_points_account_t account;
    h2_gizclaw_points_transaction_page_t page;
  } result;
} points_decode_context_t;

static h2_pal_result_t
decode_account(points_decode_context_t *request,
               const h2_gizclaw_rpc_response_t *response) {
  gizclaw_rpc_v1_ServerPointsGetResponse decoded =
      gizclaw_rpc_v1_ServerPointsGetResponse_init_zero;
  h2_gizclaw_points_text_decode_context_t updated_at_context;
  set_text_decoder(&decoded.value.updated_at, request->allocator,
                   &request->result.account.updated_at, &updated_at_context);
  pb_istream_t stream = pb_istream_from_buffer(response->result_payload,
                                               response->result_payload_len);
  if (!pb_decode(&stream, gizclaw_rpc_v1_ServerPointsGetResponse_fields,
                 &decoded) ||
      !decoded.has_value) {
    return H2_PAL_ERR_FORMAT;
  }
  request->result.account.balance = decoded.value.balance;
  return H2_PAL_OK;
}

static h2_pal_result_t decode_page(points_decode_context_t *request,
                                   const h2_gizclaw_rpc_response_t *response) {
  gizclaw_rpc_v1_ServerPointsTransactionListResponse decoded =
      gizclaw_rpc_v1_ServerPointsTransactionListResponse_init_zero;
  h2_gizclaw_points_page_decode_context_t items_context = {
      .allocator = request->allocator,
      .page = &request->result.page,
      .max_count = request->limit,
  };
  h2_gizclaw_points_text_decode_context_t cursor_context;
  decoded.value.items.funcs.decode = decode_transaction;
  decoded.value.items.arg = &items_context;
  set_text_decoder(&decoded.value.next_cursor, request->allocator,
                   &request->result.page.next_cursor, &cursor_context);
  pb_istream_t stream = pb_istream_from_buffer(response->result_payload,
                                               response->result_payload_len);
  if (!pb_decode(&stream,
                 gizclaw_rpc_v1_ServerPointsTransactionListResponse_fields,
                 &decoded) ||
      !decoded.has_value) {
    return H2_PAL_ERR_FORMAT;
  }
  request->result.page.has_next = decoded.value.has_next;
  return H2_PAL_OK;
}

static const char point_get_tag;
static const char point_list_tag;

h2_pal_result_t
h2_gizclaw_req_create_point_get(h2_gizclaw_service_t *service,
                                uint64_t identity, uint32_t timeout_ms,
                                h2_gizclaw_req_t **out_request) {
  return h2_gizclaw_req_create_rpc_internal(
      service, identity, H2_GIZCLAW_RPC_SERVER_POINTS_GET, &point_get_tag,
      (h2_gizclaw_rpc_bytes_t){0}, timeout_ms, out_request);
}

h2_pal_result_t h2_gizclaw_req_create_point_transaction_list(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t cursor,
    size_t limit, uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (service == NULL || limit == 0u ||
#if SIZE_MAX > INT64_MAX
      limit > INT64_MAX ||
#endif
      (cursor.len != 0u && cursor.data == NULL) || out_request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_ServerPointsTransactionListRequest message =
      gizclaw_rpc_v1_ServerPointsTransactionListRequest_init_zero;
  message.has_value = true;
  message.value.has_limit = true;
  message.value.limit = (int64_t)limit;
  h2_gizclaw_points_encode_text_t cursor_text = {cursor.data, cursor.len};
  if (cursor.len != 0u) {
    message.value.cursor.funcs.encode = encode_text;
    message.value.cursor.arg = &cursor_text;
  }
  const h2_pal_mem_api_t *allocator = service->client_config.allocator;
  uint8_t *payload = NULL;
  size_t payload_len = 0u;
  h2_pal_result_t rc = (h2_pal_result_t)encode_message(
      allocator, gizclaw_rpc_v1_ServerPointsTransactionListRequest_fields,
      &message, &payload, &payload_len);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_create_rpc_internal(
        service, identity, H2_GIZCLAW_RPC_SERVER_POINTS_TRANSACTIONS_LIST,
        &point_list_tag, (h2_gizclaw_rpc_bytes_t){payload, payload_len},
        timeout_ms, out_request);
  h2_pal_mem_free(allocator, payload);
  return rc;
}

h2_pal_result_t
h2_gizclaw_resp_parse_point_get(const h2_gizclaw_req_t *request,
                                h2_gizclaw_resp_storage_t *storage,
                                h2_gizclaw_points_account_t *out_account) {
  if (out_account == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_account, 0, sizeof(*out_account));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc =
      h2_gizclaw_req_response_internal(request, &point_get_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  points_decode_context_t decoded = {.allocator = &arena.allocator};
  rc = h2_gizclaw_resp_arena_end(&arena, decode_account(&decoded, response));
  if (rc == H2_PAL_OK)
    *out_account = decoded.result.account;
  return rc;
}

h2_pal_result_t h2_gizclaw_resp_parse_point_transaction_list(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_points_transaction_page_t *out_page) {
  if (out_page == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_page, 0, sizeof(*out_page));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc =
      h2_gizclaw_req_response_internal(request, &point_list_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_rpc_bytes_t params;
  rc = h2_gizclaw_req_input_internal(request, &point_list_tag, &params);
  if (rc != H2_PAL_OK)
    return rc;
  gizclaw_rpc_v1_ServerPointsTransactionListRequest input =
      gizclaw_rpc_v1_ServerPointsTransactionListRequest_init_zero;
  pb_istream_t stream = pb_istream_from_buffer(params.data, params.len);
  if (!pb_decode(&stream,
                 gizclaw_rpc_v1_ServerPointsTransactionListRequest_fields,
                 &input) ||
      !input.has_value || !input.value.has_limit || input.value.limit <= 0)
    return H2_PAL_ERR_FORMAT;
  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  points_decode_context_t decoded = {
      .allocator = &arena.allocator,
      .limit = (size_t)input.value.limit,
  };
  rc = h2_gizclaw_resp_arena_end(&arena, decode_page(&decoded, response));
  if (rc == H2_PAL_OK)
    *out_page = decoded.result.page;
  return rc;
}

h2_pal_result_t
h2_gizclaw_rpc_point_get(h2_gizclaw_service_t *service, uint32_t timeout_ms,
                         h2_gizclaw_resp_storage_t *storage,
                         h2_gizclaw_points_account_t *out_account) {
  if (out_account == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_account, 0, sizeof(*out_account));
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc =
      h2_gizclaw_req_create_point_get(service, 0u, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_point_get(request, storage, out_account);
  h2_gizclaw_req_release(request);
  return rc;
}

h2_pal_result_t h2_gizclaw_rpc_point_transaction_list(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t cursor, size_t limit,
    uint32_t timeout_ms, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_points_transaction_page_t *out_page) {
  if (out_page == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_page, 0, sizeof(*out_page));
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_point_transaction_list(
      service, 0u, cursor, limit, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_point_transaction_list(request, storage,
                                                      out_page);
  h2_gizclaw_req_release(request);
  return rc;
}
