#include "h2_gizclaw_e2e_group_audio.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct h2_gizclaw_req {
  unsigned phase;
};
static struct {
  h2_gizclaw_e2e_fixture_t *fixture;
  h2_gizclaw_req_t request;
  h2_gizclaw_friend_group_message_audio_write_fn sink[2];
  h2_gizclaw_req_output_write_fn output[2];
  void *user[2];
  unsigned stages, fail, budgets, budget_fail, fault, fault_api;
  unsigned cancels, releases, proofs;
  bool emit;
} state;

h2_gizclaw_str_t h2_gizclaw_e2e_str(const char *s) {
  return (h2_gizclaw_str_t){s, strlen(s)};
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
  return ++state.budgets != state.budget_fail;
}
void h2_gizclaw_e2e_evidence(const char *symbol, const char *stage, int rc) {
  if (strstr(stage, "-assert") && rc == H2_PAL_OK)
    ++state.proofs;
  if (state.emit)
    printf("H2_GIZCLAW_E2E symbol=%s stage=%s result=%s rc=%d\n", symbol, stage,
           rc == H2_PAL_OK ? "PASS" : "FAIL", rc);
}
static int step(void) {
  return ++state.stages == state.fail ? H2_PAL_ERR_IO : H2_PAL_OK;
}
static void arguments(h2_gizclaw_service_t *service, h2_gizclaw_str_t group,
                      h2_gizclaw_str_t history, uint32_t timeout,
                      h2_gizclaw_friend_group_message_audio_write_fn sink,
                      void *user, unsigned api) {
  assert(service == state.fixture->actors[0].service && timeout == 30000u);
  assert(group.len == 5u && memcmp(group.data, "group", 5u) == 0);
  assert(history.len == 7u && memcmp(history.data, "history", 7u) == 0);
  assert(sink && user == &state.fixture->group_audio_bytes[api]);
  assert(atomic_load(&state.fixture->group_audio_bytes[api]) == 0u);
  state.sink[api] = sink;
  state.user[api] = user;
}
static char *save(h2_gizclaw_resp_storage_t *s, const char *value) {
  size_t len = strlen(value) + 1u;
  assert(len <= s->capacity - s->used);
  char *out = (char *)s->data + s->used;
  memcpy(out, value, len);
  s->used += len;
  return out;
}
static int deliver(unsigned api, const uint8_t *data, size_t length) {
  if (api != 0u)
    return state.sink[api](state.user[api], data, length);
  size_t written = 0u;
  int rc = state.output[api](state.user[api], data, length, &written);
  assert(rc != H2_PAL_OK || written == length);
  return rc;
}
static int payload(unsigned api) {
  const uint8_t data[] = {1, 2, 3, 4, 5};
  unsigned fault = api == state.fault_api ? state.fault : 0u;
  if (fault == 12u)
    return H2_PAL_OK; /* Metadata without actual bytes must fail. */
  if (fault == 13u)
    return deliver(api, NULL, sizeof(data));
  if (fault == 14u) {
    atomic_store(&state.fixture->group_audio_bytes[api], SIZE_MAX - 1u);
    int rc = deliver(api, data, 2u);
    assert(atomic_load(&state.fixture->group_audio_bytes[api]) ==
           SIZE_MAX - 1u);
    return rc;
  }
  int rc = deliver(api, data, 2u);
  return rc == H2_PAL_OK ? deliver(api, data + 2u, 3u) : rc;
}
static void response(unsigned api, h2_gizclaw_resp_storage_t *s,
                     h2_gizclaw_friend_group_message_audio_info_t *out) {
  assert(s->used == 0u);
  memset(s->data, 0xa5, s->capacity);
  *out = (h2_gizclaw_friend_group_message_audio_info_t){
      .friend_group_name = save(s, "group"),
      .history_id = save(s, "history"),
      .mime_type = save(s, "audio/ogg"),
      .received_bytes = 5u,
      .size_bytes = 5u};
  switch (api == state.fault_api ? state.fault : 0u) {
  case 1:
    out->friend_group_name = save(s, "wrong");
    break;
  case 2:
    out->history_id = save(s, "wrong");
    break;
  case 3:
    out->mime_type = save(s, "");
    break;
  case 4:
    out->friend_group_name = NULL;
    break;
  case 5:
    out->history_id = NULL;
    break;
  case 6:
    out->mime_type = NULL;
    break;
  case 7:
    out->history_id = "history";
    break;
  case 8:
    out->history_id = (char *)s->data + s->capacity - 7u;
    memcpy(out->history_id, "history", 7u);
    s->used = s->capacity;
    break;
  case 9:
    s->used = s->capacity + 1u;
    break;
  case 10:
    out->received_bytes = 4u;
    break;
  case 11:
    out->size_bytes = 6u;
    break;
  }
}
h2_pal_result_t h2_gizclaw_req_create_friend_group_message_audio_download(
    h2_gizclaw_service_t *service, uint64_t id, h2_gizclaw_str_t group,
    h2_gizclaw_str_t history, uint32_t timeout, h2_gizclaw_req_t **out) {
  assert(id == 401u);
  int rc = step();
  if (rc == H2_PAL_OK) {
    assert(service == state.fixture->actors[0].service && timeout == 30000u);
    assert(group.len == 5u && memcmp(group.data, "group", 5u) == 0);
    assert(history.len == 7u && memcmp(history.data, "history", 7u) == 0);
    *out = &state.request;
    state.request.phase = 1u;
  }
  return rc;
}
h2_pal_result_t h2_gizclaw_req_do(h2_gizclaw_req_t *request,
                                  void *user,
                                  h2_gizclaw_req_input_read_fn input_read,
                                  h2_gizclaw_req_output_write_fn output_write,
                                  h2_gizclaw_req_complete_fn on_complete) {
  (void)on_complete;
  assert(request == &state.request && request->phase == 1u && user &&
         !input_read && output_write);
  state.user[0] = user;
  state.output[0] = output_write;
  request->phase = 2u;
  return step();
}
h2_pal_result_t h2_gizclaw_req_wait(h2_gizclaw_req_t *request,
                                    uint32_t timeout) {
  assert(request == &state.request && request->phase == 2u &&
         (timeout == 1u || timeout == 30000u));
  request->phase = 3u;
  int rc = step();
  return rc == H2_PAL_OK ? payload(0u) : rc;
}
h2_pal_result_t h2_gizclaw_service_poll(h2_gizclaw_service_t *service,
                                        size_t max_events,
                                        size_t *out_dispatched) {
  assert(service == state.fixture->actors[0].service && max_events == 8u &&
         out_dispatched != NULL);
  *out_dispatched = 0u;
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_resp_parse_friend_group_message_audio_download(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_message_audio_info_t *out) {
  assert(request == &state.request && request->phase == 3u);
  state.request.phase = 4u;
  int rc = step();
  if (rc == H2_PAL_OK)
    response(0u, storage, out);
  return rc;
}
h2_pal_result_t h2_gizclaw_req_cancel(h2_gizclaw_req_t *request) {
  assert(request == &state.request && request->phase < 5u);
  ++state.cancels;
  return H2_PAL_ERR_BUSY; /* A failed cancel must not mask the original failure.
                           */
}
void h2_gizclaw_req_release(h2_gizclaw_req_t *request) {
  assert(request == &state.request && request->phase < 5u);
  request->phase = 5u;
  ++state.releases;
}
h2_pal_result_t h2_gizclaw_rpc_friend_group_message_audio_download(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t group,
    h2_gizclaw_str_t history,
    h2_gizclaw_friend_group_message_audio_write_fn sink, void *user,
    uint32_t timeout, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_friend_group_message_audio_info_t *out) {
  assert(state.releases == 1u && state.proofs == 1u);
  arguments(service, group, history, timeout, sink, user, 1u);
  int rc = step();
  if (rc == H2_PAL_OK)
    rc = payload(1u);
  if (rc == H2_PAL_OK)
    response(1u, storage, out);
  return rc;
}

static int scenario(unsigned fail, unsigned budget, unsigned fault,
                    unsigned api, bool emit) {
  memset(&state, 0, sizeof(state));
  state.fail = fail;
  state.budget_fail = budget;
  state.fault = fault;
  state.fault_api = api;
  state.emit = emit;
  state.fixture = calloc(1u, sizeof(*state.fixture));
  assert(state.fixture);
  state.fixture->actors[0].service = (h2_gizclaw_service_t *)&state.request;
  strcpy(state.fixture->friend_group_name, "group");
  uint8_t arena[256];
  h2_gizclaw_resp_storage_t storage = {.data = arena,
                                       .capacity = sizeof(arena)};
  if (emit) {
    puts("H2_GIZCLAW_E2E case=rpc stage=coverage-begin");
    puts("H2_GIZCLAW_E2E case=rpc/group stage=coverage-begin");
  }
  int rc = h2_gizclaw_e2e_run_group_audio(state.fixture, &storage,
                                          h2_gizclaw_e2e_str("history"));
  bool success = !fail && !budget && !fault;
  assert((rc == H2_PAL_OK) == success && storage.used == 0u);
  if (fail)
    assert(rc == H2_PAL_ERR_IO && state.stages == fail);
  if (budget)
    assert(rc == H2_PAL_ERR_TIMEOUT && state.budgets == budget);
  if (success)
    assert(state.stages == 5u && state.proofs == 2u);
  assert(state.releases == (state.output[0] ? 1u : 0u));
  unsigned expected_cancel =
      (fail >= 2u && fail <= 4u) || (fault >= 13u && api == 0u);
  assert(state.cancels == expected_cancel);
  if (emit) {
    printf("H2_GIZCLAW_E2E case=rpc/group stage=coverage-end status=%s rc=%d "
           "cleanup_rc=0\n",
           rc ? "FAIL" : "PASS", rc);
    printf("H2_GIZCLAW_E2E case=rpc stage=coverage-end status=%s rc=%d "
           "cleanup_rc=0\n",
           rc ? "FAIL" : "PASS", rc);
  }
  /* Failed waits can leave Service holding the sink. Invoke the captured sink
   * after run_group_audio returned and released its request reference. */
  const uint8_t byte = 1u;
  for (unsigned i = 0u; i < 2u; ++i) {
    if (!state.sink[i])
      continue;
    size_t before = atomic_load(&state.fixture->group_audio_bytes[i]);
    assert(state.sink[i](state.user[i], &byte, 1u) == H2_PAL_OK);
    assert(atomic_load(&state.fixture->group_audio_bytes[i]) == before + 1u);
    assert(state.sink[i](NULL, &byte, 1u) == H2_PAL_ERR_INVALID_ARG);
    assert(state.sink[i](state.user[i], &byte, 0u) == H2_PAL_ERR_INVALID_ARG);
  }
  unsigned before = state.stages;
  assert(h2_gizclaw_e2e_run_group_audio(state.fixture, &storage,
                                        h2_gizclaw_e2e_str("history")) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(state.stages == before);
  free(state.fixture);
  return rc;
}

static void invalid_inputs(void) {
  memset(&state, 0, sizeof(state));
  h2_gizclaw_e2e_fixture_t fixture = {0};
  fixture.actors[0].service = (h2_gizclaw_service_t *)&state.request;
  strcpy(fixture.friend_group_name, "group");
  uint8_t arena[64];
  h2_gizclaw_resp_storage_t storage = {.data = arena,
                                       .capacity = sizeof(arena)};
  h2_gizclaw_str_t history = h2_gizclaw_e2e_str("history");
  assert(h2_gizclaw_e2e_run_group_audio(NULL, &storage, history) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_e2e_run_group_audio(&fixture, NULL, history) ==
         H2_PAL_ERR_INVALID_ARG);
  h2_gizclaw_resp_storage_t invalid = {.capacity = sizeof(arena)};
  assert(h2_gizclaw_e2e_run_group_audio(&fixture, &invalid, history) ==
         H2_PAL_ERR_INVALID_ARG);
  invalid.data = arena;
  invalid.capacity = 0u;
  assert(h2_gizclaw_e2e_run_group_audio(&fixture, &invalid, history) ==
         H2_PAL_ERR_INVALID_ARG);
  const h2_gizclaw_str_t bad[] = {
      {NULL, 7u}, {"", 0u}, {"a\0b", 3u}, {"", SIZE_MAX}};
  for (size_t i = 0u; i < sizeof(bad) / sizeof(*bad); ++i)
    assert(h2_gizclaw_e2e_run_group_audio(&fixture, &storage, bad[i]) ==
           H2_PAL_ERR_INVALID_ARG);
  fixture.actors[0].service = NULL;
  assert(h2_gizclaw_e2e_run_group_audio(&fixture, &storage, history) ==
         H2_PAL_ERR_INVALID_ARG);
  fixture.actors[0].service = (h2_gizclaw_service_t *)&state.request;
  fixture.friend_group_name[0] = '\0';
  assert(h2_gizclaw_e2e_run_group_audio(&fixture, &storage, history) ==
         H2_PAL_ERR_INVALID_STATE);
  memset(fixture.friend_group_name, 'x', sizeof(fixture.friend_group_name));
  assert(h2_gizclaw_e2e_run_group_audio(&fixture, &storage, history) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(!fixture.group_audio_started && state.stages == 0u);
}

int main(int argc, char **argv) {
  if (argc == 5) {
    (void)scenario((unsigned)atoi(argv[1]), (unsigned)atoi(argv[2]),
                   (unsigned)atoi(argv[3]), (unsigned)atoi(argv[4]), true);
    return 0;
  }
  invalid_inputs();
  (void)scenario(0, 0, 0, 0, false);
  for (unsigned fail = 1u; fail <= 5u; ++fail)
    (void)scenario(fail, 0, 0, 0, false);
  for (unsigned budget = 1u; budget <= 2u; ++budget)
    (void)scenario(0, budget, 0, 0, false);
  for (unsigned api = 0u; api < 2u; ++api)
    for (unsigned fault = 1u; fault <= 14u; ++fault)
      (void)scenario(0, 0, fault, api, false);
  return 0;
}
