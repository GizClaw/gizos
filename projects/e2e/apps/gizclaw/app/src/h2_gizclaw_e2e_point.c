#include "h2_gizclaw_e2e_point.h"

#include <string.h>

#define POINT_TIMEOUT_MS 30000u
#define POINT_LIMIT 32u
#define POINT_MAX_PAGES 32u
#define POINT_CURSOR_CAPACITY 1024u

static int checked(const char *symbol, const char *stage, int rc) {
  h2_gizclaw_e2e_evidence(symbol, stage, rc);
  return rc;
}

/* Parsed pointers must belong to the caller's used arena, not the request. */
static bool in_storage(const h2_gizclaw_resp_storage_t *storage,
                       const void *data, size_t size) {
  uintptr_t base = (uintptr_t)storage->data, address = (uintptr_t)data;
  return storage->used <= storage->capacity && address >= base &&
         address - base <= storage->used &&
         size <= storage->used - (address - base);
}

static bool text_valid(const h2_gizclaw_resp_storage_t *storage,
                       h2_gizclaw_owned_text_t text, bool required) {
  if (text.len == 0u && text.data == NULL)
    return !required;
  return (!required || text.len > 0u) && text.len < SIZE_MAX &&
         in_storage(storage, text.data, text.len + 1u) &&
         text.data[text.len] == '\0' &&
         memchr(text.data, '\0', text.len) == NULL;
}

static int validate_page(const h2_gizclaw_resp_storage_t *storage,
                         const h2_gizclaw_points_transaction_page_t *page) {
  if (storage->used > storage->capacity || page->count > POINT_LIMIT ||
      (page->count != 0u &&
       !in_storage(storage, page->items, page->count * sizeof(*page->items))) ||
      !text_valid(storage, page->next_cursor, page->has_next))
    return H2_PAL_ERR_INVALID_STATE;
  for (size_t i = 0u; i < page->count; ++i) {
    const h2_gizclaw_points_transaction_t *item = &page->items[i];
    if (!text_valid(storage, item->id, true) ||
        !text_valid(storage, item->created_at, true) ||
        !text_valid(storage, item->reason, false) ||
        !text_valid(storage, item->source_type, false) ||
        !text_valid(storage, item->source_id, false) ||
        !text_valid(storage, item->game_result_id, false) ||
        !text_valid(storage, item->pet_name, false) ||
        !text_valid(storage, item->reward_grant_id, false))
      return H2_PAL_ERR_INVALID_STATE;
    for (size_t j = 0u; j < i; ++j)
      if (item->id.len == page->items[j].id.len &&
          memcmp(item->id.data, page->items[j].id.data, item->id.len) == 0)
        return H2_PAL_ERR_INVALID_STATE;
  }
  return H2_PAL_OK;
}

static int call(h2_gizclaw_e2e_fixture_t *fixture,
                h2_gizclaw_resp_storage_t *storage, bool req_api, bool list,
                uint64_t identity, h2_gizclaw_str_t cursor,
                h2_gizclaw_points_account_t *account,
                h2_gizclaw_points_transaction_page_t *page) {
  storage->used = 0u;
  if (!h2_gizclaw_e2e_fixture_has_time(fixture, POINT_TIMEOUT_MS))
    return H2_PAL_ERR_TIMEOUT;
  h2_gizclaw_service_t *service = fixture->actors[H2_GIZCLAW_E2E_OWNER].service;
  if (!req_api) {
    if (list)
      return checked(
          "h2_gizclaw_rpc_point_transaction_list", "point-rpc",
          h2_gizclaw_rpc_point_transaction_list(
              service, cursor, POINT_LIMIT, POINT_TIMEOUT_MS, storage, page));
    return checked(
        "h2_gizclaw_rpc_point_get", "point-rpc",
        h2_gizclaw_rpc_point_get(service, POINT_TIMEOUT_MS, storage, account));
  }
  h2_gizclaw_req_t *request = NULL;
  int rc = list ? checked("h2_gizclaw_req_create_point_transaction_list",
                          "point-req",
                          h2_gizclaw_req_create_point_transaction_list(
                              service, identity, cursor, POINT_LIMIT,
                              POINT_TIMEOUT_MS, &request))
                : checked("h2_gizclaw_req_create_point_get", "point-req",
                          h2_gizclaw_req_create_point_get(
                              service, identity, POINT_TIMEOUT_MS, &request));
  if (rc == H2_PAL_OK)
    rc = checked("h2_gizclaw_req_do", "point-req",
                 h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL));
  if (rc == H2_PAL_OK)
    rc = checked("h2_gizclaw_req_wait", "point-req",
                 h2_gizclaw_req_wait(request, POINT_TIMEOUT_MS));
  if (rc == H2_PAL_OK)
    rc = list ? checked("h2_gizclaw_resp_parse_point_transaction_list",
                        "point-req",
                        h2_gizclaw_resp_parse_point_transaction_list(
                            request, storage, page))
              : checked(
                    "h2_gizclaw_resp_parse_point_get", "point-req",
                    h2_gizclaw_resp_parse_point_get(request, storage, account));
  if (request != NULL) {
    if (rc != H2_PAL_OK)
      (void)checked("h2_gizclaw_req_cancel", "point-cleanup",
                    h2_gizclaw_req_cancel(request));
    h2_gizclaw_req_release(request);
  }
  return rc;
}

int h2_gizclaw_e2e_run_point(h2_gizclaw_e2e_fixture_t *fixture,
                             h2_gizclaw_resp_storage_t *storage) {
  if (fixture == NULL || storage == NULL || storage->data == NULL ||
      storage->capacity == 0u ||
      fixture->actors[H2_GIZCLAW_E2E_OWNER].service == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  uint64_t identity = 61u;
  int rc = H2_PAL_OK;
  for (unsigned api = 0u; api < 2u && rc == H2_PAL_OK; ++api) {
    const bool req_api = api != 0u;
    h2_gizclaw_points_account_t account = {0};
    rc = call(fixture, storage, req_api, false, identity++,
              (h2_gizclaw_str_t){0}, &account, NULL);
    if (rc == H2_PAL_OK)
      rc = checked(req_api ? "h2_gizclaw_resp_parse_point_get"
                           : "h2_gizclaw_rpc_point_get",
                   "point_get-assert",
                   text_valid(storage, account.updated_at, true)
                       ? H2_PAL_OK
                       : H2_PAL_ERR_INVALID_STATE);
    // Initial balance is server/profile-owned; never assume zero or a fixed
    // reward. Concurrent rewards can change snapshots between these calls.
    char cursor[POINT_CURSOR_CAPACITY] = {0};
    size_t cursor_len = 0u;
    for (unsigned index = 0u; index < POINT_MAX_PAGES && rc == H2_PAL_OK;
         ++index) {
      h2_gizclaw_points_transaction_page_t page = {0};
      rc = call(fixture, storage, req_api, true, identity++,
                (h2_gizclaw_str_t){cursor, cursor_len}, NULL, &page);
      if (rc != H2_PAL_OK)
        break;
      rc = validate_page(storage, &page);
      if (rc == H2_PAL_OK && page.has_next) {
        if (page.next_cursor.len >= sizeof(cursor))
          rc = H2_PAL_ERR_TRUNCATED;
        else if ((page.next_cursor.len == cursor_len &&
                  memcmp(page.next_cursor.data, cursor, cursor_len) == 0) ||
                 index + 1u == POINT_MAX_PAGES)
          rc = H2_PAL_ERR_INVALID_STATE;
        else {
          cursor_len = page.next_cursor.len;
          memcpy(cursor, page.next_cursor.data, cursor_len + 1u);
        }
      }
      checked(req_api ? "h2_gizclaw_resp_parse_point_transaction_list"
                      : "h2_gizclaw_rpc_point_transaction_list",
              "point_transaction_list-assert", rc);
      if (!page.has_next)
        break;
    }
  }
  storage->used = 0u;
  return rc;
}
