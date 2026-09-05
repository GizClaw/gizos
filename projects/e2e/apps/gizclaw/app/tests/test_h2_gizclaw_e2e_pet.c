#include "h2_gizclaw_e2e_pet.h"
// Keep test operations and assertions enabled in optimized builds.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum method { ADOPT, GET, LIST, DRIVE, ACTIONS, PIXA, DELETE };
struct h2_gizclaw_req {
  enum method method;
  bool started, waited;
  int result;
  char cursor[256];
  h2_gizclaw_pet_pixa_write_fn write;
  void *user;
};
static struct {
  h2_gizclaw_e2e_fixture_t *fixture;
  unsigned stage, fail_stage, budget, fail_budget, replies, corrupt_reply,
      fault;
  unsigned live, calls[3][7], runs, deletes, sleeps;
  bool exists, emit;
  unsigned variant;
  uint64_t last_id;
  h2_gizclaw_pet_pixa_write_fn late_write;
  void *late_user;
  char remote[256];
} state;
static h2_pal_result_t sleep_ms(void *user, uint32_t ms) {
  (void)user;
  assert(ms == 100u);
  ++state.sleeps;
  if (state.variant == 9u)
    return H2_PAL_ERR_IO;
  if (state.variant == 8u && state.sleeps == 2u)
    state.exists = false;
  return H2_PAL_OK;
}
static const h2_pal_time_vtable_t time_vtable = {.sleep_ms = sleep_ms};
static const h2_pal_time_api_t time_api = {.vtable = &time_vtable};
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
int h2_gizclaw_e2e_fixture_call_sync(h2_gizclaw_e2e_fixture_t *fixture,
                                     h2_gizclaw_service_t *service,
                                     int (*fn)(void *ctx), void *ctx) {
  assert(fixture != NULL && service != NULL && fn != NULL);
  return fn(ctx);
}
bool h2_gizclaw_e2e_fixture_has_time(const h2_gizclaw_e2e_fixture_t *f,
                                     uint32_t ms) {
  assert(f == state.fixture && ms == 30000u);
  return ++state.budget != state.fail_budget &&
         !(state.variant == 10u && state.sleeps);
}
static void service(h2_gizclaw_service_t *s, uint32_t ms) {
  assert(s == (h2_gizclaw_service_t *)&state && ms == 30000u);
}
static void target(h2_gizclaw_str_t name) {
  assert(name.len == strlen(state.fixture->pet_name));
  assert(!memcmp(name.data, state.fixture->pet_name, name.len));
}
static void adopt_options(const h2_gizclaw_pet_adopt_options_t *o) {
  target(o->name);
  assert(o->display_name.len == strlen("H2 E2E Pet"));
  assert(!memcmp(o->display_name.data, "H2 E2E Pet", o->display_name.len));
}
static void drive_options(const h2_gizclaw_pet_drive_options_t *o) {
  target(o->pet_name);
  assert(o->behavior == H2_GIZCLAW_PET_BEHAVIOR_NONE && !o->game_result);
  const size_t name_len = strlen(state.fixture->pet_name);
  assert(o->idempotency_key.len == name_len + strlen("-drive-0"));
  assert(!memcmp(o->idempotency_key.data, state.fixture->pet_name, name_len));
  assert(!memcmp(o->idempotency_key.data + name_len, "-drive-",
                 strlen("-drive-")));
  char last = o->idempotency_key.data[o->idempotency_key.len - 1u];
  assert(last == '0' || last == '1');
}
static int perform(enum method m, h2_gizclaw_pet_pixa_write_fn write,
                   void *user) {
  if (m == ADOPT) {
    assert(state.fixture->pet_created);
    assert(!state.fixture->pet_delete_acknowledged);
    if (state.exists)
      assert(!strcmp(state.remote, state.fixture->pet_name));
    strcpy(state.remote, state.fixture->pet_name);
    state.exists = state.variant != 5u;
  }
  if (m != LIST && m != ADOPT && !state.exists)
    return H2_PAL_ERR_NOT_FOUND;
  if (m == DELETE) {
    ++state.deletes;
    state.exists =
        (state.variant == 6u && state.deletes == 1u) ||
        (state.variant == 7u && state.deletes == 2u) ||
        ((state.variant == 8u || state.variant == 9u || state.variant == 10u) &&
         state.deletes == 1u);
  }
  if (m == PIXA) {
    assert(write && user == state.fixture);
    state.late_write = write;
    state.late_user = user;
    const uint8_t bytes[] = {1, 2, 3, 4};
    assert(write(user, bytes, 1u) == H2_PAL_OK);
    assert(write(user, bytes + 1u, 3u) == H2_PAL_OK);
  }
  return H2_PAL_OK;
}
static int create(enum method m, h2_gizclaw_service_t *s, uint64_t id,
                  h2_gizclaw_str_t cursor, h2_gizclaw_pet_pixa_write_fn write,
                  void *user, uint32_t timeout, h2_gizclaw_req_t **out) {
  service(s, timeout);
  assert(id > state.last_id);
  state.last_id = id;
  ++state.calls[0][m];
  *out = NULL;
  int rc = step();
  if (rc)
    return rc;
  *out = calloc(1u, sizeof(**out));
  assert(*out);
  (*out)->method = m;
  assert(cursor.len < sizeof((*out)->cursor));
  if (cursor.len)
    memcpy((*out)->cursor, cursor.data, cursor.len);
  (*out)->write = write;
  (*out)->user = user;
  ++state.live;
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_req_do(h2_gizclaw_req_t *r,
                                  void *user,
                                  h2_gizclaw_req_input_read_fn input_read,
                                  h2_gizclaw_req_output_write_fn output_write,
                                  h2_gizclaw_req_complete_fn on_complete) {
  (void)on_complete;
  assert(r && !r->started && !input_read);
  assert((r->method == PIXA) == (user != NULL && output_write != NULL));
  int rc = step();
  if (!rc) {
    r->started = true;
    if (r->method == PIXA) {
      const uint8_t bytes[] = {1, 2, 3, 4};
      size_t written = 0u;
      r->result = output_write(user, bytes, 1u, &written);
      assert(r->result == H2_PAL_OK && written == 1u);
      r->result = output_write(user, bytes + 1u, 3u, &written);
      assert(r->result == H2_PAL_OK && written == 3u);
    } else {
      r->result = perform(r->method, r->write, r->user);
    }
  }
  return rc;
}
h2_pal_result_t h2_gizclaw_req_wait(h2_gizclaw_req_t *r, uint32_t ms) {
  assert(r->started && (ms == 1u || ms == 30000u));
  int rc = step();
  r->waited = !rc;
  return rc ? rc : r->result;
}
h2_pal_result_t h2_gizclaw_service_poll(h2_gizclaw_service_t *service,
                                        size_t max_events,
                                        size_t *out_dispatched) {
  assert(service == state.fixture->actors[0].service && max_events == 8u &&
         out_dispatched != NULL);
  *out_dispatched = 0u;
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_req_cancel(h2_gizclaw_req_t *r) {
  assert(r);
  return H2_PAL_OK;
}
void h2_gizclaw_req_release(h2_gizclaw_req_t *r) {
  assert(r && state.live);
  --state.live;
  free(r);
}
static void *alloc(h2_gizclaw_resp_storage_t *s, size_t n, size_t alignment) {
  uintptr_t address = (uintptr_t)s->data + s->used;
  size_t padding = (alignment - address % alignment) % alignment;
  assert(s->used + padding + n <= s->capacity);
  void *p = (char *)s->data + s->used + padding;
  s->used += n + padding;
  memset(p, 0, n);
  return p;
}
static char *save(h2_gizclaw_resp_storage_t *s, const char *value) {
  char *p = alloc(s, strlen(value) + 1u, 1u);
  strcpy(p, value);
  return p;
}
static void pet(h2_gizclaw_resp_storage_t *s, h2_gizclaw_pet_t *p,
                const char *name) {
  *p = (h2_gizclaw_pet_t){.name = save(s, name),
                          .pet_def_name = save(s, "server-definition"),
                          .display_name = save(s, "H2 E2E Pet"),
                          .lifecycle = 127};
}
static void reply(enum method m, const char *cursor,
                  h2_gizclaw_resp_storage_t *s, void *out) {
  assert(s->used == 0u);
  memset(s->data, 0xa5, s->capacity);
  unsigned fault = ++state.replies == state.corrupt_reply ? state.fault : 0u;
  h2_gizclaw_pet_t *p = NULL;
  char **name = NULL;
  if (m == LIST) {
    h2_gizclaw_pet_page_t *page = out;
    memset(page, 0, sizeof(*page));
    page->items =
        alloc(s, 2u * sizeof(*page->items), _Alignof(h2_gizclaw_pet_t));
    page->count = 1u;
    p = &page->items[0];
    bool first = state.variant == 1u && !cursor[0];
    pet(s, p, first ? "unrelated" : state.fixture->pet_name);
    if (first) {
      page->has_next = true;
      page->next_cursor = save(s, "next");
    }
    if (state.variant == 2u) {
      pet(s, p, "unrelated");
      page->has_next = true;
      page->next_cursor = save(s, !strcmp(cursor, "a") ? "b" : "a");
    }
    if (state.variant == 3u && !cursor[0]) {
      page->has_next = true;
      page->next_cursor = save(s, "next");
    }
    if (fault == 10u)
      page->count = 33u;
    if (fault == 11u)
      page->items = NULL;
    if (fault == 12u)
      page->items = (void *)((char *)page->items + 1u);
    if (fault == 13u) {
      page->count = 2u;
      pet(s, &page->items[1], state.fixture->pet_name);
    }
    if (fault == 14u) {
      page->has_next = true;
      page->next_cursor = save(s, cursor);
    }
    if (fault == 15u) {
      page->has_next = true;
      page->next_cursor = "unowned";
    }
    if (fault == 16u) {
      page->has_next = true;
      page->next_cursor = alloc(s, 300u, 1u);
      memset(page->next_cursor, 'a', 299u);
    }
    if (fault == 17u)
      page->count = 0u;
  } else if (m == ACTIONS) {
    h2_gizclaw_pet_actions_t *a = out;
    *a = (h2_gizclaw_pet_actions_t){
        .pet_name = save(s, state.fixture->pet_name),
        .pet_def_name = save(s, "server-definition")};
    name = &a->pet_name;
    a->clip_name_count = 1u;
    a->clip_names = alloc(s, 2u * sizeof(*a->clip_names),
                          _Alignof(h2_gizclaw_pet_clip_name_t));
    a->clip_names[0].id = save(s, "custom-server-action");
    a->clip_names[0].pixa_clip_name = save(s, "custom-clip");
    if (state.variant == 4u) {
      a->clip_name_count = 256u;
      a->clip_names = alloc(s, 256u * sizeof(*a->clip_names),
                            _Alignof(h2_gizclaw_pet_clip_name_t));
      for (size_t i = 0u; i < a->clip_name_count; ++i) {
        char label[32];
        snprintf(label, sizeof(label), "clip-%zu", i);
        a->clip_names[i].id = save(s, label);
        a->clip_names[i].pixa_clip_name = save(s, label);
      }
    }
    if (fault == 10u)
      a->clip_name_count = 257u;
    if (fault == 11u)
      a->clip_names = NULL;
    if (fault == 12u)
      a->clip_names = (void *)((char *)a->clip_names + 1u);
    if (fault == 13u) {
      a->clip_name_count = 2u;
      a->clip_names[1] = a->clip_names[0];
    }
    if (fault == 14u)
      a->clip_names[0].id = "unowned";
    if (fault == 15u)
      a->clip_names[0].pixa_clip_name = NULL;
    if (fault == 16u)
      a->feed = "unowned";
    if (fault == 17u)
      a->pet_def_name = save(s, "wrong");
  } else if (m == PIXA) {
    h2_gizclaw_pet_pixa_info_t *i = out;
    *i = (h2_gizclaw_pet_pixa_info_t){
        .pet_name = save(s, state.fixture->pet_name),
        .pet_def_name = save(s, "server-definition"),
        .source_path = save(s, "pets/asset.pixa"),
        .size_bytes = 4u,
        .received_bytes = 4u};
    name = &i->pet_name;
    if (fault == 10u)
      i->size_bytes = 0u;
    if (fault == 11u)
      i->received_bytes = 3u;
    if (fault == 12u)
      i->size_bytes = i->received_bytes = 5u;
    if (fault == 13u)
      i->source_path = "unowned";
    if (fault == 14u)
      i->source_path = NULL;
    if (fault == 15u)
      i->pet_def_name = save(s, "wrong");
  } else {
    p = out;
    pet(s, p, state.fixture->pet_name);
  }
  if (p) {
    name = &p->name;
    if (fault == 6u)
      p->pet_def_name = save(s, "wrong");
    if (fault == 7u)
      p->display_name = save(s, "not-persisted");
    if (fault == 8u)
      p->stats.energy = NAN;
    if (fault == 9u)
      p->updated_at = "unowned";
  }
  if (fault == 1u)
    *name = save(s, "wrong");
  if (fault == 2u)
    *name = "unowned";
  if (fault == 3u)
    *name = NULL;
  if (fault == 4u) {
    *name = alloc(s, 1u, 1u);
    **name = 'x';
  }
  if (fault == 5u)
    s->used = s->capacity + 1u;
}
static int parse(enum method m, const h2_gizclaw_req_t *r,
                 h2_gizclaw_resp_storage_t *s, void *out) {
  assert(r && r->method == m && r->waited);
  ++state.calls[1][m];
  int rc = step();
  if (!rc)
    reply(m, r->cursor, s, out);
  return rc;
}
static int rpc(enum method m, h2_gizclaw_service_t *s, h2_gizclaw_str_t cursor,
               h2_gizclaw_pet_pixa_write_fn write, void *user, uint32_t ms,
               h2_gizclaw_resp_storage_t *storage, void *out) {
  service(s, ms);
  ++state.calls[2][m];
  int rc = step();
  if (!rc)
    rc = perform(m, write, user);
  char c[256] = {0};
  assert(cursor.len < sizeof(c));
  if (cursor.len)
    memcpy(c, cursor.data, cursor.len);
  if (!rc)
    reply(m, c, storage, out);
  return rc;
}
#define PARSER(method, word, type)                                             \
  h2_pal_result_t h2_gizclaw_resp_parse_pet_##word(                            \
      const h2_gizclaw_req_t *r, h2_gizclaw_resp_storage_t *s, type *out) {    \
    return parse(method, r, s, out);                                           \
  }
#define NAMED(method, word, type)                                              \
  h2_pal_result_t h2_gizclaw_req_create_pet_##word(                            \
      h2_gizclaw_service_t *s, uint64_t id, h2_gizclaw_str_t name,             \
      uint32_t ms, h2_gizclaw_req_t **out) {                                   \
    target(name);                                                              \
    return create(method, s, id, (h2_gizclaw_str_t){0}, NULL, NULL, ms, out);  \
  }                                                                            \
  h2_pal_result_t h2_gizclaw_rpc_pet_##word(                                   \
      h2_gizclaw_service_t *s, h2_gizclaw_str_t name, uint32_t ms,             \
      h2_gizclaw_resp_storage_t *storage, type *out) {                         \
    target(name);                                                              \
    return rpc(method, s, (h2_gizclaw_str_t){0}, NULL, NULL, ms, storage,      \
               out);                                                           \
  }                                                                            \
  PARSER(method, word, type)
NAMED(GET, get, h2_gizclaw_pet_t)
NAMED(DELETE, delete, h2_gizclaw_pet_t)
NAMED(ACTIONS, action_get, h2_gizclaw_pet_actions_t)
#define OPTIONS(method, word, type, check)                                     \
  h2_pal_result_t h2_gizclaw_req_create_pet_##word(                            \
      h2_gizclaw_service_t *s, uint64_t id, const type *o, uint32_t ms,        \
      h2_gizclaw_req_t **out) {                                                \
    check(o);                                                                  \
    return create(method, s, id, (h2_gizclaw_str_t){0}, NULL, NULL, ms, out);  \
  }                                                                            \
  h2_pal_result_t h2_gizclaw_rpc_pet_##word(                                   \
      h2_gizclaw_service_t *s, const type *o, uint32_t ms,                     \
      h2_gizclaw_resp_storage_t *storage, h2_gizclaw_pet_t *out) {             \
    check(o);                                                                  \
    return rpc(method, s, (h2_gizclaw_str_t){0}, NULL, NULL, ms, storage,      \
               out);                                                           \
  }                                                                            \
  PARSER(method, word, h2_gizclaw_pet_t)
OPTIONS(ADOPT, adopt, h2_gizclaw_pet_adopt_options_t, adopt_options)
OPTIONS(DRIVE, drive, h2_gizclaw_pet_drive_options_t, drive_options)
h2_pal_result_t h2_gizclaw_req_create_pet_list(h2_gizclaw_service_t *s,
                                               uint64_t id, h2_gizclaw_str_t c,
                                               size_t limit, uint32_t ms,
                                               h2_gizclaw_req_t **out) {
  assert(limit == 32u);
  return create(LIST, s, id, c, NULL, NULL, ms, out);
}
h2_pal_result_t h2_gizclaw_rpc_pet_list(h2_gizclaw_service_t *s,
                                        h2_gizclaw_str_t c, size_t limit,
                                        uint32_t ms,
                                        h2_gizclaw_resp_storage_t *storage,
                                        h2_gizclaw_pet_page_t *out) {
  assert(limit == 32u);
  return rpc(LIST, s, c, NULL, NULL, ms, storage, out);
}
PARSER(LIST, list, h2_gizclaw_pet_page_t)
h2_pal_result_t h2_gizclaw_req_create_pet_pixa_download(
    h2_gizclaw_service_t *s, uint64_t id, h2_gizclaw_str_t name,
    uint32_t ms, h2_gizclaw_req_t **out) {
  target(name);
  return create(PIXA, s, id, (h2_gizclaw_str_t){0}, NULL, NULL, ms, out);
}
h2_pal_result_t h2_gizclaw_rpc_pet_pixa_download(
    h2_gizclaw_service_t *s, h2_gizclaw_str_t name,
    h2_gizclaw_pet_pixa_write_fn write, void *user, uint32_t ms,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_pet_pixa_info_t *out) {
  target(name);
  return rpc(PIXA, s, (h2_gizclaw_str_t){0}, write, user, ms, storage, out);
}
PARSER(PIXA, pixa_download, h2_gizclaw_pet_pixa_info_t)

static void run(unsigned failure, unsigned budget, unsigned reply_index,
                unsigned fault, unsigned variant, bool emit) {
  unsigned runs = state.runs + 1u;
  memset(&state, 0, sizeof(state));
  state.runs = runs;
  h2_gizclaw_e2e_fixture_t *f = calloc(1, sizeof(*f));
  assert(f);
  state.fixture = f;
  state.fail_stage = failure;
  state.fail_budget = budget;
  state.corrupt_reply = reply_index;
  state.fault = fault;
  state.variant = variant;
  state.emit = emit;
  f->time = &time_api;
  strcpy(f->pet_name, "test-pet");
  f->actors[H2_GIZCLAW_E2E_OWNER].service = (void *)&state;
  uint8_t arena[16384];
  h2_gizclaw_resp_storage_t storage = {.data = arena,
                                       .capacity = sizeof(arena)};
  if (emit) {
    puts("H2_GIZCLAW_E2E case=rpc stage=coverage-begin");
    puts("H2_GIZCLAW_E2E case=rpc/gameplay stage=coverage-begin");
  }
  int rc = h2_gizclaw_e2e_run_pet(f, &storage);
  bool success =
      !failure && !budget && !reply_index &&
      (variant == 0u || variant == 1u || variant == 4u || variant == 8u);
  if (emit) {
    printf("H2_GIZCLAW_E2E case=rpc/gameplay stage=coverage-end status=%s "
           "rc=%d cleanup_rc=0\n",
           rc ? "FAIL" : "PASS", rc);
    printf("H2_GIZCLAW_E2E case=rpc stage=coverage-end status=%s rc=%d "
           "cleanup_rc=0\n",
           rc ? "FAIL" : "PASS", rc);
  }
  if ((rc == H2_PAL_OK) != success)
    fprintf(stderr, "failure=%u budget=%u reply=%u fault=%u rc=%d\n", failure,
            budget, reply_index, fault, rc);
  assert((rc == H2_PAL_OK) == success);
  assert(!state.live);
  assert(f->pet_created ==
         !(failure == 1u || budget == 1u || budget == 17u || budget == 21u));
  if (state.exists)
    assert(f->pet_created && !strcmp(state.remote, f->pet_name));
  bool acknowledged = failure == 40u || failure == 44u || budget == 16u ||
                      budget == 20u || variant == 6u || variant == 7u ||
                      variant == 9u || variant == 10u;
  assert(f->pet_delete_acknowledged == acknowledged);
  if (success) {
    for (unsigned a = 0; a < 3u; ++a)
      for (unsigned m = 0; m < 7u; ++m)
        assert(state.calls[a][m] > 0u);
    assert(storage.used == 0u);
    assert(!strcmp(f->pet_name, "test-pet-keep") && state.exists);
    assert(state.replies == (variant == 1u || variant == 8u ? 22u : 20u));
    assert(state.stage == (variant == 1u ? 51u : variant == 8u ? 48u : 46u));
  }
  if (variant == 2u)
    assert(state.calls[0][LIST] == 32u && rc == H2_PAL_ERR_NO_SPACE);
  if (variant == 3u)
    assert(state.calls[0][LIST] == 2u && rc == H2_PAL_ERR_FORMAT);
  /* Even a sink invoked after a failed wait still has heap-owned storage;
   * teardown must stop Service before freeing the fixture. */
  if (state.late_write) {
    const uint8_t byte = 1u;
    uint64_t prior = atomic_load(&f->pet_download_bytes);
    assert(state.late_write(state.late_user, &byte, 1u) == H2_PAL_OK);
    assert(atomic_load(&f->pet_download_bytes) == prior + 1u);
    atomic_store(&f->pet_download_bytes, SIZE_MAX);
    assert(state.late_write(state.late_user, &byte, 1u) == H2_PAL_ERR_NO_SPACE);
  }
  free(f);
}
static void test_name_boundaries(void) {
  const size_t capacity = sizeof(((h2_gizclaw_e2e_fixture_t *)0)->pet_name);
  const size_t lengths[] = {capacity - sizeof("-keep"),
                            capacity - sizeof("-keep") + 1u,
                            capacity - 1u, capacity};
  for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
    memset(&state, 0, sizeof(state));
    h2_gizclaw_e2e_fixture_t *f = calloc(1, sizeof(*f));
    assert(f != NULL);
    state.fixture = f;
    f->time = &time_api;
    f->actors[H2_GIZCLAW_E2E_OWNER].service = (void *)&state;
    memset(f->pet_name, 'n', lengths[i]);
    char original[sizeof(f->pet_name)];
    memcpy(original, f->pet_name, sizeof(original));
    uint8_t arena[16384];
    h2_gizclaw_resp_storage_t storage = {.data = arena,
                                        .capacity = sizeof(arena)};
    int rc = h2_gizclaw_e2e_run_pet(f, &storage);
    assert(state.live == 0u);
    if (i == 0u) {
      assert(rc == H2_PAL_OK && state.exists && f->pet_created);
      assert(strlen(f->pet_name) == capacity - 1u);
      assert(memcmp(f->pet_name, original, lengths[i]) == 0);
      assert(strcmp(f->pet_name + lengths[i], "-keep") == 0);
      assert(strcmp(state.remote, f->pet_name) == 0);
      assert(state.stage == 46u && storage.used == 0u);
    } else {
      assert(rc == (lengths[i] == capacity ? H2_PAL_ERR_INVALID_ARG
                                          : H2_PAL_ERR_TRUNCATED));
      assert(state.stage == 0u && state.budget == 0u && !f->pet_created);
      assert(memcmp(f->pet_name, original, sizeof(original)) == 0);
    }
    free(f);
  }
}

int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "--emit-success-evidence")) {
    run(0, 0, 0, 0, false, true);
    return 0;
  }
  if (argc == 4 && !strcmp(argv[1], "--emit-failure-evidence")) {
    run((unsigned)atoi(argv[2]), (unsigned)atoi(argv[3]), 0, 0, false, true);
    return 0;
  }
  if (argc == 3 && !strcmp(argv[1], "--emit-variant-evidence")) {
    run(0, 0, 0, 0, (unsigned)atoi(argv[2]), true);
    return 0;
  }
  run(0, 0, 0, 0, false, false);
  run(0, 0, 0, 0, true, false);
  for (unsigned variant = 2u; variant <= 10u; ++variant)
    run(0, 0, 0, 0, variant, false);
  for (unsigned i = 1u; i <= 46u; ++i)
    run(i, 0, 0, 0, false, false);
  for (unsigned i = 1u; i <= 22u; ++i)
    run(0, i, 0, 0, false, false);
  for (unsigned r = 1u; r <= 20u; ++r)
    for (unsigned fault = 1u; fault <= 5u; ++fault)
      run(0, 0, r, fault, false, false);
  const unsigned pets[] = {2, 4, 5, 8, 9, 11, 12};
  for (unsigned i = 0; i < sizeof(pets) / sizeof(pets[0]); ++i)
    for (unsigned fault = 6u; fault <= 9u; ++fault)
      run(0, 0, pets[i], fault, false, false);
  const unsigned lists[] = {3, 10}, actions[] = {6, 13}, pixas[] = {7, 14};
  for (unsigned i = 0; i < 2u; ++i) {
    for (unsigned fault = 10u; fault <= 17u; ++fault) {
      run(0, 0, lists[i], fault, false, false);
      run(0, 0, actions[i], fault, false, false);
    }
    for (unsigned fault = 10u; fault <= 15u; ++fault)
      run(0, 0, pixas[i], fault, false, false);
  }
  printf("Pet case: %u scenarios passed\n", state.runs);
  test_name_boundaries();
  return 0;
}
