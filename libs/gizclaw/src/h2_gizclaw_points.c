#include "h2_gizclaw_points.h"
#include "h2_gizclaw_internal.h"

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
  h2_gizclaw_points_transaction_t *items =
      h2_pal_mem_realloc(context->allocator, context->page->items,
                         (count + 1u) * sizeof(context->page->items[0]));
  if (items == NULL) {
    return false;
  }
  context->page->items = items;
  h2_gizclaw_points_transaction_t *out = &items[count];
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

static int rpc_result_status(const h2_gizclaw_rpc_response_t *response) {
  return response != NULL && !response->has_error ? H2_PAL_OK : H2_PAL_ERR_IO;
}

int h2_gizclaw_client_points_get(h2_gizclaw_client_t *client,
                                 h2_gizclaw_points_account_t *out_account) {
  if (client == NULL || out_account == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_account, 0, sizeof(*out_account));
  const h2_pal_mem_api_t *allocator =
      h2_gizclaw_client_allocator_internal(client);
  if (allocator == NULL) {
    return H2_PAL_ERR_INVALID_STATE;
  }

  gizclaw_rpc_v1_ServerPointsGetRequest request =
      gizclaw_rpc_v1_ServerPointsGetRequest_init_zero;
  uint8_t *payload = NULL;
  size_t payload_len = 0u;
  int rc =
      encode_message(allocator, gizclaw_rpc_v1_ServerPointsGetRequest_fields,
                     &request, &payload, &payload_len);
  h2_gizclaw_rpc_response_t response = {0};
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_client_rpc_call(
        client, H2_GIZCLAW_RPC_SERVER_POINTS_GET,
        (h2_gizclaw_rpc_bytes_t){.data = payload, .len = payload_len},
        &response);
  }
  h2_pal_mem_free(allocator, payload);
  if (rc == H2_PAL_OK)
    rc = rpc_result_status(&response);
  if (rc == H2_PAL_OK) {
    gizclaw_rpc_v1_ServerPointsGetResponse decoded =
        gizclaw_rpc_v1_ServerPointsGetResponse_init_zero;
    h2_gizclaw_points_text_decode_context_t updated_at_context;
    set_text_decoder(&decoded.value.updated_at, allocator,
                     &out_account->updated_at, &updated_at_context);
    pb_istream_t stream = pb_istream_from_buffer(response.result_payload,
                                                 response.result_payload_len);
    if (!pb_decode(&stream, gizclaw_rpc_v1_ServerPointsGetResponse_fields,
                   &decoded) ||
        !decoded.has_value) {
      rc = H2_PAL_ERR_FORMAT;
    } else {
      out_account->balance = decoded.value.balance;
    }
  }
  h2_gizclaw_rpc_response_deinit(client, &response);
  if (rc != H2_PAL_OK) {
    h2_gizclaw_points_account_deinit(client, out_account);
  }
  return rc;
}

int h2_gizclaw_client_points_transactions_list(
    h2_gizclaw_client_t *client, h2_gizclaw_str_t cursor, size_t limit,
    h2_gizclaw_points_transaction_page_t *out_page) {
  if (client == NULL || out_page == NULL || limit == 0u ||
#if SIZE_MAX > INT64_MAX
      limit > (size_t)INT64_MAX ||
#endif
      (cursor.len > 0u && cursor.data == NULL)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_page, 0, sizeof(*out_page));
  const h2_pal_mem_api_t *allocator =
      h2_gizclaw_client_allocator_internal(client);
  if (allocator == NULL) {
    return H2_PAL_ERR_INVALID_STATE;
  }

  gizclaw_rpc_v1_ServerPointsTransactionListRequest request =
      gizclaw_rpc_v1_ServerPointsTransactionListRequest_init_zero;
  request.has_value = true;
  request.value.has_limit = true;
  request.value.limit = (int64_t)limit;
  h2_gizclaw_points_encode_text_t cursor_text = {
      .data = cursor.data,
      .len = cursor.len,
  };
  if (cursor.len > 0u) {
    request.value.cursor.funcs.encode = encode_text;
    request.value.cursor.arg = &cursor_text;
  }
  uint8_t *payload = NULL;
  size_t payload_len = 0u;
  int rc = encode_message(
      allocator, gizclaw_rpc_v1_ServerPointsTransactionListRequest_fields,
      &request, &payload, &payload_len);
  h2_gizclaw_rpc_response_t response = {0};
  if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_client_rpc_call(
        client, H2_GIZCLAW_RPC_SERVER_POINTS_TRANSACTIONS_LIST,
        (h2_gizclaw_rpc_bytes_t){.data = payload, .len = payload_len},
        &response);
  }
  h2_pal_mem_free(allocator, payload);
  if (rc == H2_PAL_OK)
    rc = rpc_result_status(&response);
  if (rc == H2_PAL_OK) {
    gizclaw_rpc_v1_ServerPointsTransactionListResponse decoded =
        gizclaw_rpc_v1_ServerPointsTransactionListResponse_init_zero;
    h2_gizclaw_points_page_decode_context_t items_context = {
        .allocator = allocator,
        .page = out_page,
        .max_count = limit,
    };
    h2_gizclaw_points_text_decode_context_t cursor_context;
    decoded.value.items.funcs.decode = decode_transaction;
    decoded.value.items.arg = &items_context;
    set_text_decoder(&decoded.value.next_cursor, allocator,
                     &out_page->next_cursor, &cursor_context);
    pb_istream_t stream = pb_istream_from_buffer(response.result_payload,
                                                 response.result_payload_len);
    if (!pb_decode(&stream,
                   gizclaw_rpc_v1_ServerPointsTransactionListResponse_fields,
                   &decoded) ||
        !decoded.has_value) {
      rc = H2_PAL_ERR_FORMAT;
    } else {
      out_page->has_next = decoded.value.has_next;
    }
  }
  h2_gizclaw_rpc_response_deinit(client, &response);
  if (rc != H2_PAL_OK) {
    h2_gizclaw_points_transaction_page_deinit(client, out_page);
  }
  return rc;
}

void h2_gizclaw_points_account_deinit(h2_gizclaw_client_t *client,
                                      h2_gizclaw_points_account_t *account) {
  if (account == NULL)
    return;
  const h2_pal_mem_api_t *allocator =
      h2_gizclaw_client_allocator_internal(client);
  owned_text_deinit(allocator, &account->updated_at);
  memset(account, 0, sizeof(*account));
}

void h2_gizclaw_points_transaction_page_deinit(
    h2_gizclaw_client_t *client, h2_gizclaw_points_transaction_page_t *page) {
  if (page == NULL)
    return;
  const h2_pal_mem_api_t *allocator =
      h2_gizclaw_client_allocator_internal(client);
  for (size_t index = 0u; index < page->count; ++index) {
    transaction_deinit(allocator, &page->items[index]);
  }
  if (allocator != NULL)
    h2_pal_mem_free(allocator, page->items);
  owned_text_deinit(allocator, &page->next_cursor);
  memset(page, 0, sizeof(*page));
}
