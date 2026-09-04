#include "h2_gizclaw_e2e_contact.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Public API boundary double for the real Contact case. No network or real
 * server is involved, including the optional coverage-evidence output mode. */
enum method { CREATE, GET, PUT, LIST, DELETE };
struct value {
  char name[256], display[256], phone[65];
};
struct h2_gizclaw_req {
  enum method method;
  struct value input, response;
  char cursor[256];
  bool submitted, complete;
};
static struct {
  struct value remote, response;
  struct h2_gizclaw_req request;
  h2_gizclaw_e2e_fixture_t *fixture;
  bool exists, alive, emit;
  unsigned calls[3][5], step, fail_at, replies, corrupt_at, corrupt_field;
  unsigned changes, discard_change, deadlines, expire_at, creates, releases;
  unsigned pagination;
  uint64_t next_id;
} state;

static int step(void) {
  return ++state.step == state.fail_at ? H2_PAL_ERR_IO : H2_PAL_OK;
}
static void copy(char *out, size_t cap, h2_gizclaw_str_t in) {
  assert(in.len < cap && in.data != NULL);
  memcpy(out, in.data, in.len);
  out[in.len] = '\0';
}
h2_gizclaw_str_t h2_gizclaw_e2e_str(const char *s) {
  return (h2_gizclaw_str_t){s, strlen(s)};
}
bool h2_gizclaw_e2e_fixture_has_time(const h2_gizclaw_e2e_fixture_t *fixture,
                                     uint32_t ms) {
  assert(fixture == state.fixture && ms == 30000u);
  return ++state.deadlines != state.expire_at;
}
void h2_gizclaw_e2e_evidence(const char *symbol, const char *stage, int rc) {
  assert(symbol != NULL && stage != NULL);
  if (state.emit)
    printf("H2_GIZCLAW_E2E symbol=%s stage=%s result=%s rc=%d\n", symbol, stage,
           rc == H2_PAL_OK ? "PASS" : "FAIL", rc);
}

static char *save(h2_gizclaw_resp_storage_t *storage, const char *s) {
  size_t size = strlen(s) + 1u;
  assert(storage->used + size <= storage->capacity);
  char *out = (char *)storage->data + storage->used;
  memcpy(out, s, size);
  storage->used += size;
  return out;
}
static void snapshot(h2_gizclaw_resp_storage_t *storage, const struct value *v,
                     h2_gizclaw_contact_t *out) {
  *out = (h2_gizclaw_contact_t){.name = save(storage, v->name),
                                .display_name = save(storage, v->display),
                                .phone_number = save(storage, v->phone)};
}
static int perform(enum method method, const struct value *input,
                   struct value *response) {
  if (method == LIST)
    return H2_PAL_OK;
  if (method != CREATE && !state.exists)
    return H2_PAL_ERR_NOT_FOUND;
  struct value prior = state.remote;
  const bool existed = state.exists;
  if (method == CREATE) {
    assert(!state.exists && state.fixture->contact_created);
    state.remote = *input;
    state.exists = true;
  } else if (method == PUT) {
    state.remote = *input;
  } else if (method == DELETE) {
    state.exists = false;
  }
  *response = state.remote;
  if (method != GET && ++state.changes == state.discard_change) {
    state.remote = prior;
    state.exists = existed;
  }
  return H2_PAL_OK;
}

static void reply(enum method method, const struct value *value,
                  const char *cursor, h2_gizclaw_resp_storage_t *storage,
                  h2_gizclaw_contact_t *contact,
                  h2_gizclaw_contact_page_t *page) {
  assert(storage != NULL && storage->used == 0u);
  memset(storage->data, 0xa5, storage->capacity);
  unsigned fault =
      ++state.replies == state.corrupt_at ? state.corrupt_field : 0u;
  if (method != LIST) {
    snapshot(storage, value, contact);
    switch (fault) {
    case 1:
      contact->name = NULL;
      break;
    case 2:
      contact->name = save(storage, "wrong");
      break;
    case 3:
      contact->display_name = save(storage, "wrong");
      break;
    case 4:
      contact->phone_number = save(storage, "wrong");
      break;
    }
    return;
  }
  *page = (h2_gizclaw_contact_page_t){0};
  page->items = (h2_gizclaw_contact_t *)storage->data;
  storage->used = 2u * sizeof(*page->items);
  memset(page->items, 0, storage->used);
  if (state.pagination && cursor[0] == '\0') {
    struct value unrelated = {"another", "another", "000"};
    page->count = 1u;
    snapshot(storage, &unrelated, page->items);
    page->has_next = true;
    page->next_cursor = save(storage, "next");
  } else {
    if (state.pagination == 1u)
      assert(strcmp(cursor, "next") == 0);
    if (state.exists && state.pagination != 3u) {
      page->count = 1u;
      snapshot(storage, &state.remote, page->items);
    }
    if (state.pagination >= 2u) {
      page->has_next = true;
      page->next_cursor =
          save(storage, state.pagination == 2u || strcmp(cursor, "again") == 0
                            ? "next"
                            : "again");
    }
  }
  switch (fault) {
  case 1:
    page->count = 0u;
    break;
  case 2:
    page->count = 33u;
    break;
  case 3:
    page->count = 1u;
    page->items = NULL;
    break;
  case 4:
    page->count = 1u;
    page->items[0].name = NULL;
    break;
  case 5:
    page->count = 2u;
    page->items[1] = page->items[0];
    break;
  case 6:
    page->has_next = true;
    page->next_cursor = NULL;
    break;
  case 7:
    page->has_next = true;
    page->next_cursor = save(storage, "");
    break;
  }
}

static int create(enum method method, h2_gizclaw_service_t *service,
                  uint64_t id, h2_gizclaw_str_t name, h2_gizclaw_str_t display,
                  h2_gizclaw_str_t phone, h2_gizclaw_str_t cursor, uint32_t ms,
                  h2_gizclaw_req_t **out) {
  assert(service == state.fixture->actors[0].service && !state.alive &&
         ms == 30000u);
  assert(id == state.next_id++);
  ++state.calls[0][method];
  *out = NULL;
  int rc = step();
  if (rc != H2_PAL_OK)
    return rc;
  memset(&state.request, 0, sizeof(state.request));
  state.request.method = method;
  copy(state.request.input.name, sizeof(state.request.input.name), name);
  copy(state.request.input.display, sizeof(state.request.input.display),
       display);
  copy(state.request.input.phone, sizeof(state.request.input.phone), phone);
  copy(state.request.cursor, sizeof(state.request.cursor), cursor);
  state.alive = true;
  ++state.creates;
  *out = &state.request;
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_req_do(h2_gizclaw_req_t *req,
                                  void *user,
                                  h2_gizclaw_req_input_read_fn input_read,
                                  h2_gizclaw_req_output_write_fn output_write,
                                  h2_gizclaw_req_complete_fn on_complete) {
  (void)on_complete;
  assert(state.alive && req == &state.request && !req->submitted &&
         user == NULL && input_read == NULL && output_write == NULL);
  if (req->method == CREATE)
    assert(state.fixture->contact_created);
  int rc = step();
  if (rc == H2_PAL_OK)
    req->submitted = true;
  return rc;
}
h2_pal_result_t h2_gizclaw_req_wait(h2_gizclaw_req_t *req, uint32_t ms) {
  assert(state.alive && req == &state.request && req->submitted &&
         ms == 30000u);
  int rc = step();
  if (rc == H2_PAL_OK)
    rc = perform(req->method, &req->input, &req->response);
  if (rc == H2_PAL_OK)
    req->complete = true;
  return rc;
}
static int parse(enum method method, const h2_gizclaw_req_t *req,
                 h2_gizclaw_resp_storage_t *storage,
                 h2_gizclaw_contact_t *contact,
                 h2_gizclaw_contact_page_t *page) {
  assert(state.alive && req == &state.request && req->complete &&
         method == req->method);
  ++state.calls[1][method];
  int rc = step();
  if (rc == H2_PAL_OK)
    reply(method, &req->response, req->cursor, storage, contact, page);
  return rc;
}
h2_pal_result_t h2_gizclaw_req_cancel(h2_gizclaw_req_t *req) {
  assert(state.alive && req == &state.request);
  return H2_PAL_OK;
}
void h2_gizclaw_req_release(h2_gizclaw_req_t *req) {
  if (req == NULL)
    return;
  assert(state.alive && req == &state.request);
  state.alive = false;
  ++state.releases;
  memset(req, 0xa5, sizeof(*req));
}
static int rpc(enum method method, h2_gizclaw_service_t *service,
               h2_gizclaw_str_t name, h2_gizclaw_str_t display,
               h2_gizclaw_str_t phone, h2_gizclaw_str_t cursor, uint32_t ms,
               h2_gizclaw_resp_storage_t *storage,
               h2_gizclaw_contact_t *contact, h2_gizclaw_contact_page_t *page) {
  assert(service == state.fixture->actors[0].service && !state.alive &&
         ms == 30000u);
  ++state.calls[2][method];
  struct value input = {0}, output = {0};
  char cursor_copy[256];
  copy(input.name, sizeof(input.name), name);
  copy(input.display, sizeof(input.display), display);
  copy(input.phone, sizeof(input.phone), phone);
  copy(cursor_copy, sizeof(cursor_copy), cursor);
  int rc = step();
  if (rc == H2_PAL_OK)
    rc = perform(method, &input, &output);
  if (rc == H2_PAL_OK)
    reply(method, &output, cursor_copy, storage, contact, page);
  return rc;
}

h2_pal_result_t h2_gizclaw_req_create_contact_create(
    h2_gizclaw_service_t *service, uint64_t id, h2_gizclaw_str_t name,
    h2_gizclaw_str_t display, h2_gizclaw_str_t phone, uint32_t ms,
    h2_gizclaw_req_t **out) {

  return create(CREATE, service, id, name, display, phone,
                h2_gizclaw_e2e_str(""), ms, out);
}
h2_pal_result_t
h2_gizclaw_resp_parse_contact_create(const h2_gizclaw_req_t *req,
                                     h2_gizclaw_resp_storage_t *storage,
                                     h2_gizclaw_contact_t *out) {
  return parse(CREATE, req, storage, out, NULL);
}
h2_pal_result_t h2_gizclaw_rpc_contact_create(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t name,
    h2_gizclaw_str_t display, h2_gizclaw_str_t phone, uint32_t ms,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_contact_t *out) {

  return rpc(CREATE, service, name, display, phone, h2_gizclaw_e2e_str(""), ms,
             storage, out, NULL);
}

h2_pal_result_t h2_gizclaw_req_create_contact_get(h2_gizclaw_service_t *service,
                                                  uint64_t id,
                                                  h2_gizclaw_str_t name,
                                                  uint32_t ms,
                                                  h2_gizclaw_req_t **out) {

  return create(GET, service, id, name, h2_gizclaw_e2e_str(""),
                h2_gizclaw_e2e_str(""), h2_gizclaw_e2e_str(""), ms, out);
}
h2_pal_result_t
h2_gizclaw_resp_parse_contact_get(const h2_gizclaw_req_t *req,
                                  h2_gizclaw_resp_storage_t *storage,
                                  h2_gizclaw_contact_t *out) {
  return parse(GET, req, storage, out, NULL);
}
h2_pal_result_t h2_gizclaw_rpc_contact_get(h2_gizclaw_service_t *service,
                                           h2_gizclaw_str_t name, uint32_t ms,
                                           h2_gizclaw_resp_storage_t *storage,
                                           h2_gizclaw_contact_t *out) {

  return rpc(GET, service, name, h2_gizclaw_e2e_str(""), h2_gizclaw_e2e_str(""),
             h2_gizclaw_e2e_str(""), ms, storage, out, NULL);
}

h2_pal_result_t h2_gizclaw_req_create_contact_put(
    h2_gizclaw_service_t *service, uint64_t id, h2_gizclaw_str_t name,
    h2_gizclaw_str_t display, h2_gizclaw_str_t phone, uint32_t ms,
    h2_gizclaw_req_t **out) {

  return create(PUT, service, id, name, display, phone, h2_gizclaw_e2e_str(""),
                ms, out);
}
h2_pal_result_t
h2_gizclaw_resp_parse_contact_put(const h2_gizclaw_req_t *req,
                                  h2_gizclaw_resp_storage_t *storage,
                                  h2_gizclaw_contact_t *out) {
  return parse(PUT, req, storage, out, NULL);
}
h2_pal_result_t h2_gizclaw_rpc_contact_put(h2_gizclaw_service_t *service,
                                           h2_gizclaw_str_t name,
                                           h2_gizclaw_str_t display,
                                           h2_gizclaw_str_t phone, uint32_t ms,
                                           h2_gizclaw_resp_storage_t *storage,
                                           h2_gizclaw_contact_t *out) {

  return rpc(PUT, service, name, display, phone, h2_gizclaw_e2e_str(""), ms,
             storage, out, NULL);
}

h2_pal_result_t
h2_gizclaw_req_create_contact_list(h2_gizclaw_service_t *service, uint64_t id,
                                   h2_gizclaw_str_t cursor, size_t limit,
                                   uint32_t ms, h2_gizclaw_req_t **out) {
  assert(limit == 32u);
  return create(LIST, service, id, h2_gizclaw_e2e_str(""),
                h2_gizclaw_e2e_str(""), h2_gizclaw_e2e_str(""), cursor, ms,
                out);
}
h2_pal_result_t
h2_gizclaw_resp_parse_contact_list(const h2_gizclaw_req_t *req,
                                   h2_gizclaw_resp_storage_t *storage,
                                   h2_gizclaw_contact_page_t *out) {
  return parse(LIST, req, storage, NULL, out);
}
h2_pal_result_t h2_gizclaw_rpc_contact_list(h2_gizclaw_service_t *service,
                                            h2_gizclaw_str_t cursor,
                                            size_t limit, uint32_t ms,
                                            h2_gizclaw_resp_storage_t *storage,
                                            h2_gizclaw_contact_page_t *out) {
  assert(limit == 32u);
  return rpc(LIST, service, h2_gizclaw_e2e_str(""), h2_gizclaw_e2e_str(""),
             h2_gizclaw_e2e_str(""), cursor, ms, storage, NULL, out);
}

h2_pal_result_t
h2_gizclaw_req_create_contact_delete(h2_gizclaw_service_t *service, uint64_t id,
                                     h2_gizclaw_str_t name, uint32_t ms,
                                     h2_gizclaw_req_t **out) {

  return create(DELETE, service, id, name, h2_gizclaw_e2e_str(""),
                h2_gizclaw_e2e_str(""), h2_gizclaw_e2e_str(""), ms, out);
}
h2_pal_result_t
h2_gizclaw_resp_parse_contact_delete(const h2_gizclaw_req_t *req,
                                     h2_gizclaw_resp_storage_t *storage,
                                     h2_gizclaw_contact_t *out) {
  return parse(DELETE, req, storage, out, NULL);
}
h2_pal_result_t h2_gizclaw_rpc_contact_delete(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t name, uint32_t ms,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_contact_t *out) {

  return rpc(DELETE, service, name, h2_gizclaw_e2e_str(""),
             h2_gizclaw_e2e_str(""), h2_gizclaw_e2e_str(""), ms, storage, out,
             NULL);
}

static void reset(h2_gizclaw_e2e_fixture_t *fixture) {
  memset(&state, 0, sizeof(state));
  memset(fixture, 0, sizeof(*fixture));
  state.fixture = fixture;
  state.next_id = 31u;
  fixture->actors[0].service = (void *)(uintptr_t)1u;
  strcpy(fixture->run_prefix, "isolated");
}
static void released(void) {
  assert(!state.alive && state.creates == state.releases);
  if (state.exists)
    assert(state.fixture->contact_created);
}
int main(int argc, char **argv) {
  (void)argv;
  h2_gizclaw_e2e_fixture_t fixture;
  union {
    max_align_t alignment;
    uint8_t bytes[4096];
  } buffer;
  h2_gizclaw_resp_storage_t storage = {buffer.bytes, sizeof(buffer.bytes), 0u};
  reset(&fixture);
  state.emit = argc > 1;
  if (state.emit)
    printf("H2_GIZCLAW_E2E stage=coverage-begin case=rpc\n"
           "H2_GIZCLAW_E2E stage=coverage-begin case=rpc/contact\n");
  assert(h2_gizclaw_e2e_run_contact(&fixture, &storage) == H2_PAL_OK);
  released();
  assert(state.exists && fixture.contact_created);
  assert(strcmp(state.remote.name, "isolated-contact") == 0);
  assert(strcmp(state.remote.display, "owner contact") == 0);
  assert(storage.used == 0u && state.step == 43u && state.replies == 16u &&
         state.changes == 7u && state.deadlines == 16u);
  for (unsigned method = 0u; method < 5u; ++method) {
    assert(state.calls[0][method] > 0u &&
           state.calls[1][method] == state.calls[0][method]);
    assert(state.calls[2][method] > 0u);
  }
  if (state.emit) {
    printf("H2_GIZCLAW_E2E stage=coverage-end case=rpc/contact status=PASS "
           "rc=0 cleanup_rc=0\n"
           "H2_GIZCLAW_E2E stage=coverage-end case=rpc status=PASS rc=0 "
           "cleanup_rc=0\n");
    return 0; // Deliberately no live Desktop summary.
  }
  for (unsigned fail = 1u; fail <= 43u; ++fail) {
    reset(&fixture);
    state.fail_at = fail;
    assert(h2_gizclaw_e2e_run_contact(&fixture, &storage) == H2_PAL_ERR_IO);
    assert(state.step == fail && storage.used == 0u);
    released();
  }
  for (unsigned expiry = 1u; expiry <= 16u; ++expiry) {
    reset(&fixture);
    state.expire_at = expiry;
    assert(h2_gizclaw_e2e_run_contact(&fixture, &storage) ==
           H2_PAL_ERR_TIMEOUT);
    assert(state.deadlines == expiry && storage.used == 0u);
    released();
  }
  for (unsigned update = 1u; update <= 7u; ++update) {
    reset(&fixture);
    state.discard_change = update;
    assert(h2_gizclaw_e2e_run_contact(&fixture, &storage) != H2_PAL_OK);
    released();
  }
  // Creation, reads and updates match all fields; delete needs the identity
  // and a separate absent-in-list assertion, not other snapshot fields.
  const unsigned objects[] = {1, 2, 3, 4, 8, 9, 10, 11, 15, 16};
  for (size_t i = 0u; i < sizeof(objects) / sizeof(objects[0]); ++i) {
    for (unsigned field = 1u; field <= 4u; ++field) {
      reset(&fixture);
      state.corrupt_at = objects[i];
      state.corrupt_field = field;
      assert(h2_gizclaw_e2e_run_contact(&fixture, &storage) ==
             H2_PAL_ERR_INVALID_STATE);
      released();
    }
  }
  for (unsigned which = 0u; which < 2u; ++which) {
    for (unsigned field = 1u; field <= 4u; ++field) {
      reset(&fixture);
      state.corrupt_at = which ? 13u : 6u;
      state.corrupt_field = field;
      assert(h2_gizclaw_e2e_run_contact(&fixture, &storage) ==
             (field <= 2u ? H2_PAL_ERR_INVALID_STATE : H2_PAL_OK));
      released();
    }
  }
  for (unsigned field = 1u; field <= 7u; ++field) {
    for (unsigned which = 0u; which < 2u; ++which) {
      reset(&fixture);
      state.corrupt_at = which ? 12u : 5u;
      state.corrupt_field = field;
      assert(h2_gizclaw_e2e_run_contact(&fixture, &storage) ==
             H2_PAL_ERR_INVALID_STATE);
      released();
    }
  }
  reset(&fixture);
  state.pagination = 1u;
  assert(h2_gizclaw_e2e_run_contact(&fixture, &storage) == H2_PAL_OK);
  released();
  for (unsigned mode = 2u; mode <= 3u; ++mode) {
    reset(&fixture);
    state.pagination = mode;
    assert(h2_gizclaw_e2e_run_contact(&fixture, &storage) ==
           (mode == 2u ? H2_PAL_ERR_INVALID_STATE : H2_PAL_ERR_NO_SPACE));
    if (mode == 3u)
      assert(state.deadlines == 36u); // Four object calls plus 32 pages.
    released();
  }
  reset(&fixture);
  fixture.contact_created = true;
  strcpy(fixture.contact_name, "do-not-overwrite");
  assert(h2_gizclaw_e2e_run_contact(&fixture, &storage) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(strcmp(fixture.contact_name, "do-not-overwrite") == 0 &&
         state.step == 0u);
  reset(&fixture);
  memset(fixture.run_prefix, 'x', sizeof(fixture.run_prefix));
  assert(h2_gizclaw_e2e_run_contact(&fixture, &storage) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(state.step == 0u && !fixture.contact_created);
  assert(h2_gizclaw_e2e_run_contact(NULL, &storage) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_e2e_run_contact(&fixture, NULL) == H2_PAL_ERR_INVALID_ARG);
  return 0;
}
