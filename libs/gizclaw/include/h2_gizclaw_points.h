#ifndef H2_GIZCLAW_POINTS_H
#define H2_GIZCLAW_POINTS_H

#include "h2_gizclaw_config.h"
#include "h2_gizclaw_service.h"
#include "h2_gizclaw_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Owned UTF-8 text returned by a Points operation. */
typedef struct h2_gizclaw_owned_text {
  char *data;
  size_t len;
} h2_gizclaw_owned_text_t;

/** Server-authoritative Points account snapshot. */
typedef struct h2_gizclaw_points_account {
  int64_t balance;
  h2_gizclaw_owned_text_t updated_at;
} h2_gizclaw_points_account_t;

/**
 * Server-authoritative Points ledger entry.
 *
 * Public `id` and relationship `*_id` fields are semantic identifiers. The
 * adapter copies the pinned Peer wire `name` and `*_name` values into these
 * fields verbatim; callers must not normalize, derive, or translate them.
 */
typedef struct h2_gizclaw_points_transaction {
  int64_t balance_after;
  int64_t delta;
  h2_gizclaw_owned_text_t created_at;
  h2_gizclaw_owned_text_t id;
  h2_gizclaw_owned_text_t reason;
  h2_gizclaw_owned_text_t source_type;
  h2_gizclaw_owned_text_t source_id;
  h2_gizclaw_owned_text_t game_result_id;
  h2_gizclaw_owned_text_t pet_name;
  h2_gizclaw_owned_text_t reward_grant_id;
} h2_gizclaw_points_transaction_t;

/** Owned page returned by server.points.transactions.list. */
typedef struct h2_gizclaw_points_transaction_page {
  h2_gizclaw_points_transaction_t *items;
  size_t count;
  bool has_next;
  h2_gizclaw_owned_text_t next_cursor;
} h2_gizclaw_points_transaction_page_t;

h2_pal_result_t h2_gizclaw_req_create_point_get(h2_gizclaw_service_t *service,
                                                uint64_t identity,
                                                uint32_t timeout_ms,
                                                h2_gizclaw_req_t **out_request);
h2_pal_result_t h2_gizclaw_req_create_point_transaction_list(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t cursor,
    size_t limit, uint32_t timeout_ms, h2_gizclaw_req_t **out_request);

/** Result pointers live in storage, not in request. NO_SPACE rolls back the
 * storage checkpoint and leaves output empty; retry with a larger buffer. */
h2_pal_result_t
h2_gizclaw_resp_parse_point_get(const h2_gizclaw_req_t *request,
                                h2_gizclaw_resp_storage_t *storage,
                                h2_gizclaw_points_account_t *out_account);
h2_pal_result_t h2_gizclaw_resp_parse_point_transaction_list(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_points_transaction_page_t *out_page);

h2_pal_result_t
h2_gizclaw_rpc_point_get(h2_gizclaw_service_t *service, uint32_t timeout_ms,
                         h2_gizclaw_resp_storage_t *storage,
                         h2_gizclaw_points_account_t *out_account);
h2_pal_result_t h2_gizclaw_rpc_point_transaction_list(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t cursor, size_t limit,
    uint32_t timeout_ms, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_points_transaction_page_t *out_page);

#ifdef __cplusplus
}
#endif

#endif
