#include "h2_gizclaw_e2e_group_message.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct h2_gizclaw_req { unsigned phase; bool get; };
static struct {
  h2_gizclaw_e2e_fixture_t *fixture;
  h2_gizclaw_req_t request;
  unsigned stages, fail, budgets, budget_fail, responses, fault_response, fault;
  unsigned pages, pagination, creates, releases, cancels, proofs;
  uint64_t last_id;
  bool emit;
} state;

h2_gizclaw_str_t h2_gizclaw_e2e_str(const char *s) {
  return (h2_gizclaw_str_t){s, strlen(s)};
}
bool h2_gizclaw_e2e_fixture_has_time(const h2_gizclaw_e2e_fixture_t *f, uint32_t ms) {
  assert(f == state.fixture && ms == 30000u);
  return ++state.budgets != state.budget_fail;
}
void h2_gizclaw_e2e_evidence(const char *symbol, const char *stage, int rc) {
  if (strstr(stage, "-assert") && !rc)
    ++state.proofs;
  if (state.emit)
    printf("H2_GIZCLAW_E2E symbol=%s stage=%s result=%s rc=%d\n", symbol,
           stage, rc ? "FAIL" : "PASS", rc);
}
static int step(void) {
  return ++state.stages == state.fail ? H2_PAL_ERR_IO : H2_PAL_OK;
}
static void *allocate(h2_gizclaw_resp_storage_t *s, size_t n) {
  const size_t align = _Alignof(max_align_t);
  size_t offset = (s->used + align - 1u) / align * align;
  assert(offset <= s->capacity && n <= s->capacity - offset);
  void *out = s->data + offset;
  memset(out, 0, n);
  s->used = offset + n;
  return out;
}
static char *save(h2_gizclaw_resp_storage_t *s, const char *text) {
  size_t len = strlen(text) + 1u;
  char *out = allocate(s, len);
  memcpy(out, text, len);
  return out;
}
static char *long_text(h2_gizclaw_resp_storage_t *s, size_t n) {
  char *out = allocate(s, n + 1u);
  memset(out, 'x', n);
  return out;
}
static void arguments(h2_gizclaw_service_t *service, h2_gizclaw_str_t group,
                      h2_gizclaw_str_t arg, uint32_t timeout, bool get) {
  assert(service == state.fixture->actors[0].service && timeout == 30000u);
  assert(group.len == 5u && !memcmp(group.data, "group", 5u));
  if (get) {
    assert(arg.len == 7u && !memcmp(arg.data, "history", 7u));
  } else if (!arg.len) {
    state.pages = 1u;
  } else {
    char boundary_cursor[256] = {0};
    if (state.pagination == 9u)
      snprintf(boundary_cursor, sizeof(boundary_cursor), "p%u", state.pages);
    if (state.pagination == 10u)
      memset(boundary_cursor, 'c', 255u);
    const char *cursor = state.pagination >= 9u ? boundary_cursor
                         : state.pagination == 3u
                             ? (state.pages % 2u ? "a" : "b") : "next";
    assert(arg.len == strlen(cursor) && !memcmp(arg.data, cursor, arg.len));
    ++state.pages;
  }
}
static void message(h2_gizclaw_resp_storage_t *s,
                     h2_gizclaw_friend_group_message_t *m, unsigned fault) {
  memset(m, 0, sizeof(*m));
  m->history_id = save(s, "history");
  m->friend_group_name = save(s, "group");
  m->sender_peer_public_key = save(s, "owner-key");
  m->type = H2_GIZCLAW_FRIEND_GROUP_MESSAGE_TYPE_GEAR;
  m->audio_available = true;
  m->text = state.pagination == 8u ? NULL : save(s, "transcript");
  switch (fault) {
  case 1: s->used = s->capacity + 1u; break;
  case 2: m->friend_group_name = save(s, "wrong"); break;
  case 3: m->history_id = NULL; break;
  case 4: m->history_id = save(s, ""); break;
  case 5: m->history_id = "history"; break;
  case 6:
    m->history_id = (char *)s->data + s->capacity - 7u;
    memcpy(m->history_id, "history", 7u);
    s->used = s->capacity;
    break;
  case 7: m->sender_peer_public_key = save(s, "other-key"); break;
  case 8: m->type = H2_GIZCLAW_FRIEND_GROUP_MESSAGE_TYPE_AGENT; break;
  case 9: m->audio_available = false; break;
  case 10: m->text = "unowned"; break;
  case 11: m->text = long_text(s, 4097u); break;
  case 12: m->name = "unowned"; break;
  case 13: m->history_id = save(s, "other-history"); break;
  case 14: m->created_at = "unowned"; break;
  case 15: m->expires_at = "unowned"; break;
  case 16: m->history_id = long_text(s, 256u); break;
  case 17: m->sender_peer_public_key = NULL; break;
  case 18: m->sender_peer_public_key = long_text(s, 65u); break;
  }
}
static void response(h2_gizclaw_resp_storage_t *s, void *out, bool get) {
  assert(!s->used);
  memset(s->data, 0xa5, s->capacity);
  unsigned fault = ++state.responses == state.fault_response ? state.fault : 0u;
  if (get) {
    message(s, out, fault);
    return;
  }
  h2_gizclaw_friend_group_message_page_t *p = out;
  memset(p, 0, sizeof(*p));
  p->count = state.pagination == 4u || state.pagination == 6u ? 2u : 1u;
  p->items = allocate(s, p->count * sizeof(*p->items));
  message(s, &p->items[0], fault);
  if (p->count == 2u) {
    p->items[1] = p->items[0];
    if (state.pagination == 6u) {
      p->items[0].history_id = save(s, "future-history");
      p->items[0].type = (h2_gizclaw_friend_group_message_type_t)127;
      p->items[0].sender_peer_public_key = NULL;
      p->items[0].audio_available = false;
      p->items[0].text = NULL;
    }
  }
  if (state.pagination == 1u) {
    if (state.pages == 1u) {
      p->count = 0u;
      p->has_next = true;
      p->next_cursor = save(s, "next");
    }
  } else if (state.pagination == 2u || state.pagination == 3u) {
    p->count = 0u;
    p->has_next = true;
    p->next_cursor = save(s, state.pagination == 3u
                                ? (state.pages % 2u ? "a" : "b") : "next");
  } else if (state.pagination == 5u) {
    p->has_next = state.pages == 1u;
    p->next_cursor = p->has_next ? save(s, "next") : NULL;
  } else if (state.pagination == 7u) {
    p->count = 0u;
  } else if (state.pagination == 9u || state.pagination == 10u) {
    if (state.pages < (state.pagination == 9u ? 32u : 2u)) {
      p->count = 0u;
      p->has_next = true;
      char cursor[256] = {0};
      if (state.pagination == 9u)
        snprintf(cursor, sizeof(cursor), "p%u", state.pages);
      else
        memset(cursor, 'c', 255u);
      p->next_cursor = save(s, cursor);
    }
  }
  switch (fault) {
  case 19: p->count = 33u; break;
  case 20: p->items = NULL; break;
  case 21: p->items = (void *)((uint8_t *)p->items + 1u); break;
  case 22: p->items = (void *)&state; break;
  case 23: p->has_next = true; p->next_cursor = NULL; break;
  case 24: p->has_next = true; p->next_cursor = save(s, ""); break;
  case 25: p->has_next = true; p->next_cursor = "unowned"; break;
  case 26: p->has_next = true; p->next_cursor = long_text(s, 256u); break;
  }
}

static int create(h2_gizclaw_service_t *service, uint64_t id,
                   h2_gizclaw_str_t group, h2_gizclaw_str_t arg,
                   uint32_t timeout, bool get, h2_gizclaw_req_t **out) {
  assert(!state.request.phase && id > state.last_id && id >= 501u);
  state.last_id = id;
  arguments(service, group, arg, timeout, get);
  *out = NULL;
  int rc = step();
  if (!rc) {
    state.request = (h2_gizclaw_req_t){.phase = 1u, .get = get};
    *out = &state.request;
    ++state.creates;
  }
  return rc;
}
h2_pal_result_t h2_gizclaw_req_create_friend_group_message_list(
    h2_gizclaw_service_t *service, uint64_t id, h2_gizclaw_str_t group,
    h2_gizclaw_str_t cursor, size_t limit, uint32_t timeout, h2_gizclaw_req_t **out) {
  assert(limit == 32u);
  return create(service, id, group, cursor, timeout, false, out);
}
h2_pal_result_t h2_gizclaw_req_create_friend_group_message_get(
    h2_gizclaw_service_t *service, uint64_t id, h2_gizclaw_str_t group,
    h2_gizclaw_str_t history, uint32_t timeout, h2_gizclaw_req_t **out) {
  return create(service, id, group, history, timeout, true, out);
}
h2_pal_result_t h2_gizclaw_req_do(h2_gizclaw_req_t *r,
    void *user, h2_gizclaw_req_input_read_fn input_read,
    h2_gizclaw_req_output_write_fn output_write,
                                  h2_gizclaw_req_complete_fn on_complete) {
  (void)on_complete;
  assert(r == &state.request && r->phase == 1u && !user && !input_read &&
         !output_write);
  ++r->phase;
  return step();
}
h2_pal_result_t h2_gizclaw_req_wait(h2_gizclaw_req_t *r, uint32_t timeout) {
  assert(r == &state.request && r->phase == 2u && timeout == 30000u);
  ++r->phase;
  return step();
}
static int parse(const h2_gizclaw_req_t *r, h2_gizclaw_resp_storage_t *s,
                  void *out, bool get) {
  assert(r == &state.request && r->phase == 3u && r->get == get);
  int rc = step();
  if (!rc)
    response(s, out, get);
  return rc;
}
h2_pal_result_t h2_gizclaw_resp_parse_friend_group_message_list(
    const h2_gizclaw_req_t *r, h2_gizclaw_resp_storage_t *s,
    h2_gizclaw_friend_group_message_page_t *out) { return parse(r, s, out, false); }
h2_pal_result_t h2_gizclaw_resp_parse_friend_group_message_get(
    const h2_gizclaw_req_t *r, h2_gizclaw_resp_storage_t *s,
    h2_gizclaw_friend_group_message_t *out) { return parse(r, s, out, true); }
h2_pal_result_t h2_gizclaw_req_cancel(h2_gizclaw_req_t *r) {
  assert(r == &state.request && r->phase);
  ++state.cancels;
  return H2_PAL_ERR_CLOSED; /* Must not overwrite the original failure. */
}
void h2_gizclaw_req_release(h2_gizclaw_req_t *r) {
  assert(r == &state.request && r->phase);
  ++state.releases;
  r->phase = 0u;
}
static int rpc(h2_gizclaw_service_t *service, h2_gizclaw_str_t group,
                h2_gizclaw_str_t arg, uint32_t timeout,
                h2_gizclaw_resp_storage_t *s, void *out, bool get) {
  arguments(service, group, arg, timeout, get);
  int rc = step();
  if (!rc)
    response(s, out, get);
  return rc;
}
h2_pal_result_t h2_gizclaw_rpc_friend_group_message_list(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t group,
    h2_gizclaw_str_t cursor, size_t limit, uint32_t timeout,
    h2_gizclaw_resp_storage_t *s, h2_gizclaw_friend_group_message_page_t *out) {
  assert(limit == 32u);
  return rpc(service, group, cursor, timeout, s, out, false);
}
h2_pal_result_t h2_gizclaw_rpc_friend_group_message_get(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t group,
    h2_gizclaw_str_t history, uint32_t timeout,
    h2_gizclaw_resp_storage_t *s, h2_gizclaw_friend_group_message_t *out) {
  return rpc(service, group, history, timeout, s, out, true);
}

static void scenario(unsigned fail, unsigned budget, unsigned fault,
                      unsigned fault_response, unsigned pagination, bool emit) {
  memset(&state, 0, sizeof(state));
  state.fail = fail;
  state.budget_fail = budget;
  state.fault = fault;
  state.fault_response = fault_response;
  state.pagination = pagination;
  state.emit = emit;
  state.fixture = calloc(1u, sizeof(*state.fixture));
  assert(state.fixture);
  state.fixture->actors[0].service = (h2_gizclaw_service_t *)&state;
  state.fixture->friend_group_created = true;
  strcpy(state.fixture->friend_group_name, "group");
  strcpy(state.fixture->actors[0].public_key, "owner-key");
  uint8_t *arena = malloc(65536u);
  assert(arena);
  h2_gizclaw_resp_storage_t storage = {.data = arena, .capacity = 65536u};
  if (emit) {
    puts("H2_GIZCLAW_E2E case=rpc stage=coverage-begin");
    puts("H2_GIZCLAW_E2E case=rpc/group stage=coverage-begin");
  }
  int rc = h2_gizclaw_e2e_run_group_message(state.fixture, &storage,
                                            h2_gizclaw_e2e_str("history"));
  bool success = !fail && !budget && !fault &&
                 (pagination == 0u || pagination == 1u ||
                  pagination == 6u || pagination >= 8u);
  assert((rc == H2_PAL_OK) == success && !storage.used);
  assert(state.creates == state.releases && !state.request.phase);
  assert(state.fixture->friend_group_created);
  assert(!strcmp(state.fixture->friend_group_name, "group"));
  if (success)
    assert(state.proofs == 4u && state.cancels == 0u);
  if (success && !pagination)
    assert(state.stages == 10u && state.budgets == 4u && state.responses == 4u);
  if (fail)
    assert(rc == H2_PAL_ERR_IO && state.stages == fail);
  if (fail && !pagination)
    assert(state.cancels == ((fail >= 2u && fail <= 4u) ||
                             (fail >= 6u && fail <= 8u) ? 1u : 0u));
  if (budget)
    assert(rc == H2_PAL_ERR_TIMEOUT && state.budgets == budget);
  if (pagination == 3u)
    assert(rc == H2_PAL_ERR_NO_SPACE && state.pages == 32u);
  if (pagination == 7u)
    assert(rc == H2_PAL_ERR_NOT_FOUND);
  if (emit) {
    printf("H2_GIZCLAW_E2E case=rpc/group stage=coverage-end status=%s rc=%d cleanup_rc=0\n",
           rc ? "FAIL" : "PASS", rc);
    printf("H2_GIZCLAW_E2E case=rpc stage=coverage-end status=%s rc=%d cleanup_rc=0\n",
           rc ? "FAIL" : "PASS", rc);
  }
  free(arena);
  free(state.fixture);
}

static void invalid_inputs(void) {
  memset(&state, 0, sizeof(state));
  h2_gizclaw_e2e_fixture_t fixture = {0};
  fixture.actors[0].service = (h2_gizclaw_service_t *)&state;
  fixture.friend_group_created = true;
  strcpy(fixture.friend_group_name, "group");
  strcpy(fixture.actors[0].public_key, "owner-key");
  uint8_t arena[64];
  h2_gizclaw_resp_storage_t storage = {.data = arena, .capacity = sizeof(arena)};
  h2_gizclaw_str_t history = h2_gizclaw_e2e_str("history");
  assert(h2_gizclaw_e2e_run_group_message(NULL, &storage, history) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_e2e_run_group_message(&fixture, NULL, history) == H2_PAL_ERR_INVALID_ARG);
  storage.data = NULL;
  assert(h2_gizclaw_e2e_run_group_message(&fixture, &storage, history) == H2_PAL_ERR_INVALID_ARG);
  storage.data = arena;
  storage.capacity = 0u;
  assert(h2_gizclaw_e2e_run_group_message(&fixture, &storage, history) == H2_PAL_ERR_INVALID_ARG);
  storage.capacity = sizeof(arena);
  const h2_gizclaw_str_t bad[] = {{NULL, 7u}, {"", 0u}, {"a\0b", 3u}, {"", SIZE_MAX}};
  for (size_t i = 0u; i < sizeof(bad) / sizeof(*bad); ++i)
    assert(h2_gizclaw_e2e_run_group_message(&fixture, &storage, bad[i]) == H2_PAL_ERR_INVALID_ARG);
  fixture.actors[0].service = NULL;
  assert(h2_gizclaw_e2e_run_group_message(&fixture, &storage, history) == H2_PAL_ERR_INVALID_ARG);
  fixture.actors[0].service = (h2_gizclaw_service_t *)&state;
  fixture.friend_group_created = false;
  assert(h2_gizclaw_e2e_run_group_message(&fixture, &storage, history) == H2_PAL_ERR_INVALID_STATE);
  fixture.friend_group_created = true;
  fixture.friend_group_name[0] = '\0';
  assert(h2_gizclaw_e2e_run_group_message(&fixture, &storage, history) == H2_PAL_ERR_INVALID_STATE);
  memset(fixture.friend_group_name, 'x', sizeof(fixture.friend_group_name));
  assert(h2_gizclaw_e2e_run_group_message(&fixture, &storage, history) == H2_PAL_ERR_INVALID_STATE);
  strcpy(fixture.friend_group_name, "group");
  fixture.actors[0].public_key[0] = '\0';
  assert(h2_gizclaw_e2e_run_group_message(&fixture, &storage, history) == H2_PAL_ERR_INVALID_STATE);
  memset(fixture.actors[0].public_key, 'x', sizeof(fixture.actors[0].public_key));
  assert(h2_gizclaw_e2e_run_group_message(&fixture, &storage, history) == H2_PAL_ERR_INVALID_STATE);
  assert(!state.stages && !state.budgets);
}

int main(int argc, char **argv) {
  if (argc == 6) {
    scenario((unsigned)atoi(argv[1]), (unsigned)atoi(argv[2]),
             (unsigned)atoi(argv[3]), (unsigned)atoi(argv[4]),
             (unsigned)atoi(argv[5]), true);
    return 0;
  }
  for (unsigned pagination = 0u; pagination <= 10u; ++pagination)
    scenario(0u, 0u, 0u, 0u, pagination, false);
  for (unsigned fail = 1u; fail <= 10u; ++fail)
    scenario(fail, 0u, 0u, 0u, 0u, false);
  for (unsigned budget = 1u; budget <= 4u; ++budget)
    scenario(0u, budget, 0u, 0u, 0u, false);
  for (unsigned response_index = 1u; response_index <= 4u; ++response_index)
    for (unsigned fault = 1u; fault <= (response_index % 2u ? 26u : 18u); ++fault)
      scenario(0u, 0u, fault, response_index, 0u, false);
  invalid_inputs();
  return 0;
}
