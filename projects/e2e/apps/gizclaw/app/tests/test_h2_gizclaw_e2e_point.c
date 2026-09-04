#include "h2_gizclaw_e2e_point.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Public API doubles around the actual case. No server or live acceptance. */
struct h2_gizclaw_req {
  bool list, submitted, complete;
  char cursor[1024];
};
static struct {
  struct h2_gizclaw_req req;
  bool alive, emit, empty;
  unsigned step, fail_at, replies, fault_at, fault, pagination;
  unsigned deadlines, expire_at, creates, releases, cancels, proofs;
  unsigned calls[6];
} state;

static int step(void) {
  return ++state.step == state.fail_at ? H2_PAL_ERR_IO : H2_PAL_OK;
}
bool h2_gizclaw_e2e_fixture_has_time(const h2_gizclaw_e2e_fixture_t *f,
                                     uint32_t ms) {
  assert(f != NULL && ms == 30000u && !state.alive);
  return ++state.deadlines != state.expire_at;
}
void h2_gizclaw_e2e_evidence(const char *symbol, const char *stage, int rc) {
  assert(symbol != NULL && stage != NULL);
  if (strstr(stage, "-assert") != NULL && rc == H2_PAL_OK)
    ++state.proofs;
  if (state.emit)
    printf("H2_GIZCLAW_E2E symbol=%s stage=%s result=%s rc=%d\n", symbol, stage,
           rc == H2_PAL_OK ? "PASS" : "FAIL", rc);
}
static h2_gizclaw_owned_text_t save(h2_gizclaw_resp_storage_t *storage,
                                    const char *s) {
  size_t len = strlen(s);
  assert(storage->used + len + 1u <= storage->capacity);
  char *data = (char *)storage->data + storage->used;
  memcpy(data, s, len + 1u);
  storage->used += len + 1u;
  return (h2_gizclaw_owned_text_t){data, len};
}
static void corrupt_text(h2_gizclaw_owned_text_t *text, unsigned fault,
                         h2_gizclaw_resp_storage_t *storage) {
  switch (fault) {
  case 1:
    *text = (h2_gizclaw_owned_text_t){0};
    break;
  case 2:
    text->data = NULL;
    break;
  case 3:
    text->len = SIZE_MAX;
    break;
  case 4:
    text->data[text->len] = 'x';
    break;
  case 5:
    text->data[0] = '\0';
    break;
  case 6:
    text->data = (char *)&state.req;
    break;
  case 7:
    storage->used = storage->capacity + 1u;
    break;
  }
}
static void reply(h2_gizclaw_resp_storage_t *storage, bool list,
                  h2_gizclaw_str_t cursor, h2_gizclaw_points_account_t *account,
                  h2_gizclaw_points_transaction_page_t *page) {
  assert(storage->used == 0u);
  memset(storage->data, 0xa5, storage->capacity); // Poison all prior responses.
  unsigned fault = ++state.replies == state.fault_at ? state.fault : 0u;
  if (!list) {
    assert(account != NULL);
    account->balance = INT64_MIN; // Server-authoritative; not a closed range.
    account->updated_at = save(storage, "2026-09-03T01:02:03Z");
    corrupt_text(&account->updated_at, fault, storage);
    return;
  }
  assert(page != NULL && (cursor.len == 0u || cursor.data != NULL));
  // Caller must have copied the prior response cursor before arena reuse.
  if (cursor.len != 0u)
    assert(cursor.len == 1u &&
           (cursor.data[0] == 'a' || cursor.data[0] == 'b'));
  page->count = state.empty ? 0u : 2u;
  page->items =
      page->count ? (h2_gizclaw_points_transaction_t *)storage->data : NULL;
  storage->used = page->count * sizeof(*page->items);
  memset(storage->data, 0, storage->used);
  for (size_t i = 0u; i < page->count; ++i) {
    page->items[i].id = save(storage, i == 0u ? "opaque/id:a" : "opaque/id:b");
    page->items[i].created_at = save(storage, "2026-09-03T01:02:03Z");
    page->items[i].reason = save(storage, "future-unknown-reason");
    page->items[i].source_type = save(storage, "future-source-type");
    page->items[i].delta = i == 0u ? INT64_MIN : INT64_MAX;
    page->items[i].balance_after = INT64_MIN;
  }
  page->has_next =
      state.pagination != 0u && (cursor.len == 0u || state.pagination != 1u);
  if (page->has_next)
    page->next_cursor = save(storage, state.pagination == 3u && cursor.len &&
                                              cursor.data[0] == 'a'
                                          ? "b"
                                          : "a");
  if (fault <= 7u && page->count)
    corrupt_text(&page->items[0].id, fault, storage);
  switch (fault) {
  case 8:
    page->count = 33u;
    break;
  case 9:
    page->items = NULL;
    break;
  case 10:
    page->items[1].id = page->items[0].id;
    break;
  case 11:
    page->items[0].created_at = (h2_gizclaw_owned_text_t){0};
    break;
  case 12:
    page->has_next = true;
    break;
  case 13:
    page->has_next = true;
    page->next_cursor = save(storage, "");
    break;
  case 14:
    page->items[0].source_id = (h2_gizclaw_owned_text_t){NULL, 1u};
    break;
  case 15: {
    char oversized[1025];
    memset(oversized, 'x', sizeof(oversized));
    oversized[sizeof(oversized) - 1u] = '\0';
    page->next_cursor = save(storage, oversized);
    page->has_next = true;
    break;
  }
  }
}
static int create(bool list, h2_gizclaw_service_t *service, uint64_t identity,
                  h2_gizclaw_str_t cursor, uint32_t ms,
                  h2_gizclaw_req_t **out) {
  assert(service != NULL && identity >= 61u && ms == 30000u);
  assert(!state.alive && out != NULL && *out == NULL);
  ++state.calls[list ? 1 : 0];
  int rc = step();
  if (rc == H2_PAL_OK) {
    memset(&state.req, 0, sizeof(state.req));
    state.req.list = list;
    assert(cursor.len < sizeof(state.req.cursor));
    if (cursor.len)
      memcpy(state.req.cursor, cursor.data, cursor.len);
    *out = &state.req;
    state.alive = true;
    ++state.creates;
  }
  return rc;
}
h2_pal_result_t h2_gizclaw_req_create_point_get(h2_gizclaw_service_t *s,
                                                uint64_t id, uint32_t ms,
                                                h2_gizclaw_req_t **out) {
  return create(false, s, id, (h2_gizclaw_str_t){0}, ms, out);
}
h2_pal_result_t h2_gizclaw_req_create_point_transaction_list(
    h2_gizclaw_service_t *s, uint64_t id, h2_gizclaw_str_t cursor, size_t limit,
    uint32_t ms, h2_gizclaw_req_t **out) {
  assert(limit == 32u);
  return create(true, s, id, cursor, ms, out);
}
h2_pal_result_t h2_gizclaw_req_do(h2_gizclaw_req_t *r,
                                  void *user,
                                  h2_gizclaw_req_input_read_fn input_read,
                                  h2_gizclaw_req_output_write_fn output_write,
                                  h2_gizclaw_req_complete_fn on_complete) {
  (void)on_complete;
  assert(state.alive && r == &state.req && !r->submitted);
  assert(user == NULL && input_read == NULL && output_write == NULL);
  int rc = step();
  r->submitted = rc == H2_PAL_OK;
  return rc;
}
h2_pal_result_t h2_gizclaw_req_wait(h2_gizclaw_req_t *r, uint32_t ms) {
  assert(state.alive && r == &state.req && r->submitted && ms == 30000u);
  int rc = step();
  r->complete = rc == H2_PAL_OK;
  return rc;
}
static int parse(const h2_gizclaw_req_t *r, bool list,
                 h2_gizclaw_resp_storage_t *storage,
                 h2_gizclaw_points_account_t *account,
                 h2_gizclaw_points_transaction_page_t *page) {
  assert(state.alive && r == &state.req && r->complete && r->list == list);
  ++state.calls[list ? 3 : 2];
  int rc = step();
  if (rc == H2_PAL_OK)
    reply(storage, list, (h2_gizclaw_str_t){r->cursor, strlen(r->cursor)},
          account, page);
  return rc;
}
h2_pal_result_t
h2_gizclaw_resp_parse_point_get(const h2_gizclaw_req_t *r,
                                h2_gizclaw_resp_storage_t *storage,
                                h2_gizclaw_points_account_t *out) {
  return parse(r, false, storage, out, NULL);
}
h2_pal_result_t h2_gizclaw_resp_parse_point_transaction_list(
    const h2_gizclaw_req_t *r, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_points_transaction_page_t *out) {
  return parse(r, true, storage, NULL, out);
}
h2_pal_result_t h2_gizclaw_req_cancel(h2_gizclaw_req_t *r) {
  assert(state.alive && r == &state.req);
  ++state.cancels;
  return H2_PAL_ERR_CLOSED;
}
void h2_gizclaw_req_release(h2_gizclaw_req_t *r) {
  assert(state.alive && r == &state.req);
  state.alive = false;
  ++state.releases;
  memset(r, 0xa5, sizeof(*r));
}
h2_pal_result_t h2_gizclaw_rpc_point_get(h2_gizclaw_service_t *s, uint32_t ms,
                                         h2_gizclaw_resp_storage_t *storage,
                                         h2_gizclaw_points_account_t *out) {
  assert(s != NULL && ms == 30000u && !state.alive);
  ++state.calls[4];
  int rc = step();
  if (rc == H2_PAL_OK)
    reply(storage, false, (h2_gizclaw_str_t){0}, out, NULL);
  return rc;
}
h2_pal_result_t h2_gizclaw_rpc_point_transaction_list(
    h2_gizclaw_service_t *s, h2_gizclaw_str_t cursor, size_t limit, uint32_t ms,
    h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_points_transaction_page_t *out) {
  assert(s != NULL && limit == 32u && ms == 30000u && !state.alive);
  ++state.calls[5];
  int rc = step();
  if (rc == H2_PAL_OK)
    reply(storage, true, cursor, NULL, out);
  return rc;
}
static int run(h2_gizclaw_e2e_fixture_t *f,
               h2_gizclaw_resp_storage_t *storage) {
  int rc = h2_gizclaw_e2e_run_point(f, storage);
  assert(!state.alive && state.creates == state.releases &&
         storage->used == 0u);
  return rc;
}
int main(int argc, char **argv) {
  (void)argv;
  h2_gizclaw_e2e_fixture_t f = {0};
  f.actors[H2_GIZCLAW_E2E_OWNER].service = (void *)&state;
  _Alignas(max_align_t) unsigned char bytes[8192];
  h2_gizclaw_resp_storage_t storage = {bytes, sizeof(bytes), 0u};
  state.emit = argc > 1;
  if (state.emit)
    puts("H2_GIZCLAW_E2E stage=coverage-begin case=rpc\n"
         "H2_GIZCLAW_E2E stage=coverage-begin case=rpc/gameplay");
  assert(run(&f, &storage) == H2_PAL_OK);
  assert(state.step == 10u && state.proofs == 4u && state.deadlines == 4u);
  for (unsigned i = 0u; i < 6u; ++i)
    assert(state.calls[i] == 1u);
  if (state.emit) {
    puts("H2_GIZCLAW_E2E stage=coverage-end case=rpc/gameplay status=PASS rc=0 "
         "cleanup_rc=0\n"
         "H2_GIZCLAW_E2E stage=coverage-end case=rpc status=PASS rc=0 "
         "cleanup_rc=0");
    return 0;
  }
  for (unsigned fail = 1u; fail <= 10u; ++fail) {
    memset(&state, 0, sizeof(state));
    state.fail_at = fail;
    assert(run(&f, &storage) == H2_PAL_ERR_IO && state.step == fail);
  }
  for (unsigned expire = 1u; expire <= 4u; ++expire) {
    memset(&state, 0, sizeof(state));
    state.expire_at = expire;
    assert(run(&f, &storage) == H2_PAL_ERR_TIMEOUT);
  }
  for (unsigned at = 1u; at <= 4u; ++at)
    for (unsigned fault = 1u; fault <= (at % 2u ? 7u : 15u); ++fault) {
      memset(&state, 0, sizeof(state));
      state.fault_at = at;
      state.fault = fault;
      assert(run(&f, &storage) ==
             (fault == 15u ? H2_PAL_ERR_TRUNCATED : H2_PAL_ERR_INVALID_STATE));
    }
  for (unsigned mode = 1u; mode <= 3u; ++mode) {
    memset(&state, 0, sizeof(state));
    state.pagination = mode;
    assert(run(&f, &storage) ==
           (mode == 1u ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE));
    if (mode == 3u)
      assert(state.deadlines == 33u);
  }
  memset(&state, 0, sizeof(state));
  state.empty = true;
  assert(run(&f, &storage) == H2_PAL_OK);
  assert(h2_gizclaw_e2e_run_point(NULL, &storage) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_e2e_run_point(&f, NULL) == H2_PAL_ERR_INVALID_ARG);
  f.actors[H2_GIZCLAW_E2E_OWNER].service = NULL;
  assert(h2_gizclaw_e2e_run_point(&f, &storage) == H2_PAL_ERR_INVALID_ARG);
  puts("Point E2E case boundary tests passed (not live E2E)");
  return 0;
}
