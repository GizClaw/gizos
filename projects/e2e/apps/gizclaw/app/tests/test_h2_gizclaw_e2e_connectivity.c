#include "h2_gizclaw_e2e_rpc.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum method { PING, SPEED, REGISTER, DELETE };
static int s_services[2];
static unsigned s_stage, s_fail_stage, s_mode, s_live, s_clock_calls;
static unsigned s_budget_calls, s_fail_budget, s_fail_clock;
static unsigned s_speed[2], s_deleted[2];
static bool s_id_used[64], s_emit;
static uint64_t s_now;
static h2_gizclaw_req_t *s_pending;
static h2_gizclaw_req_t *s_tail;
static h2_gizclaw_req_t *s_late;
static bool s_late_injected;
struct h2_gizclaw_req {
  enum method method;
  bool started, waited;
  size_t upload, download;
  h2_gizclaw_req_input_read_fn input_read;
  h2_gizclaw_req_output_write_fn output_write;
  void *user;
  size_t hook_bytes;
  bool released, canceled;
  bool hook_pending;
  h2_gizclaw_req_t *next;
};

static int stage(void) {
  return ++s_stage == s_fail_stage ? H2_PAL_ERR_IO : H2_PAL_OK;
}
static void service_check(h2_gizclaw_service_t *service, enum method method,
                          unsigned api) {
  assert(service == (h2_gizclaw_service_t
                         *)&s_services[method == DELETE && api == 1u ? 1 : 0]);
}
static int create(h2_gizclaw_service_t *service, uint64_t identity,
                  uint32_t timeout, enum method method,
                  h2_gizclaw_req_t **out) {
  service_check(service, method, 0u);
  assert(timeout == 30000u && identity < 64u && !s_id_used[identity]);
  s_id_used[identity] = true;
  *out = NULL;
  int rc = stage();
  if (rc != H2_PAL_OK)
    return rc;
  *out = calloc(1u, sizeof(**out));
  assert(*out != NULL);
  (*out)->method = method;
  ++s_live;
  return H2_PAL_OK;
}
h2_pal_result_t h2_gizclaw_req_create_ping(h2_gizclaw_service_t *s, uint64_t id,
                                           uint32_t timeout,
                                           h2_gizclaw_req_t **out) {
  return create(s, id, timeout, PING, out);
}
h2_pal_result_t h2_gizclaw_req_create_register(h2_gizclaw_service_t *s,
                                               uint64_t id, const char *token,
                                               uint32_t timeout,
                                               h2_gizclaw_req_t **out) {
  assert(strcmp(token, "synthetic-registration") == 0);
  return create(s, id, timeout, REGISTER, out);
}
h2_pal_result_t h2_gizclaw_req_create_peer_delete(h2_gizclaw_service_t *s,
                                                  uint64_t id, uint32_t timeout,
                                                  h2_gizclaw_req_t **out) {
  assert(s_speed[0] == 6u && s_speed[1] == 6u);
  return create(s, id, timeout, DELETE, out);
}
h2_pal_result_t h2_gizclaw_req_create_speedtest(h2_gizclaw_service_t *s,
                                                uint64_t id, size_t upload,
                                                size_t download,
                                                uint32_t timeout,
                                                h2_gizclaw_req_t **out) {
  assert((upload == 0u) != (download == 0u));
  assert(upload + download == 2u * 1024u * 1024u);
  ++s_speed[0];
  int rc = create(s, id, timeout, SPEED, out);
  if (rc == H2_PAL_OK) {
    (*out)->upload = upload;
    (*out)->download = download;
  }
  return rc;
}
h2_pal_result_t h2_gizclaw_req_do(h2_gizclaw_req_t *req,
                                  void *user,
                                  h2_gizclaw_req_input_read_fn input_read,
                                  h2_gizclaw_req_output_write_fn output_write,
                                  h2_gizclaw_req_complete_fn on_complete) {
  (void)on_complete;
  assert(req != NULL && !req->started);
  assert((user != NULL) == (req->method == SPEED));
  assert((input_read != NULL) == (req->upload != 0u));
  assert(output_write == NULL);
  int rc = stage();
  req->started = rc == H2_PAL_OK;
  if (rc == H2_PAL_OK && req->method == SPEED) {
    req->input_read = input_read;
    req->output_write = output_write;
    req->user = user;
    req->hook_pending = true;
    if (s_tail != NULL)
      s_tail->next = req;
    else
      s_pending = req;
    s_tail = req;
  }
  return rc;
}
h2_pal_result_t h2_gizclaw_req_wait(h2_gizclaw_req_t *req, uint32_t timeout) {
  assert(req->started && (timeout == 1u || timeout == 30000u));
  if (req->method == SPEED && req->hook_pending)
    return H2_PAL_ERR_TIMEOUT;
  if (req->waited)
    return H2_PAL_OK;
  int rc = stage();
  s_now += 100u;
  if (rc == H2_PAL_OK && req->method == SPEED &&
      (s_mode == 11u || s_mode == 12u))
    rc = H2_PAL_ERR_TIMEOUT;
  req->waited = rc == H2_PAL_OK;
  return rc;
}
h2_pal_result_t h2_gizclaw_req_cancel(h2_gizclaw_req_t *req) {
  assert(req != NULL);
  req->canceled = true;
  return s_mode == 12u ? H2_PAL_ERR_IO : H2_PAL_OK;
}
void h2_gizclaw_req_release(h2_gizclaw_req_t *req) {
  if (req != NULL) {
    if (req->hook_pending) {
      req->released = true;
      return;
    }
    assert(s_live > 0u);
    --s_live;
    memset(req, 0xa5, sizeof(*req));
    free(req);
  }
}

h2_pal_result_t h2_gizclaw_service_poll(h2_gizclaw_service_t *service,
                                        size_t budget, size_t *dispatched) {
  service_check(service, SPEED, 0u);
  assert(budget == 8u);
  *dispatched = 0u;
  if (s_mode == 11u || s_mode == 12u || s_mode == 23u)
    return H2_PAL_OK;
  while (s_pending != NULL && *dispatched < budget) {
    h2_gizclaw_req_t *req = s_pending;
    uint8_t payload[32768];
    for (size_t i = 0u; i < sizeof(payload); ++i)
      payload[i] = (uint8_t)(req->hook_bytes + i);
    if (!req->canceled && req->hook_bytes < req->upload) {
      size_t read = 0u;
      const int rc = req->input_read(req->user, payload, sizeof(payload), &read);
      assert(rc == H2_PAL_OK && read <= sizeof(payload));
      req->hook_bytes += read;
    } else if (!req->canceled && req->hook_bytes < req->download) {
      size_t count = req->download - req->hook_bytes;
      if (count > sizeof(payload))
        count = sizeof(payload);
      req->hook_bytes += count;
    }
    ++*dispatched;
    if (req->canceled || req->hook_bytes == req->upload + req->download) {
      s_pending = req->next;
      if (s_pending == NULL)
        s_tail = NULL;
      req->hook_pending = false;
      if (req->released)
        h2_gizclaw_req_release(req);
    }
  }
  return H2_PAL_OK;
}
static void registration(h2_gizclaw_registration_result_t *out, unsigned api) {
  memset(out, 0, sizeof(*out));
  strcpy(out->runtime_profile_name, "default");
  if ((s_mode == 1u && api == 0u) || (s_mode == 2u && api == 1u))
    strcpy(out->runtime_profile_name, "wrong-profile");
  if (s_mode == 3u)
    memset(out->runtime_profile_name, 'x', sizeof(out->runtime_profile_name));
  if (s_mode == 4u)
    out->runtime_profile_name[0] = '\0';
}
static void ping(h2_gizclaw_ping_result_t *out) {
  *out = (h2_gizclaw_ping_result_t){.round_trip_ms = 0u,
                                    .server_time_ms = s_mode == 5u ? 0 : 1000};
}
static void speed(size_t upload, size_t download,
                  h2_gizclaw_speedtest_result_t *out) {
  *out = (h2_gizclaw_speedtest_result_t){
      .upload_bytes = upload,
      .download_bytes = download,
      .upload_elapsed_ms = upload ? 100u : 0u,
      .elapsed_ms = download ? 100u : 0u,
      .upload_bits_per_second = upload * 80u,
      .download_bits_per_second = download * 80u};
  if (s_mode == 6u)
    ++out->download_bytes;
  if (s_mode == 7u)
    out->elapsed_ms = out->upload_elapsed_ms = 0u;
  if (s_mode == 8u) {
    ++out->upload_bits_per_second;
    ++out->download_bits_per_second;
  }
}
h2_pal_result_t
h2_gizclaw_resp_parse_register(const h2_gizclaw_req_t *req,
                               h2_gizclaw_registration_result_t *out) {
  assert(req->method == REGISTER && req->waited);
  int rc = stage();
  if (rc == H2_PAL_OK)
    registration(out, 0u);
  return rc;
}
h2_pal_result_t h2_gizclaw_resp_parse_ping(const h2_gizclaw_req_t *req,
                                           h2_gizclaw_ping_result_t *out) {
  assert(req->method == PING && req->waited);
  int rc = stage();
  if (rc == H2_PAL_OK)
    ping(out);
  return rc;
}
h2_pal_result_t
h2_gizclaw_resp_parse_speedtest(const h2_gizclaw_req_t *req,
                                h2_gizclaw_speedtest_result_t *out) {
  assert(req->method == SPEED && req->waited);
  int rc = stage();
  if (rc == H2_PAL_OK)
    speed(req->upload, req->download, out);
  return rc;
}
h2_pal_result_t h2_gizclaw_resp_parse_peer_delete(const h2_gizclaw_req_t *req) {
  assert(req->method == DELETE && req->waited);
  int rc = stage();
  if (rc == H2_PAL_OK)
    ++s_deleted[0];
  return rc;
}
h2_pal_result_t h2_gizclaw_rpc_register(h2_gizclaw_service_t *s,
                                        const char *token, uint32_t timeout,
                                        h2_gizclaw_registration_result_t *out) {
  service_check(s, REGISTER, 1u);
  assert(strcmp(token, "synthetic-registration") == 0 && timeout == 30000u);
  int rc = stage();
  if (rc == H2_PAL_OK)
    registration(out, 1u);
  return rc;
}
h2_pal_result_t h2_gizclaw_rpc_ping(h2_gizclaw_service_t *s, uint32_t timeout,
                                    h2_gizclaw_ping_result_t *out) {
  service_check(s, PING, 1u);
  assert(timeout == 30000u);
  int rc = stage();
  if (rc == H2_PAL_OK)
    ping(out);
  return rc;
}
h2_pal_result_t h2_gizclaw_rpc_speedtest(h2_gizclaw_service_t *s, size_t upload,
                                         size_t download, uint32_t timeout,
                                         h2_gizclaw_speedtest_result_t *out) {
  service_check(s, SPEED, 1u);
  assert(timeout == 30000u && ((upload == 0u) != (download == 0u)));
  assert(upload + download == 2u * 1024u * 1024u);
  ++s_speed[1];
  int rc = stage();
  s_now += 100u;
  if (rc == H2_PAL_OK)
    speed(upload, download, out);
  return rc;
}
h2_pal_result_t h2_gizclaw_rpc_peer_delete(h2_gizclaw_service_t *s,
                                           uint32_t timeout) {
  service_check(s, DELETE, 1u);
  assert(timeout == 30000u && s_deleted[0] == 1u);
  int rc = stage();
  if (rc == H2_PAL_OK)
    ++s_deleted[1];
  return rc;
}
void h2_gizclaw_e2e_evidence(const char *symbol, const char *where, int rc) {
  if (s_emit)
    printf("H2_GIZCLAW_E2E symbol=%s stage=%s result=%s rc=%d\n", symbol, where,
           rc == H2_PAL_OK ? "PASS" : "FAIL", rc);
}
bool h2_gizclaw_e2e_fixture_has_time(const h2_gizclaw_e2e_fixture_t *fixture,
                                     uint32_t ms) {
  ++s_budget_calls;
  return s_mode != 10u && s_budget_calls != s_fail_budget &&
         s_now + ms <= fixture->deadline_ms;
}
static int clock_now(void *user, uint64_t *out) {
  (void)user;
  ++s_clock_calls;
  if (s_clock_calls == s_fail_clock)
    return H2_PAL_ERR_IO;
  *out = s_mode == 9u && s_clock_calls % 2u == 0u ? s_now - 200u : s_now;
  return H2_PAL_OK;
}
static int sleep_ms(void *user, uint32_t ms) {
  (void)user;
  s_now += ms;
  return H2_PAL_OK;
}
static void run(unsigned fail_stage, unsigned mode, unsigned fail_budget,
                unsigned fail_clock) {
  assert(s_live == 0u);
  s_stage = s_clock_calls = s_budget_calls = 0u;
  s_fail_budget = fail_budget;
  s_fail_clock = fail_clock;
  s_fail_stage = fail_stage;
  s_mode = mode;
  s_late_injected = false;
  s_now = 1000u;
  memset(s_speed, 0, sizeof(s_speed));
  memset(s_deleted, 0, sizeof(s_deleted));
  memset(s_id_used, 0, sizeof(s_id_used));
  const h2_pal_time_vtable_t vtable = {
      .get_monotonic_ms = clock_now, .sleep_ms = sleep_ms};
  const h2_pal_time_api_t time = {.vtable = &vtable};
  h2_gizclaw_e2e_fixture_t *fixture = calloc(1u, sizeof(*fixture));
  assert(fixture != NULL);
  fixture->time = &time;
  fixture->registration_token = "synthetic-registration";
  fixture->deadline_ms = 600000u;
  strcpy(fixture->runtime_profile_name, "default");
  strcpy(fixture->endpoint, "example.invalid:9821");
  for (unsigned i = 0u; i < 2u; ++i) {
    fixture->actors[i].service = (h2_gizclaw_service_t *)&s_services[i];
    fixture->actors[i].registered = true;
    fixture->actors[i].peer_delete_required = true;
  }
  if (s_emit)
    printf("H2_GIZCLAW_E2E stage=coverage-begin case=connectivity\n");
  int rc = h2_gizclaw_e2e_run_connectivity(fixture);
  int expected = fail_stage || fail_clock   ? H2_PAL_ERR_IO
                 : fail_budget              ? H2_PAL_ERR_TIMEOUT
                 : mode >= 1u && mode <= 5u ? H2_PAL_ERR_FORMAT
                 : mode >= 6u && mode <= 9u ? H2_PAL_ERR_INVALID_STATE
                 : mode >= 10u              ? H2_PAL_ERR_TIMEOUT
                                            : H2_PAL_OK;
  if (rc != expected)
    fprintf(stderr, "fail=%u mode=%u budget=%u clock=%u rc=%d expected=%d\n",
            fail_stage, mode, fail_budget, fail_clock, rc, expected);
  assert(rc == expected);
  /* A failed call releases its caller reference, but the fixture must remain
   * alive for queued callbacks delivered during later cleanup. */
  if (s_pending != NULL) {
    assert(s_pending->released);
    s_mode = 0u;
    size_t dispatched = 0u;
    assert(h2_gizclaw_service_poll(fixture->actors[0].service, 8u,
                                   &dispatched) == H2_PAL_OK);
  }
  assert(s_live == 0u && s_pending == NULL && s_late == NULL);
  for (unsigned i = 0u; i < 2u; ++i) {
    assert(fixture->actors[i].peer_delete_required == (s_deleted[i] == 0u));
    assert(fixture->actors[i].registered == (s_deleted[i] == 0u));
    assert(fixture->actors[i].peer_delete_requested == (s_deleted[i] != 0u));
  }
  if (expected == H2_PAL_OK)
    assert(s_stage == 45u && s_deleted[0] == 1u && s_deleted[1] == 1u &&
           s_speed[0] == 6u && s_speed[1] == 6u);
  if (s_emit)
    printf("H2_GIZCLAW_E2E stage=coverage-end case=connectivity status=%s "
           "rc=%d cleanup_rc=0\n",
           rc == H2_PAL_OK ? "PASS" : "FAIL", rc);
  free(fixture);
}
int main(int argc, char **argv) {
  if (argc == 6 && strcmp(argv[1], "--emit-evidence") == 0) {
    unsigned fail = (unsigned)atoi(argv[2]), mode = (unsigned)atoi(argv[3]);
    unsigned budget = (unsigned)atoi(argv[4]), clock = (unsigned)atoi(argv[5]);
    assert(fail <= 45u && mode <= 12u && budget <= 18u && clock <= 24u);
    s_emit = true;
    run(fail, mode, budget, clock);
    return 0;
  }
  run(0u, 0u, 0u, 0u);
  for (unsigned stage = 1u; stage <= 45u; ++stage)
    run(stage, 0u, 0u, 0u);
  for (unsigned mode = 1u; mode <= 12u; ++mode)
    run(0u, mode, 0u, 0u);
  for (unsigned budget = 1u; budget <= 18u; ++budget)
    run(0u, 0u, budget, 0u);
  for (unsigned clock = 1u; clock <= 24u; ++clock)
    run(0u, 0u, 0u, clock);
  return 0;
}
