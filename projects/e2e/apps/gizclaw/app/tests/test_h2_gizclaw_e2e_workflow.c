#include "h2_gizclaw_e2e_workflow.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

/* Test the real E2E case at the public API boundary; no live server here. */
struct h2_gizclaw_req {
  bool get, submitted, complete;
  char name[256];
};

static struct {
  struct h2_gizclaw_req req;
  bool alive, paginated;
  unsigned step, fail_at, replies, fault_at, fault;
  unsigned deadlines, expire_at, creates, releases, cancels;
  unsigned calls[6];
} state;

static int step(void) {
  return ++state.step == state.fail_at ? H2_PAL_ERR_IO : H2_PAL_OK;
}

h2_gizclaw_str_t h2_gizclaw_e2e_str(const char *s) {
  return (h2_gizclaw_str_t){s, strlen(s)};
}
bool h2_gizclaw_e2e_fixture_has_time(const h2_gizclaw_e2e_fixture_t *fixture,
                                     uint32_t ms) {
  assert(fixture != NULL && ms == 30000u);
  return ++state.deadlines != state.expire_at;
}
void h2_gizclaw_e2e_evidence(const char *symbol, const char *stage, int rc) {
  assert(symbol != NULL && stage != NULL);
  assert(rc == H2_PAL_OK || rc == H2_PAL_ERR_IO ||
         rc == H2_PAL_ERR_INVALID_STATE);
}

static char *save(h2_gizclaw_resp_storage_t *storage, const char *s) {
  const size_t len = strlen(s) + 1u;
  assert(storage->used + len <= storage->capacity);
  char *out = (char *)storage->data + storage->used;
  memcpy(out, s, len);
  storage->used += len;
  return out;
}

static unsigned begin_reply(h2_gizclaw_resp_storage_t *storage) {
  assert(storage != NULL && storage->used == 0u && storage->capacity >= 1024u);
  // Invalidate prior list strings. The following get must own its alias copy.
  memset(storage->data, 0xa5, storage->capacity);
  return ++state.replies == state.fault_at ? state.fault : 0u;
}

static void list_reply(h2_gizclaw_resp_storage_t *storage,
                       h2_gizclaw_workflow_page_t *out) {
  const unsigned fault = begin_reply(storage);
  memset(out, 0, sizeof(*out));
  out->items = (h2_gizclaw_workflow_t *)storage->data;
  storage->used = 2u * sizeof(*out->items);
  memset(out->items, 0, storage->used);
  out->count = 2u;
  out->items[0].name = save(storage, "zeta-model");
  out->items[1].name = save(storage, "alpha-model");
  out->items[0].collection = save(storage, "assistants");
  out->items[1].collection = out->items[0].collection;
  out->runtime_profile_name = save(storage, "default");
  out->runtime_profile_revision = save(storage, "revision");
  if (state.paginated) {
    out->has_next = true;
    out->next_cursor = save(storage, "next-page");
  }
  switch (fault) {
  case 1:
    out->runtime_profile_name = NULL;
    break;
  case 2:
    out->runtime_profile_name = save(storage, "other");
    break;
  case 3:
    out->runtime_profile_revision = NULL;
    break;
  case 4:
    out->runtime_profile_revision = save(storage, "");
    break;
  case 5:
    out->count = 0u;
    break;
  case 6:
    out->count = 33u;
    break;
  case 7:
    out->items = NULL;
    break;
  case 8:
    out->items[0].name = NULL;
    break;
  case 9:
    out->items[0].name = save(storage, "");
    break;
  case 10:
    out->items[0].collection = NULL;
    break;
  case 11:
    out->items[0].collection = save(storage, "other");
    break;
  case 12:
    out->has_next = true;
    break;
  case 13:
    out->has_next = true;
    out->next_cursor = save(storage, "");
    break;
  }
}

static void get_reply(h2_gizclaw_resp_storage_t *storage,
                      h2_gizclaw_workflow_get_result_t *out) {
  const unsigned fault = begin_reply(storage);
  memset(out, 0, sizeof(*out));
  out->workflow.name = save(storage, "alpha-model");
  out->workflow.collection = save(storage, "assistants");
  out->runtime_profile_name = save(storage, "default");
  out->runtime_profile_revision = save(storage, "revision");
  switch (fault) {
  case 1:
    out->runtime_profile_name = NULL;
    break;
  case 2:
    out->runtime_profile_name = save(storage, "other");
    break;
  case 3:
    out->runtime_profile_revision = NULL;
    break;
  case 4:
    out->runtime_profile_revision = save(storage, "");
    break;
  case 5:
    out->workflow.name = NULL;
    break;
  case 6:
    out->workflow.name = save(storage, "wrong-model");
    break;
  case 7:
    out->workflow.collection = NULL;
    break;
  case 8:
    out->workflow.collection = save(storage, "other");
    break;
  }
}

static void check_list(h2_gizclaw_str_t collection, h2_gizclaw_str_t cursor,
                       size_t limit) {
  assert(collection.len == strlen("assistants") &&
         memcmp(collection.data, "assistants", collection.len) == 0);
  assert(cursor.len == 0u && limit == 32u);
}
static void check_name(h2_gizclaw_str_t name) {
  assert(name.len == strlen("alpha-model") &&
         memcmp(name.data, "alpha-model", name.len) == 0);
}

h2_pal_result_t h2_gizclaw_rpc_workflow_list(h2_gizclaw_service_t *service,
                                             h2_gizclaw_str_t collection,
                                             h2_gizclaw_str_t cursor,
                                             size_t limit, uint32_t ms,
                                             h2_gizclaw_resp_storage_t *storage,
                                             h2_gizclaw_workflow_page_t *out) {
  assert(service != NULL && ms == 30000u && !state.alive);
  check_list(collection, cursor, limit);
  ++state.calls[0];
  int rc = step();
  if (rc == H2_PAL_OK)
    list_reply(storage, out);
  return rc;
}
h2_pal_result_t h2_gizclaw_rpc_workflow_get(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t name, uint32_t ms,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_workflow_get_result_t *out) {
  assert(service != NULL && ms == 30000u && !state.alive);
  check_name(name);
  ++state.calls[1];
  int rc = step();
  if (rc == H2_PAL_OK)
    get_reply(storage, out);
  return rc;
}

static int create(h2_gizclaw_service_t *service, uint64_t id, bool get,
                  uint32_t ms, h2_gizclaw_req_t **out) {
  assert(service != NULL && ms == 30000u && !state.alive);
  assert(id == (get ? 22u : 21u));
  ++state.calls[get ? 3 : 2];
  *out = NULL;
  int rc = step();
  if (rc != H2_PAL_OK)
    return rc;
  memset(&state.req, 0, sizeof(state.req));
  state.req.get = get;
  state.alive = true;
  ++state.creates;
  *out = &state.req;
  return rc;
}
h2_pal_result_t
h2_gizclaw_req_create_workflow_list(h2_gizclaw_service_t *service, uint64_t id,
                                    h2_gizclaw_str_t collection,
                                    h2_gizclaw_str_t cursor, size_t limit,
                                    uint32_t ms, h2_gizclaw_req_t **out) {
  check_list(collection, cursor, limit);
  return create(service, id, false, ms, out);
}
h2_pal_result_t
h2_gizclaw_req_create_workflow_get(h2_gizclaw_service_t *service, uint64_t id,
                                   h2_gizclaw_str_t name, uint32_t ms,
                                   h2_gizclaw_req_t **out) {
  check_name(name);
  int rc = create(service, id, true, ms, out);
  if (rc == H2_PAL_OK) {
    memcpy(state.req.name, name.data, name.len);
    state.req.name[name.len] = '\0';
  }
  return rc;
}
h2_pal_result_t h2_gizclaw_req_do(h2_gizclaw_req_t *request,
                                  void *user,
                                  h2_gizclaw_req_input_read_fn input_read,
                                  h2_gizclaw_req_output_write_fn output_write,
                                  h2_gizclaw_req_complete_fn on_complete) {
  (void)on_complete;
  assert(state.alive && request == &state.req && !request->submitted);
  assert(user == NULL && input_read == NULL && output_write == NULL);
  int rc = step();
  if (rc == H2_PAL_OK)
    request->submitted = true;
  return rc;
}
h2_pal_result_t h2_gizclaw_req_wait(h2_gizclaw_req_t *request, uint32_t ms) {
  assert(state.alive && request == &state.req && request->submitted &&
         ms == 30000u);
  int rc = step();
  if (rc == H2_PAL_OK)
    request->complete = true;
  return rc;
}
h2_pal_result_t
h2_gizclaw_resp_parse_workflow_list(const h2_gizclaw_req_t *request,
                                    h2_gizclaw_resp_storage_t *storage,
                                    h2_gizclaw_workflow_page_t *out) {
  assert(state.alive && request == &state.req && request->complete &&
         !request->get);
  ++state.calls[4];
  int rc = step();
  if (rc == H2_PAL_OK)
    list_reply(storage, out);
  return rc;
}
h2_pal_result_t
h2_gizclaw_resp_parse_workflow_get(const h2_gizclaw_req_t *request,
                                   h2_gizclaw_resp_storage_t *storage,
                                   h2_gizclaw_workflow_get_result_t *out) {
  assert(state.alive && request == &state.req && request->complete &&
         request->get);
  assert(strcmp(request->name, "alpha-model") == 0);
  ++state.calls[5];
  int rc = step();
  if (rc == H2_PAL_OK)
    get_reply(storage, out);
  return rc;
}
h2_pal_result_t h2_gizclaw_req_cancel(h2_gizclaw_req_t *request) {
  assert(state.alive && request == &state.req);
  ++state.cancels;
  return H2_PAL_OK;
}
void h2_gizclaw_req_release(h2_gizclaw_req_t *request) {
  if (request == NULL)
    return;
  assert(state.alive && request == &state.req);
  ++state.releases;
  state.alive = false;
  memset(request, 0xa5, sizeof(*request));
}

int main(void) {
  h2_gizclaw_e2e_fixture_t fixture = {0};
  fixture.actors[0].service = (void *)(uintptr_t)1u;
  strcpy(fixture.runtime_profile_name, "default");
  union {
    max_align_t alignment;
    uint8_t bytes[1024];
  } buffer;
  h2_gizclaw_resp_storage_t storage = {buffer.bytes, sizeof(buffer.bytes), 0u};
  assert(h2_gizclaw_e2e_run_workflow(&fixture, &storage) == H2_PAL_OK);
  assert(strcmp(fixture.workflow_name, "alpha-model") == 0);
  assert(state.replies == 4u && state.step == 10u && state.deadlines == 4u);
  assert(state.creates == 2u && state.releases == 2u && state.cancels == 0u);
  for (unsigned i = 0u; i < 6u; ++i)
    assert(state.calls[i] == 1u);
  assert(storage.used == 0u && !state.alive);
  memset(&state, 0, sizeof(state));
  state.paginated = true;
  assert(h2_gizclaw_e2e_run_workflow(&fixture, &storage) == H2_PAL_OK);
  assert(strcmp(fixture.workflow_name, "alpha-model") == 0);
  for (unsigned i = 1u; i <= 10u; ++i) {
    memset(&state, 0, sizeof(state));
    state.fail_at = i;
    assert(h2_gizclaw_e2e_run_workflow(&fixture, &storage) == H2_PAL_ERR_IO);
    assert(state.step == i && !state.alive && state.creates == state.releases);
    assert(storage.used == 0u && fixture.workflow_name[0] == '\0');
  }
  for (unsigned reply = 1u; reply <= 4u; ++reply) {
    for (unsigned fault = 1u; fault <= (reply % 2u ? 13u : 8u); ++fault) {
      memset(&state, 0, sizeof(state));
      state.fault_at = reply;
      state.fault = fault;
      assert(h2_gizclaw_e2e_run_workflow(&fixture, &storage) ==
             H2_PAL_ERR_INVALID_STATE);
      assert(state.replies == reply && !state.alive &&
             state.creates == state.releases);
      assert(storage.used == 0u && fixture.workflow_name[0] == '\0');
    }
  }
  for (unsigned i = 1u; i <= 4u; ++i) {
    memset(&state, 0, sizeof(state));
    state.expire_at = i;
    assert(h2_gizclaw_e2e_run_workflow(&fixture, &storage) ==
           H2_PAL_ERR_TIMEOUT);
    assert(state.deadlines == i && !state.alive);
    assert(storage.used == 0u && fixture.workflow_name[0] == '\0');
  }
  memset(&state, 0, sizeof(state));
  memset(fixture.runtime_profile_name, 'x',
         sizeof(fixture.runtime_profile_name));
  assert(h2_gizclaw_e2e_run_workflow(&fixture, &storage) ==
         H2_PAL_ERR_INVALID_ARG);
  fixture.runtime_profile_name[0] = '\0';
  assert(h2_gizclaw_e2e_run_workflow(&fixture, &storage) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_e2e_run_workflow(NULL, &storage) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_e2e_run_workflow(&fixture, NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(state.step == 0u);

  // Selection remains deterministic and never returns a truncated alias.
  h2_gizclaw_workflow_t items[] = {
      {.name = "zeta"}, {.name = "alpha"}, {.name = ""}, {.name = NULL}};
  h2_gizclaw_workflow_page_t page = {.items = items, .count = 4u};
  char selected[6];
  assert(h2_gizclaw_e2e_select_workflow_name(&page, selected,
                                             sizeof(selected)) == H2_PAL_OK);
  assert(strcmp(selected, "alpha") == 0);
  assert(h2_gizclaw_e2e_select_workflow_name(&page, selected, 5u) ==
         H2_PAL_ERR_TRUNCATED);
  assert(selected[0] == '\0');
  page.count = 0u;
  assert(h2_gizclaw_e2e_select_workflow_name(
             &page, selected, sizeof(selected)) == H2_PAL_ERR_NOT_FOUND);
  page.count = 1u;
  page.items = NULL;
  assert(h2_gizclaw_e2e_select_workflow_name(
             &page, selected, sizeof(selected)) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_e2e_select_workflow_name(
             NULL, selected, sizeof(selected)) == H2_PAL_ERR_INVALID_ARG);
  return 0;
}
