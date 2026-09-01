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

typedef struct h2_gizclaw_points_request h2_gizclaw_points_request_t;

typedef void (*h2_gizclaw_points_get_completion_fn)(
    void *user, h2_gizclaw_points_request_t *request);

typedef void (*h2_gizclaw_points_list_completion_fn)(
    void *user, h2_gizclaw_points_request_t *request);

#if defined(H2_GIZCLAW_TESTING)
int h2_gizclaw_client_points_get(h2_gizclaw_client_t *client,
                                 h2_gizclaw_points_account_t *out_account);
int h2_gizclaw_client_points_transactions_list(
    h2_gizclaw_client_t *client, h2_gizclaw_str_t cursor, size_t limit,
    h2_gizclaw_points_transaction_page_t *out_page);
#endif

void h2_gizclaw_points_account_deinit(h2_gizclaw_client_t *client,
                                      h2_gizclaw_points_account_t *account);
void h2_gizclaw_points_transaction_page_deinit(
    h2_gizclaw_client_t *client, h2_gizclaw_points_transaction_page_t *page);

/**
 * Read the current caller's Points account through server.points.get.
 *
 * The Server resolves the account from the caller's Runtime Profile. The
 * returned snapshot is borrowed during the completion callback and remains
 * owned by the request until release.
 */
h2_pal_result_t h2_gizclaw_service_points_get_async(
    h2_gizclaw_service_t *service, uint64_t identity, uint32_t timeout_ms,
    h2_gizclaw_points_get_completion_fn completion, void *user,
    h2_gizclaw_points_request_t **out_request);

/**
 * List the current caller's Points ledger through
 * server.points.transactions.list.
 *
 * `cursor` is encoded and copied before return; an empty cursor requests the
 * first page. `limit` must be positive. The returned page is borrowed during
 * the completion callback and remains owned by the request until release.
 */
h2_pal_result_t h2_gizclaw_service_points_transactions_list_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t cursor,
    size_t limit, uint32_t timeout_ms,
    h2_gizclaw_points_list_completion_fn completion, void *user,
    h2_gizclaw_points_request_t **out_request);

h2_pal_result_t
h2_gizclaw_points_request_cancel(h2_gizclaw_points_request_t *request);
h2_pal_result_t h2_gizclaw_points_request_wait(
    h2_gizclaw_points_request_t *request, uint32_t timeout_ms);
const h2_gizclaw_operation_result_t *h2_gizclaw_points_request_operation_result(
    const h2_gizclaw_points_request_t *request);
const h2_gizclaw_points_account_t *h2_gizclaw_points_request_account(
    const h2_gizclaw_points_request_t *request);
const h2_gizclaw_points_transaction_page_t *h2_gizclaw_points_request_page(
    const h2_gizclaw_points_request_t *request);

/** Release the terminal request and all result storage borrowed by callback. */
void h2_gizclaw_points_request_release(h2_gizclaw_points_request_t *request);

#ifdef __cplusplus
}
#endif

#endif
