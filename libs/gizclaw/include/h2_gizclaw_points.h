#ifndef H2_GIZCLAW_POINTS_H
#define H2_GIZCLAW_POINTS_H

#include "h2_gizclaw_config.h"
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

/**
 * Read the current caller's Points account through server.points.get.
 *
 * This is a blocking unary RPC and must run on the GizClaw client owner task.
 * The Server resolves the account from the caller's Runtime Profile. On
 * success `out_account` owns its text fields; release them with
 * h2_gizclaw_points_account_deinit().
 */
int h2_gizclaw_client_points_get(h2_gizclaw_client_t *client,
                                 h2_gizclaw_points_account_t *out_account);

/**
 * List the current caller's Points ledger through
 * server.points.transactions.list.
 *
 * This is a blocking unary RPC and must run on the GizClaw client owner task.
 * `cursor` is borrowed for the duration of the call; an empty cursor requests
 * the first page. `limit` must be positive. On success `out_page` owns the
 * item array and all text fields; release them with
 * h2_gizclaw_points_transaction_page_deinit().
 */
int h2_gizclaw_client_points_transactions_list(
    h2_gizclaw_client_t *client, h2_gizclaw_str_t cursor, size_t limit,
    h2_gizclaw_points_transaction_page_t *out_page);

void h2_gizclaw_points_account_deinit(h2_gizclaw_client_t *client,
                                      h2_gizclaw_points_account_t *account);

void h2_gizclaw_points_transaction_page_deinit(
    h2_gizclaw_client_t *client, h2_gizclaw_points_transaction_page_t *page);

#ifdef __cplusplus
}
#endif

#endif
