#include "h2_gizclaw_e2e_concurrency.h"
#include "h2_gizclaw_e2e_service.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  NORMAL,
  INLINE_CALLBACK,
  BAD_PROFILE,
  CREATE_ERROR,
  DO_ERROR,
  WAIT_ERROR,
  POLL_ERROR,
  HOLD_CALLBACK,
  PARSE_ERROR,
  SECOND_DO_ERROR,
  REPEAT_WAIT_ERROR,
  POLL_UNDERCOUNT,
  POLL_OVERCOUNT,
  DUPLICATE_CALLBACK,
  DUPLICATE_DO_ACCEPTED,
  UNTERMINATED_PROFILE,
  CANCEL_ERROR,
  CANCEL_REPEAT_ERROR,
  CANCEL_NO_EFFECT,
  CANCEL_WAIT_ERROR,
  CANCEL_PARSE_ERROR,
  CANCEL_DIRTY_OUTPUT,
  CANCEL_RESTART_ACCEPTED,
  SLEEP_ERROR,
  INITIAL_BUDGET_ERROR,
  CANCEL_BUDGET_ERROR,
  WAIT_BUDGET_ERROR,
  SECOND_WAIT_BUDGET_ERROR,
  NO_MEMORY
};

static unsigned s_mode, s_live, s_dos, s_accepted, s_waits;
static unsigned s_callbacks, s_polls, s_cancels, s_recovery;
static size_t s_open, s_maximum, s_unique;
static uint64_t s_now, s_deadline;
static bool s_concurrency;
static bool s_emit_evidence;
static int s_service;

struct h2_gizclaw_req {
  unsigned refs;
  bool registration, started, terminal, caller_released;
  int result;
};

static void *allocate(void *user, size_t size) {
  (void)user;
  if (s_mode == NO_MEMORY)
    return NULL;
  void *p = calloc(1, size);
  assert(p != NULL);
  ++s_live;
  return p;
}

static void release(void *user, void *p) {
  (void)user;
  if (p != NULL) {
    assert(s_live > 0u);
    --s_live;
    free(p);
  }
}

static void request_unref(h2_gizclaw_req_t *request) {
  if (request != NULL && --request->refs == 0u)
    release(NULL, request);
}

static int sleep_ms(void *user, uint32_t ms) {
  (void)user;
  if (s_mode == SLEEP_ERROR)
    return H2_PAL_ERR_IO;
  s_now += ms;
  return H2_PAL_OK;
}

bool h2_gizclaw_e2e_fixture_has_time(const h2_gizclaw_e2e_fixture_t *fixture,
                                     uint32_t ms) {
  assert(fixture != NULL);
  if (s_mode == INITIAL_BUDGET_ERROR ||
      (s_mode == CANCEL_BUDGET_ERROR && s_cancels != 0u) ||
      (s_mode == WAIT_BUDGET_ERROR && s_dos != 0u) ||
      (s_mode == SECOND_WAIT_BUDGET_ERROR && s_waits != 0u))
    return false;
  return s_now <= s_deadline && ms <= s_deadline - s_now;
}

void h2_gizclaw_e2e_evidence(const char *symbol, const char *stage, int rc) {
  assert(symbol != NULL && stage != NULL);
  if (s_emit_evidence)
    printf("H2_GIZCLAW_E2E symbol=%s stage=%s result=%s rc=%d\n", symbol, stage,
           rc == H2_PAL_OK ? "PASS" : "FAIL", rc);
}

void h2_gizclaw_e2e_fixture_reset_rpc_channel_observation(void) {
  s_open = s_maximum = s_unique = 0;
}

int h2_gizclaw_e2e_fixture_rpc_channel_observation(size_t *maximum,
                                                   size_t *unique,
                                                   size_t *open) {
  *maximum = s_maximum;
  *unique = s_unique;
  *open = s_open;
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_req_create_ping(h2_gizclaw_service_t *service,
                                           uint64_t identity, uint32_t timeout,
                                           h2_gizclaw_req_t **out) {
  assert(service == (h2_gizclaw_service_t *)&s_service && identity > 0u &&
         timeout > 0u);
  *out = NULL;
  if (s_mode == CREATE_ERROR)
    return H2_PAL_ERR_NO_MEMORY;
  *out = allocate(NULL, sizeof(**out));
  if (*out == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  (*out)->refs = 1u;
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_req_create_register(h2_gizclaw_service_t *service,
                                               uint64_t identity,
                                               const char *token,
                                               uint32_t timeout,
                                               h2_gizclaw_req_t **out) {
  assert(strcmp(token, "local-only-token") == 0);
  int rc = h2_gizclaw_req_create_ping(service, identity, timeout, out);
  if (rc == H2_PAL_OK)
    (*out)->registration = true;
  return rc;
}

h2_pal_result_t h2_gizclaw_req_do(h2_gizclaw_req_t *request,
                                  void *user,
                                  h2_gizclaw_req_input_read_fn input_read,
                                  h2_gizclaw_req_output_write_fn output_write,
                                  h2_gizclaw_req_complete_fn on_complete) {
  (void)on_complete;
  assert(user == NULL && input_read == NULL && output_write == NULL);
  if (request->started)
    return s_mode == DUPLICATE_DO_ACCEPTED ? H2_PAL_OK
                                           : H2_PAL_ERR_INVALID_STATE;
  if (request->terminal)
    return s_mode == CANCEL_RESTART_ACCEPTED ? H2_PAL_OK
                                             : H2_PAL_ERR_INVALID_STATE;
  ++s_dos;
  if (s_mode == DO_ERROR || (s_mode == SECOND_DO_ERROR && s_dos == 2u))
    return H2_PAL_ERR_NO_SPACE;
  request->started = true;
  ++s_accepted;
  if (s_concurrency) {
    ++s_open;
    ++s_unique;
    if (s_open > s_maximum)
      s_maximum = s_open;
  }
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_req_wait(h2_gizclaw_req_t *request,
                                    uint32_t timeout) {
  assert(timeout > 0u);
  if (!request->started) {
    assert(!request->registration);
    return s_mode == CANCEL_WAIT_ERROR ? H2_PAL_OK
           : request->terminal         ? request->result
                                       : H2_PAL_ERR_INVALID_STATE;
  }
  if (s_concurrency)
    assert(s_accepted == (s_mode == SECOND_DO_ERROR ? 1u : 3u));
  ++s_waits;
  if (!s_concurrency && s_mode == REPEAT_WAIT_ERROR && s_waits == 2u)
    return H2_PAL_ERR_TIMEOUT;
  s_now += 10;
  if (s_mode == HOLD_CALLBACK)
    s_deadline = s_now + 5u;
  if (!request->terminal && s_concurrency)
    --s_open;
  request->terminal = true;
  request->result = s_mode == WAIT_ERROR ? H2_PAL_ERR_TIMEOUT : H2_PAL_OK;
  return request->result;
}


h2_pal_result_t h2_gizclaw_req_cancel(h2_gizclaw_req_t *request) {
  ++s_cancels;
  if (!request->started) {
    if (s_mode == CANCEL_ERROR ||
        (s_mode == CANCEL_REPEAT_ERROR && s_cancels == 2u))
      return H2_PAL_ERR_IO;
    if (s_mode == CANCEL_NO_EFFECT)
      return H2_PAL_OK;
  }
  if (!request->terminal) {
    if (s_concurrency)
      --s_open;
    request->terminal = true;
    request->result = H2_PAL_ERR_CLOSED;
  }
  return H2_PAL_OK;
}

void h2_gizclaw_req_release(h2_gizclaw_req_t *request) {
  if (request != NULL) {
    assert(!request->caller_released);
    request->caller_released = true;
  }
  request_unref(request);
}

h2_pal_result_t
h2_gizclaw_resp_parse_register(const h2_gizclaw_req_t *request,
                               h2_gizclaw_registration_result_t *out) {
  assert(request->registration && request->terminal);
  assert(!request->caller_released);
  memset(out, 0, sizeof(*out));
  if (request->result != H2_PAL_OK)
    return request->result;
  if (s_mode == PARSE_ERROR)
    return H2_PAL_ERR_FORMAT;
  if (s_mode == UNTERMINATED_PROFILE) {
    memset(out->runtime_profile_name, 'x', sizeof(out->runtime_profile_name));
    return H2_PAL_OK;
  }
  strcpy(out->runtime_profile_name,
         s_mode == BAD_PROFILE ? "different" : "profile");
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_resp_parse_ping(const h2_gizclaw_req_t *request,
                                           h2_gizclaw_ping_result_t *out) {
  assert(!request->registration && request->terminal);
  if (!request->started) {
    memset(out, s_mode == CANCEL_DIRTY_OUTPUT ? 0xA5 : 0, sizeof(*out));
    return s_mode == CANCEL_PARSE_ERROR ? H2_PAL_OK : request->result;
  }
  *out = (h2_gizclaw_ping_result_t){.round_trip_ms = 10, .server_time_ms = 100};
  return s_mode == PARSE_ERROR ? H2_PAL_ERR_FORMAT : request->result;
}

h2_pal_result_t h2_gizclaw_rpc_ping(h2_gizclaw_service_t *service,
                                    uint32_t timeout,
                                    h2_gizclaw_ping_result_t *out) {
  assert(service == (h2_gizclaw_service_t *)&s_service && timeout > 0u);
  assert(s_live == 0u);
  ++s_recovery;
  *out = (h2_gizclaw_ping_result_t){.round_trip_ms = 10, .server_time_ms = 100};
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_service_poll(h2_gizclaw_service_t *service,
                                        size_t maximum, size_t *out_count) {
  assert(service == (h2_gizclaw_service_t *)&s_service && maximum > 0u);
  ++s_polls;
  *out_count = 0u;
  if (s_mode == POLL_ERROR)
    return H2_PAL_ERR_IO;
  if (s_mode == HOLD_CALLBACK || s_mode == SLEEP_ERROR)
    return H2_PAL_OK;
  return H2_PAL_OK;
}

static void reset(unsigned mode, bool concurrency) {
  assert(s_live == 0u);
  s_mode = mode;
  s_concurrency = concurrency;
  s_dos = s_accepted = s_waits = s_callbacks = s_polls =
      s_cancels = s_recovery = 0;
  s_now = 0u;
  s_deadline = 60000u;
}

int main(int argc, char **argv) {
  static const h2_pal_mem_vtable_t mem_vtable = {.alloc = allocate,
                                                 .free = release};
  static const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};
  static const h2_pal_time_vtable_t time_vtable = {.sleep_ms = sleep_ms};
  static const h2_pal_time_api_t time = {.vtable = &time_vtable};
  h2_gizclaw_e2e_fixture_t fixture = {
      .allocator = &mem,
      .time = &time,
      .registration_token = "local-only-token",
      .runtime_profile_name = "profile",
      .actors = {{.service = (h2_gizclaw_service_t *)&s_service}},
  };
  if (argc == 2 && strcmp(argv[1], "--emit-success-evidence") == 0) {
    reset(NORMAL, false);
    s_emit_evidence = true;
    puts("H2_GIZCLAW_E2E stage=coverage-begin case=service");
    assert(h2_gizclaw_e2e_run_service(&fixture) == H2_PAL_OK);
    assert(s_live == 0u);
    size_t dispatched = 0u;
    int poll_rc = h2_gizclaw_service_poll(fixture.actors[0].service, 1u,
                                          &dispatched);
    h2_gizclaw_e2e_evidence("h2_gizclaw_service_poll", "service", poll_rc);
    h2_gizclaw_e2e_evidence("h2_gizclaw_service_poll",
                            "service_poll-assert",
                            poll_rc == H2_PAL_OK && dispatched == 0u
                                ? H2_PAL_OK
                                : H2_PAL_ERR_INVALID_STATE);
    puts("H2_GIZCLAW_E2E stage=coverage-end case=service status=PASS rc=0 "
         "cleanup_rc=0");
    return 0;
  }
  static const unsigned base_modes[] = {NORMAL, BAD_PROFILE, CREATE_ERROR,
                                        DO_ERROR, WAIT_ERROR, PARSE_ERROR};
  static const int expected[] = {H2_PAL_OK,
                                 H2_PAL_ERR_INVALID_STATE,
                                 H2_PAL_ERR_NO_MEMORY,
                                 H2_PAL_ERR_NO_SPACE,
                                 H2_PAL_ERR_TIMEOUT,
                                 H2_PAL_ERR_FORMAT};
  for (unsigned index = 0u; index < sizeof(expected) / sizeof(expected[0]);
       ++index) {
    const unsigned mode = base_modes[index];
    reset(mode, false);
    int rc = h2_gizclaw_e2e_run_service(&fixture);
    if (rc != expected[index])
      fprintf(stderr, "base service mode=%u expected=%d actual=%d\n", mode,
              expected[index], rc);
    assert(rc == expected[index]);
    if (mode == NORMAL)
      assert(s_waits == 2u && s_polls == 0u && s_cancels == 2u);
    assert(s_live == 0u);
  }
  const unsigned service_modes[] = {
      REPEAT_WAIT_ERROR,     DUPLICATE_DO_ACCEPTED, UNTERMINATED_PROFILE,
      CANCEL_ERROR,
      CANCEL_REPEAT_ERROR,   CANCEL_NO_EFFECT,         CANCEL_WAIT_ERROR,
      CANCEL_PARSE_ERROR,    CANCEL_DIRTY_OUTPUT,      CANCEL_RESTART_ACCEPTED,
      INITIAL_BUDGET_ERROR,  WAIT_BUDGET_ERROR, SECOND_WAIT_BUDGET_ERROR,
      NO_MEMORY};
  const int service_results[] = {H2_PAL_ERR_TIMEOUT,
                                 H2_PAL_ERR_INVALID_STATE,
                                 H2_PAL_ERR_INVALID_STATE,
                                 H2_PAL_ERR_IO,
                                 H2_PAL_ERR_IO,
                                 H2_PAL_ERR_INVALID_STATE,
                                 H2_PAL_ERR_INVALID_STATE,
                                 H2_PAL_ERR_INVALID_STATE,
                                 H2_PAL_ERR_INVALID_STATE,
                                 H2_PAL_ERR_INVALID_STATE,
                                 H2_PAL_ERR_TIMEOUT,
                                 H2_PAL_ERR_TIMEOUT,
                                 H2_PAL_ERR_TIMEOUT,
                                 H2_PAL_ERR_NO_MEMORY};
  for (size_t i = 0; i < sizeof(service_modes) / sizeof(service_modes[0]);
       ++i) {
    reset(service_modes[i], false);
    int rc = h2_gizclaw_e2e_run_service(&fixture);
    if (rc != service_results[i])
      fprintf(stderr, "service mode=%u expected=%d actual=%d\n",
              service_modes[i], service_results[i], rc);
    assert(rc == service_results[i]);
    assert(s_live == 0u);
  }
  reset(NORMAL, false);
  assert(h2_gizclaw_e2e_run_service(NULL) == H2_PAL_ERR_INVALID_ARG);
  fixture.runtime_profile_name[0] = '\0';
  assert(h2_gizclaw_e2e_run_service(&fixture) == H2_PAL_ERR_INVALID_ARG);
  memset(fixture.runtime_profile_name, 'x',
         sizeof(fixture.runtime_profile_name));
  assert(h2_gizclaw_e2e_run_service(&fixture) == H2_PAL_ERR_INVALID_ARG);
  strcpy(fixture.runtime_profile_name, "profile");
  assert(s_live == 0u && s_dos == 0u);
  unsigned modes[] = {NORMAL,          CREATE_ERROR, DO_ERROR,
                      SECOND_DO_ERROR, PARSE_ERROR,  WAIT_ERROR};
  int results[] = {H2_PAL_OK,           H2_PAL_ERR_NO_MEMORY,
                   H2_PAL_ERR_NO_SPACE, H2_PAL_ERR_NO_SPACE,
                   H2_PAL_ERR_FORMAT,   H2_PAL_ERR_TIMEOUT};
  for (size_t i = 0u; i < sizeof(modes) / sizeof(modes[0]); ++i) {
    reset(modes[i], true);
    assert(h2_gizclaw_e2e_run_concurrency(&fixture) == results[i]);
    assert(s_live == 0u && s_polls == 0u && s_open == 0u);
    assert(s_recovery == 1u);
    if (modes[i] == NORMAL)
      assert(s_accepted == 3u && s_waits == 3u && s_maximum == 3u &&
             s_unique == 3u);
  }
  return 0;
}
