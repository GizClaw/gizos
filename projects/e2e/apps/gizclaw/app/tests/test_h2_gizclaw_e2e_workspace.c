#include "h2_gizclaw_e2e_workspace.h"
// Keep test operations and assertions enabled in optimized builds.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum method { LIST, GET, CREATE, INPUT, DELETE, ACTIVATE, HISTORY, RELOAD };
static unsigned s_runs;
struct h2_gizclaw_req {
  enum method method;
  char name[256], cursor[256];
  bool started, waited;
};
static struct {
  h2_gizclaw_e2e_fixture_t *fixture;
  unsigned stage, fail_stage, budget, fail_budget, live, replies, corrupt_reply,
      mode;
  unsigned creates, deletes, discard_create, discard_delete;
  unsigned calls[3][8];
  bool exists[H2_GIZCLAW_E2E_ACTOR_COUNT], emit,
      selected[H2_GIZCLAW_E2E_ACTOR_COUNT];
  unsigned pagination;
  char remote[H2_GIZCLAW_E2E_ACTOR_COUNT][256];
  size_t role;
  uint64_t last_id;
} state;
static unsigned service_tokens[H2_GIZCLAW_E2E_ACTOR_COUNT];
static void assert_collection(h2_gizclaw_str_t value) {
  assert(value.len == 10u && memcmp(value.data, "assistants", 10u) == 0);
}
static int step(void) {
  return ++state.stage == state.fail_stage ? H2_PAL_ERR_IO : H2_PAL_OK;
}
h2_gizclaw_str_t h2_gizclaw_e2e_str(const char *s) {
  return (h2_gizclaw_str_t){s, strlen(s)};
}
void h2_gizclaw_e2e_evidence(const char *symbol, const char *stage, int rc) {
  if (state.emit)
    printf("H2_GIZCLAW_E2E symbol=%s stage=%s result=%s rc=%d\n", symbol, stage,
           rc ? "FAIL" : "PASS", rc);
}
bool h2_gizclaw_e2e_fixture_has_time(const h2_gizclaw_e2e_fixture_t *f,
                                     uint32_t ms) {
  assert(f == state.fixture && ms == 30000u);
  return ++state.budget != state.fail_budget;
}
static void copy(char *out, h2_gizclaw_str_t in) {
  assert(in.len < 256u && in.data != NULL);
  memcpy(out, in.data, in.len);
  out[in.len] = '\0';
}
static void service(h2_gizclaw_service_t *s, uint32_t timeout) {
  assert(timeout == 30000u);
  for (size_t role = 0u; role < H2_GIZCLAW_E2E_ACTOR_COUNT; ++role) {
    if (s == (h2_gizclaw_service_t *)&service_tokens[role]) {
      state.role = role;
      return;
    }
  }
  assert(false);
}
static int create_req(enum method method, h2_gizclaw_service_t *s, uint64_t id,
                      h2_gizclaw_str_t name, h2_gizclaw_str_t cursor,
                      uint32_t timeout, h2_gizclaw_req_t **out) {
  service(s, timeout);
  assert(id > state.last_id);
  state.last_id = id;
  ++state.calls[0][method];
  *out = NULL;
  int rc = step();
  if (rc)
    return rc;
  *out = calloc(1, sizeof(**out));
  assert(*out != NULL);
  (*out)->method = method;
  copy((*out)->name, name);
  copy((*out)->cursor, cursor);
  ++state.live;
  return H2_PAL_OK;
}
static int perform(enum method method, const char *name) {
  if (method == LIST)
    return H2_PAL_OK;
  assert(strcmp(name, state.fixture->workspace_name) == 0);
  if (method == CREATE) {
    assert(!state.exists[state.role] && state.fixture->workspace_created);
    state.selected[state.role] = false;
    strcpy(state.remote[state.role], name);
    state.exists[state.role] = ++state.creates != state.discard_create;
    return H2_PAL_OK;
  }
  if (!state.exists[state.role])
    return H2_PAL_ERR_NOT_FOUND;
  assert(strcmp(name, state.remote[state.role]) == 0);
  if (method == ACTIVATE)
    state.selected[state.role] = true;
  if (method == RELOAD)
    assert(state.selected[state.role]);
  if (method == DELETE)
    state.exists[state.role] = ++state.deletes == state.discard_delete;
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_req_do(h2_gizclaw_req_t *r, void *user,
                                  h2_gizclaw_req_input_read_fn input_read,
                                  h2_gizclaw_req_output_write_fn output_write,
                                  h2_gizclaw_req_complete_fn on_complete) {
  (void)on_complete;
  assert(r && !r->started && !user && !input_read && !output_write);
  if (r->method == CREATE)
    assert(state.fixture->workspace_created);
  int rc = step();
  if (!rc) {
    r->started = true;
    rc = perform(r->method, r->name);
  }
  return rc;
}
h2_pal_result_t h2_gizclaw_req_wait(h2_gizclaw_req_t *r, uint32_t timeout) {
  assert(r->started && timeout == 30000u);
  int rc = step();
  r->waited = !rc;
  return rc;
}
h2_pal_result_t h2_gizclaw_req_cancel(h2_gizclaw_req_t *r) {
  assert(r);
  return H2_PAL_OK;
}
void h2_gizclaw_req_release(h2_gizclaw_req_t *r) {
  if (r) {
    assert(state.live);
    --state.live;
    memset(r, 0xa5, sizeof(*r));
    free(r);
  }
}
static void *alloc(h2_gizclaw_resp_storage_t *s, size_t n, size_t alignment) {
  uintptr_t p = (uintptr_t)s->data + s->used;
  size_t padding = (alignment - p % alignment) % alignment;
  assert(s->used + padding + n <= s->capacity);
  void *out = (char *)s->data + s->used + padding;
  s->used += padding + n;
  memset(out, 0, n);
  return out;
}
static char *save(h2_gizclaw_resp_storage_t *s, const char *text) {
  char *out = alloc(s, strlen(text) + 1u, 1u);
  strcpy(out, text);
  return out;
}
static void object(h2_gizclaw_resp_storage_t *s, h2_gizclaw_workspace_t *out,
                   const char *name) {
  *out = (h2_gizclaw_workspace_t){.name = save(s, name),
                                  .workflow_name = save(s, "chosen"),
                                  .available = true};
}
static void reply(enum method method, const char *name, const char *cursor,
                  h2_gizclaw_resp_storage_t *s, void *out) {
  assert(s->used == 0u);
  memset(s->data, 0xa5, s->capacity);
  unsigned fault = ++state.replies == state.corrupt_reply ? state.mode : 0u;
  h2_gizclaw_workspace_t *w = NULL;
  if (method == LIST) {
    h2_gizclaw_workspace_page_t *p = out;
    memset(p, 0, sizeof(*p));
    p->items =
        alloc(s, 2u * sizeof(*p->items), _Alignof(h2_gizclaw_workspace_t));
    if (state.pagination && !cursor[0]) {
      p->count = 1;
      object(s, &p->items[0], "unrelated");
      p->has_next = true;
      p->next_cursor = save(s, "next");
    } else if (state.exists[state.role]) {
      p->count = 1;
      object(s, &p->items[0], state.remote[state.role]);
    }
    p->runtime_profile_name = save(s, "default");
    p->runtime_profile_revision = save(s, "rev");
    if (state.pagination == 2u || state.pagination == 3u) {
      p->count = 0u;
      p->has_next = true;
      p->next_cursor = save(s, state.pagination == 2u     ? "loop"
                               : strcmp(cursor, "a") == 0 ? "b"
                                                          : "a");
    }
    if (fault == 3)
      p->runtime_profile_name = save(s, "wrong");
    if (fault == 4)
      p->runtime_profile_revision = NULL;
    if (fault == 7)
      p->count = 33;
    if (fault == 8) {
      p->count = 1;
      p->items = NULL;
    }
    if (fault == 9) {
      p->count = 1;
      p->items = (void *)((char *)s->data + 1);
    }
    if (fault == 10) {
      p->count = 2;
      object(s, &p->items[0], state.remote[state.role]);
      object(s, &p->items[1], state.remote[state.role]);
    }
    if (fault == 11) {
      p->has_next = true;
      p->next_cursor = NULL;
    }
    if (p->count && (fault < 7u || fault > 11u))
      w = p->items;
  } else if (method == HISTORY) {
    h2_gizclaw_workspace_history_page_t *p = out;
    memset(p, 0, sizeof(*p));
    p->available = true;
    p->items = alloc(s, 2u * sizeof(*p->items),
                     _Alignof(h2_gizclaw_workspace_history_entry_t));
    if (state.pagination) {
      p->count = 1;
      p->items[0].id = save(s, cursor[0] ? "second" : "first");
      p->items[0].type = (h2_gizclaw_workspace_history_type_t)127;
      if (!cursor[0]) {
        p->has_next = true;
        p->next_cursor = save(s, "next");
      }
    }
    if (state.pagination == 4u || state.pagination == 5u) {
      p->count = 0u;
      p->has_next = true;
      p->next_cursor = save(s, state.pagination == 4u     ? "loop"
                               : strcmp(cursor, "a") == 0 ? "b"
                                                          : "a");
    }
    if (fault == 12)
      p->available = false;
    if (fault == 7)
      p->count = 33;
    if (fault == 8) {
      p->count = 1;
      p->items = NULL;
    }
    if (fault == 9) {
      p->count = 1;
      p->items = (void *)((char *)s->data + 1);
    }
    if (fault == 10) {
      p->count = 2;
      p->items[0].id = save(s, "same");
      p->items[1].id = save(s, "same");
    }
    if (fault == 11) {
      p->has_next = true;
      p->next_cursor = NULL;
    }
    if (fault == 5) {
      p->count = 1;
      p->items[0].id = "external";
    }
    if (fault == 6) {
      p->count = 1;
      p->items[0].id = alloc(s, 1u, 1u);
      *p->items[0].id = 'x';
    }
  } else if (method == ACTIVATE || method == RELOAD) {
    h2_gizclaw_workspace_activation_t *a = out;
    *a = (h2_gizclaw_workspace_activation_t){
        .workspace_name = save(s, name),
        .active_workspace_name = save(s, name),
        .workflow_name = save(s, "chosen"),
        .runtime_state = method == ACTIVATE
                             ? H2_GIZCLAW_WORKSPACE_RUNTIME_STARTING
                             : H2_GIZCLAW_WORKSPACE_RUNTIME_RUNNING};
    if (fault == 1)
      a->active_workspace_name = save(s, "wrong");
    if (fault == 2)
      a->workflow_name = save(s, "wrong");
    if (fault == 5)
      a->workspace_name = "external";
    if (fault == 6) {
      a->workspace_name = alloc(s, 1u, 1u);
      *a->workspace_name = 'x';
    }
    if (fault == 13)
      a->runtime_state = H2_GIZCLAW_WORKSPACE_RUNTIME_STARTING;
    if (fault == 14)
      a->pending_workspace_name = save(s, "pending");
    if (fault == 18)
      a->workflow_name = NULL;
  } else if (method == GET) {
    h2_gizclaw_workspace_get_result_t *g = out;
    memset(g, 0, sizeof(*g));
    w = &g->workspace;
    object(s, w, name);
    g->runtime_profile_name = save(s, "default");
    g->runtime_profile_revision = save(s, "rev");
    if (fault == 3)
      g->runtime_profile_name = save(s, "wrong");
    if (fault == 4)
      g->runtime_profile_revision = NULL;
  } else {
    w = out;
    object(s, w, name);
    if (method == CREATE)
      w->collection = save(s, "assistants");
  }
  if (w) {
    if (fault == 1)
      w->name = save(s, "wrong");
    if (fault == 2)
      w->workflow_name = save(s, "wrong");
    if (fault == 5)
      w->name = "external";
    if (fault == 6) {
      w->name = alloc(s, 1u, 1u);
      *w->name = 'x';
    }
    if (fault == 15)
      w->system = true;
    if (fault == 16)
      w->available = false;
  }
  if (fault == 17)
    s->used = s->capacity + 1u;
}
static int parse(enum method method, const h2_gizclaw_req_t *r,
                 h2_gizclaw_resp_storage_t *s, void *out) {
  assert(r->method == method && r->waited);
  ++state.calls[1][method];
  int rc = step();
  if (!rc)
    reply(method, r->name, r->cursor, s, out);
  return rc;
}
static int rpc(enum method method, h2_gizclaw_service_t *svc,
               h2_gizclaw_str_t name, h2_gizclaw_str_t cursor, uint32_t timeout,
               h2_gizclaw_resp_storage_t *s, void *out) {
  service(svc, timeout);
  ++state.calls[2][method];
  int rc = step();
  char n[256], c[256];
  copy(n, name);
  copy(c, cursor);
  if (!rc)
    rc = perform(method, n);
  if (!rc)
    reply(method, n, c, s, out);
  return rc;
}

h2_pal_result_t
h2_gizclaw_req_create_workspace_list(h2_gizclaw_service_t *s, uint64_t id,
                                     h2_gizclaw_str_t collection,
                                     h2_gizclaw_str_t cursor, size_t limit,
                                     uint32_t timeout, h2_gizclaw_req_t **out) {
  assert_collection(collection);
  assert(limit == 32u);
  return create_req(LIST, s, id, h2_gizclaw_e2e_str(""), cursor, timeout, out);
}
h2_pal_result_t
h2_gizclaw_resp_parse_workspace_list(const h2_gizclaw_req_t *r,
                                     h2_gizclaw_resp_storage_t *s,
                                     h2_gizclaw_workspace_page_t *out) {
  return parse(LIST, r, s, out);
}
h2_pal_result_t h2_gizclaw_rpc_workspace_list(
    h2_gizclaw_service_t *s, h2_gizclaw_str_t collection,
    h2_gizclaw_str_t cursor, size_t limit, uint32_t timeout,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_workspace_page_t *out) {
  assert_collection(collection);
  assert(limit == 32u);
  return rpc(LIST, s, h2_gizclaw_e2e_str(""), cursor, timeout, storage, out);
}

h2_pal_result_t h2_gizclaw_req_create_workspace_get(h2_gizclaw_service_t *s,
                                                    uint64_t id,
                                                    h2_gizclaw_str_t name,
                                                    uint32_t timeout,
                                                    h2_gizclaw_req_t **out) {

  return create_req(GET, s, id, name, h2_gizclaw_e2e_str(""), timeout, out);
}
h2_pal_result_t
h2_gizclaw_resp_parse_workspace_get(const h2_gizclaw_req_t *r,
                                    h2_gizclaw_resp_storage_t *s,
                                    h2_gizclaw_workspace_get_result_t *out) {
  return parse(GET, r, s, out);
}
h2_pal_result_t
h2_gizclaw_rpc_workspace_get(h2_gizclaw_service_t *s, h2_gizclaw_str_t name,
                             uint32_t timeout,
                             h2_gizclaw_resp_storage_t *storage,
                             h2_gizclaw_workspace_get_result_t *out) {
  /* The production case uses an uninstrumented synchronous get only to wait
   * for an already-acknowledged asynchronous delete. Preserve the existing
   * fault-injection stage numbering for that observation. */
  service(s, timeout);
  if (!state.exists[state.role]) {
    assert(storage->used == 0u);
    return H2_PAL_ERR_NOT_FOUND;
  }
  return rpc(GET, s, name, h2_gizclaw_e2e_str(""), timeout, storage, out);
}

h2_pal_result_t h2_gizclaw_req_create_workspace_create(
    h2_gizclaw_service_t *s, uint64_t id, h2_gizclaw_str_t collection,
    h2_gizclaw_str_t workflow, h2_gizclaw_str_t name, uint32_t timeout,
    h2_gizclaw_req_t **out) {
  assert_collection(collection);
  assert(workflow.len == 6u && memcmp(workflow.data, "chosen", 6u) == 0);
  return create_req(CREATE, s, id, name, h2_gizclaw_e2e_str(""), timeout, out);
}
h2_pal_result_t
h2_gizclaw_resp_parse_workspace_create(const h2_gizclaw_req_t *r,
                                       h2_gizclaw_resp_storage_t *s,
                                       h2_gizclaw_workspace_t *out) {
  return parse(CREATE, r, s, out);
}
h2_pal_result_t h2_gizclaw_rpc_workspace_create(
    h2_gizclaw_service_t *s, h2_gizclaw_str_t collection,
    h2_gizclaw_str_t workflow, h2_gizclaw_str_t name, uint32_t timeout,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_workspace_t *out) {
  assert_collection(collection);
  assert(workflow.len == 6u && memcmp(workflow.data, "chosen", 6u) == 0);
  return rpc(CREATE, s, name, h2_gizclaw_e2e_str(""), timeout, storage, out);
}

h2_pal_result_t h2_gizclaw_req_create_workspace_set_input(
    h2_gizclaw_service_t *s, uint64_t id, h2_gizclaw_str_t name,
    h2_gizclaw_workspace_input_mode_t mode, uint32_t timeout,
    h2_gizclaw_req_t **out) {
  assert(mode == H2_GIZCLAW_WORKSPACE_INPUT_PUSH_TO_TALK);
  return create_req(INPUT, s, id, name, h2_gizclaw_e2e_str(""), timeout, out);
}
h2_pal_result_t
h2_gizclaw_resp_parse_workspace_set_input(const h2_gizclaw_req_t *r,
                                          h2_gizclaw_resp_storage_t *s,
                                          h2_gizclaw_workspace_t *out) {
  return parse(INPUT, r, s, out);
}
h2_pal_result_t h2_gizclaw_rpc_workspace_set_input(
    h2_gizclaw_service_t *s, h2_gizclaw_str_t name,
    h2_gizclaw_workspace_input_mode_t mode, uint32_t timeout,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_workspace_t *out) {
  assert(mode == H2_GIZCLAW_WORKSPACE_INPUT_PUSH_TO_TALK);
  return rpc(INPUT, s, name, h2_gizclaw_e2e_str(""), timeout, storage, out);
}

h2_pal_result_t h2_gizclaw_req_create_workspace_delete(h2_gizclaw_service_t *s,
                                                       uint64_t id,
                                                       h2_gizclaw_str_t name,
                                                       uint32_t timeout,
                                                       h2_gizclaw_req_t **out) {

  return create_req(DELETE, s, id, name, h2_gizclaw_e2e_str(""), timeout, out);
}
h2_pal_result_t
h2_gizclaw_resp_parse_workspace_delete(const h2_gizclaw_req_t *r,
                                       h2_gizclaw_resp_storage_t *s,
                                       h2_gizclaw_workspace_t *out) {
  return parse(DELETE, r, s, out);
}
h2_pal_result_t h2_gizclaw_rpc_workspace_delete(
    h2_gizclaw_service_t *s, h2_gizclaw_str_t name, uint32_t timeout,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_workspace_t *out) {

  return rpc(DELETE, s, name, h2_gizclaw_e2e_str(""), timeout, storage, out);
}

h2_pal_result_t h2_gizclaw_req_create_workspace_activate(
    h2_gizclaw_service_t *s, uint64_t id, h2_gizclaw_str_t name,
    uint32_t timeout, h2_gizclaw_req_t **out) {

  return create_req(ACTIVATE, s, id, name, h2_gizclaw_e2e_str(""), timeout,
                    out);
}
h2_pal_result_t h2_gizclaw_resp_parse_workspace_activate(
    const h2_gizclaw_req_t *r, h2_gizclaw_resp_storage_t *s,
    h2_gizclaw_workspace_activation_t *out) {
  return parse(ACTIVATE, r, s, out);
}
h2_pal_result_t
h2_gizclaw_rpc_workspace_activate(h2_gizclaw_service_t *s,
                                  h2_gizclaw_str_t name, uint32_t timeout,
                                  h2_gizclaw_resp_storage_t *storage,
                                  h2_gizclaw_workspace_activation_t *out) {

  return rpc(ACTIVATE, s, name, h2_gizclaw_e2e_str(""), timeout, storage, out);
}

h2_pal_result_t h2_gizclaw_req_create_workspace_reload(h2_gizclaw_service_t *s,
                                                       uint64_t id,
                                                       uint32_t timeout,
                                                       h2_gizclaw_req_t **out) {
  return create_req(RELOAD, s, id,
                    h2_gizclaw_e2e_str(state.fixture->workspace_name),
                    h2_gizclaw_e2e_str(""), timeout, out);
}
h2_pal_result_t
h2_gizclaw_resp_parse_workspace_reload(const h2_gizclaw_req_t *r,
                                       h2_gizclaw_resp_storage_t *s,
                                       h2_gizclaw_workspace_activation_t *out) {
  return parse(RELOAD, r, s, out);
}
h2_pal_result_t
h2_gizclaw_rpc_workspace_reload(h2_gizclaw_service_t *s, uint32_t timeout,
                                h2_gizclaw_resp_storage_t *storage,
                                h2_gizclaw_workspace_activation_t *out) {
  return rpc(RELOAD, s, h2_gizclaw_e2e_str(state.fixture->workspace_name),
             h2_gizclaw_e2e_str(""), timeout, storage, out);
}

h2_pal_result_t h2_gizclaw_req_create_workspace_history_list(
    h2_gizclaw_service_t *s, uint64_t id, h2_gizclaw_str_t name,
    h2_gizclaw_str_t cursor, size_t limit,
    h2_gizclaw_workspace_history_order_t order, uint32_t timeout,
    h2_gizclaw_req_t **out) {
  assert(limit == 32u && order == H2_GIZCLAW_WORKSPACE_HISTORY_ORDER_DESC);
  return create_req(HISTORY, s, id, name, cursor, timeout, out);
}
h2_pal_result_t h2_gizclaw_resp_parse_workspace_history_list(
    const h2_gizclaw_req_t *r, h2_gizclaw_resp_storage_t *s,
    h2_gizclaw_workspace_history_page_t *out) {
  return parse(HISTORY, r, s, out);
}
h2_pal_result_t h2_gizclaw_rpc_workspace_history_list(
    h2_gizclaw_service_t *s, h2_gizclaw_str_t name, h2_gizclaw_str_t cursor,
    size_t limit, h2_gizclaw_workspace_history_order_t order, uint32_t timeout,
    h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_workspace_history_page_t *out) {
  assert(limit == 32u && order == H2_GIZCLAW_WORKSPACE_HISTORY_ORDER_DESC);
  return rpc(HISTORY, s, name, cursor, timeout, storage, out);
}

static int run(unsigned fail, unsigned budget, unsigned response, unsigned mode,
               unsigned discard_create, unsigned discard_delete,
               unsigned pagination, bool exercise) {
  assert(!state.live);
  ++s_runs;
  bool emit = state.emit;
  memset(&state, 0, sizeof(state));
  state.emit = emit;
  state.fail_stage = fail;
  state.fail_budget = budget;
  state.corrupt_reply = response;
  state.mode = mode;
  state.discard_create = discard_create;
  state.discard_delete = discard_delete;
  state.pagination = pagination;
  h2_gizclaw_e2e_fixture_t *f = calloc(1, sizeof(*f));
  assert(f);
  state.fixture = f;
  for (size_t role = 0u; role < H2_GIZCLAW_E2E_ACTOR_COUNT; ++role)
    f->actors[role].service = (h2_gizclaw_service_t *)&service_tokens[role];
  strcpy(f->workspace_name, "isolated");
  strcpy(f->workflow_name, "chosen");
  strcpy(f->runtime_profile_name, "default");
  _Alignas(max_align_t) unsigned char data[8192];
  h2_gizclaw_resp_storage_t s = {.data = data, .capacity = sizeof(data)};
  if (emit)
    printf("H2_GIZCLAW_E2E stage=coverage-begin case=rpc\nH2_GIZCLAW_E2E "
           "stage=coverage-begin case=rpc/catalog-workspace\n");
  int rc = h2_gizclaw_e2e_run_workspace(f, &s, exercise);
  assert(!state.live);
  assert(!f->workspace_delete_acknowledged);
  assert(!f->isolation_workspace_delete_acknowledged);
  if (state.exists[H2_GIZCLAW_E2E_OWNER])
    assert(f->workspace_created &&
           !strcmp(state.remote[H2_GIZCLAW_E2E_OWNER], f->workspace_name));
  if (!rc) {
    assert(state.exists[H2_GIZCLAW_E2E_OWNER] && f->workspace_created &&
           !strcmp(f->workspace_name, "isolated"));
    assert(state.creates == (exercise ? 3u : 1u) &&
           state.deletes == (exercise ? 2u : 0u));
    if (exercise)
      for (unsigned api = 0; api < 3; ++api)
        for (unsigned method = 0; method < 8; ++method)
          assert(state.calls[api][method] > 0);
    if (exercise && !pagination)
      assert(state.stage == 53u && state.budget == 26u && state.replies == 26u);
  }
  if (emit)
    printf("H2_GIZCLAW_E2E stage=coverage-end case=rpc/catalog-workspace "
           "status=%s rc=%d cleanup_rc=0\n"
           "H2_GIZCLAW_E2E stage=coverage-end case=rpc status=%s rc=%d "
           "cleanup_rc=0\n",
           rc ? "FAIL" : "PASS", rc, rc ? "FAIL" : "PASS", rc);
  free(f);
  return rc;
}
static void test_name_boundaries(void) {
  const size_t cap = sizeof(((h2_gizclaw_e2e_fixture_t *)0)->workspace_name);
  const size_t limit = cap - sizeof("-req");
  const size_t lengths[] = {0u, limit, limit + 1u, cap - 1u, cap};
  for (unsigned exercise = 0; exercise < 2u; ++exercise) {
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
      memset(&state, 0, sizeof(state));
      h2_gizclaw_e2e_fixture_t *f = calloc(1, sizeof(*f));
      assert(f != NULL);
      state.fixture = f;
      for (size_t role = 0u; role < H2_GIZCLAW_E2E_ACTOR_COUNT; ++role)
        f->actors[role].service = (void *)&service_tokens[role];
      memset(f->workspace_name, 'n', lengths[i]);
      strcpy(f->workflow_name, "chosen");
      strcpy(f->runtime_profile_name, "default");
      char original[sizeof(f->workspace_name)];
      memcpy(original, f->workspace_name, sizeof(original));
      _Alignas(max_align_t) unsigned char data[8192];
      h2_gizclaw_resp_storage_t storage = {.data = data,
                                           .capacity = sizeof(data)};
      int rc = h2_gizclaw_e2e_run_workspace(f, &storage, exercise != 0u);
      assert(!state.live);
      assert(memcmp(f->workspace_name, original, sizeof(original)) == 0);
      if (!lengths[i] || lengths[i] == cap) {
        assert(rc == H2_PAL_ERR_INVALID_ARG);
      } else if (exercise && lengths[i] > limit) {
        assert(rc == H2_PAL_ERR_TRUNCATED);
      } else {
        assert(rc == H2_PAL_OK && state.exists[H2_GIZCLAW_E2E_OWNER] &&
               f->workspace_created);
        assert(!strcmp(state.remote[H2_GIZCLAW_E2E_OWNER], original) &&
               storage.used == 0u);
        assert(state.creates == (exercise ? 3u : 1u));
        assert(state.deletes == (exercise ? 2u : 0u));
      }
      if (rc != H2_PAL_OK)
        assert(!state.stage && !state.budget && !f->workspace_created);
      free(f);
    }
  }
}

int main(int argc, char **argv) {
  if (argc == 4 && !strcmp(argv[1], "--emit-failure-evidence")) {
    unsigned failure = (unsigned)atoi(argv[2]),
             budget = (unsigned)atoi(argv[3]);
    assert(failure <= 53u && budget <= 26u && (failure || budget));
    state.emit = true;
    assert(run(failure, budget, 0, 0, 0, 0, 0, true) != H2_PAL_OK);
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "--emit-success-evidence")) {
    state.emit = true;
    assert(run(0, 0, 0, 0, 0, 0, false, true) == H2_PAL_OK);
    return 0;
  }
  assert(run(0, 0, 0, 0, 0, 0, false, true) == H2_PAL_OK);
  assert(run(0, 0, 0, 0, 0, 0, true, true) == H2_PAL_OK);
  assert(run(0, 0, 0, 0, 0, 0, false, false) == H2_PAL_OK);
  assert(run(0, 0, 0, 0, 0, 0, 2u, true) == H2_PAL_ERR_INVALID_STATE);
  assert(run(0, 0, 0, 0, 0, 0, 3u, true) == H2_PAL_ERR_NO_SPACE);
  assert(run(0, 0, 0, 0, 0, 0, 4u, true) == H2_PAL_ERR_INVALID_STATE);
  assert(run(0, 0, 0, 0, 0, 0, 5u, true) == H2_PAL_ERR_NO_SPACE);
  for (unsigned i = 1; i <= 53; ++i)
    assert(run(i, 0, 0, 0, 0, 0, false, true) == H2_PAL_ERR_IO);
  for (unsigned i = 1; i <= 26; ++i)
    assert(run(0, i, 0, 0, 0, 0, false, true) == H2_PAL_ERR_TIMEOUT);
  for (unsigned i = 1; i <= 3; ++i)
    assert(run(0, 0, 0, 0, i, 0, false, true) == H2_PAL_ERR_NOT_FOUND);
  /* Delete acknowledgement is intentionally not followed by an absence
   * assertion until the Server stops poisoning owner-wide workspace lists. */
  /* Corrupt each arena with an escaped name or missing terminator. Every
   * nonempty baseline response must reject either corruption. Empty history
   * receives a malformed entry; empty post-delete lists are covered below. */
  const unsigned replies[] = {1,  2,  3,  4,  5,  6,  7,  8,  9,
                              10, 11, 12, 13, 14, 15, 16, 17, 18,
                              19, 20, 21, 22, 23, 24, 25, 26};
  for (size_t i = 0; i < sizeof(replies) / sizeof(replies[0]); ++i)
    for (unsigned mode = 5; mode <= 6; ++mode)
      assert(run(0, 0, replies[i], mode, 0, 0, false, true) ==
             H2_PAL_ERR_INVALID_STATE);
  for (unsigned i = 1; i <= 26; ++i)
    assert(run(0, 0, i, 17, 0, 0, false, true) == H2_PAL_ERR_INVALID_STATE);
  const unsigned objects[] = {1,  2,  3,  4,  5,  9,  10, 11, 12,
                              13, 14, 18, 19, 20, 21, 22, 23};
  for (size_t i = 0; i < sizeof(objects) / sizeof(objects[0]); ++i) {
    assert(run(0, 0, objects[i], 1, 0, 0, false, true) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(run(0, 0, objects[i], 2, 0, 0, false, true) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(run(0, 0, objects[i], 15, 0, 0, false, true) ==
           H2_PAL_ERR_INVALID_STATE);
  }
  const unsigned metadata[] = {2, 3, 5, 11, 12, 14, 20, 21, 23};
  for (size_t i = 0; i < sizeof(metadata) / sizeof(metadata[0]); ++i)
    for (unsigned mode = 3; mode <= 4; ++mode)
      assert(run(0, 0, metadata[i], mode, 0, 0, false, true) ==
             H2_PAL_ERR_INVALID_STATE);
  const unsigned pages[] = {3, 12, 21};
  for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); ++i)
    for (unsigned mode = 7; mode <= 11; ++mode)
      assert(run(0, 0, pages[i], mode, 0, 0, false, true) ==
             H2_PAL_ERR_INVALID_STATE);
  const unsigned histories[] = {8, 17, 26};
  for (size_t i = 0; i < sizeof(histories) / sizeof(histories[0]); ++i)
    assert(run(0, 0, histories[i], 12, 0, 0, false, true) ==
           H2_PAL_ERR_INVALID_STATE);
  const unsigned activations[] = {7, 16, 25};
  for (size_t i = 0; i < sizeof(activations) / sizeof(activations[0]); ++i)
    assert(run(0, 0, activations[i], 18, 0, 0, false, true) == H2_PAL_OK);
  for (size_t i = 0; i < sizeof(activations) / sizeof(activations[0]); ++i)
    for (unsigned mode = 13; mode <= 14; ++mode)
      assert(run(0, 0, activations[i], mode, 0, 0, false, true) ==
             H2_PAL_ERR_INVALID_STATE);
  const unsigned ready[] = {4, 5, 13, 14, 22, 23};
  for (size_t i = 0; i < sizeof(ready) / sizeof(ready[0]); ++i)
    assert(run(0, 0, ready[i], 16, 0, 0, false, true) ==
           H2_PAL_ERR_INVALID_STATE);
  test_name_boundaries();
  printf("Workspace boundary scenarios=%u\n", s_runs);
  return 0;
}
