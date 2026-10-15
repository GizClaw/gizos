#include "h2_gizclaw_e2e_debug.h"
#include "h2_app_test_time.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

enum { NORMAL, WRITE_NOT_PERSISTED, STALE_SNAPSHOT, UNKNOWN_SNAPSHOT,
       ASYNC_FAILED, NEVER_FINISH, READ_WRONG_MODE, API_ERROR, BUDGET_EXPIRED };
static unsigned mode, calls, fail_at;
static char durable[64], desired[64];
static bool alive, async_pending;
static unsigned busy_reads;
static h2_gizclaw_debug_snapshot_t snapshot;
static h2_app_test_time_t clock;
static int service;
struct h2_gizclaw_req { bool set, started, done; } request;
static int step(void) { return ++calls == fail_at ? H2_PAL_ERR_IO : H2_PAL_OK; }
h2_gizclaw_str_t h2_gizclaw_e2e_str(const char *s) { return (h2_gizclaw_str_t){s, strlen(s)}; }
bool h2_gizclaw_e2e_fixture_has_time(const h2_gizclaw_e2e_fixture_t *f, uint32_t n) {
  assert(f && n); return mode != BUDGET_EXPIRED;
}
void h2_gizclaw_e2e_evidence(const char *s, const char *stage, int rc) {
  assert(s && stage); (void)rc;
}
static int create(h2_gizclaw_service_t *s, bool set, h2_gizclaw_req_t **out) {
  assert(s && !alive); *out = NULL; int rc = step(); if (rc) return rc;
  request = (struct h2_gizclaw_req){.set=set}; alive = true; *out = &request; return 0;
}
h2_pal_result_t h2_gizclaw_req_create_debug_set(h2_gizclaw_service_t *s,
    uint64_t id, h2_gizclaw_str_t val, uint32_t timeout, h2_gizclaw_req_t **out) {
  assert(id == 70 && val.len == 8 && !memcmp(val.data, "readonly", 8) && timeout == 30000);
  return create(s, true, out);
}
h2_pal_result_t h2_gizclaw_req_create_debug_get(h2_gizclaw_service_t *s,
    uint64_t id, uint32_t timeout, h2_gizclaw_req_t **out) {
  assert(id == 71 && timeout == 30000); return create(s, false, out);
}
h2_pal_result_t h2_gizclaw_req_do(h2_gizclaw_req_t *r, void *u,
    h2_gizclaw_req_input_read_fn in, h2_gizclaw_req_output_write_fn out,
    h2_gizclaw_req_complete_fn done) {
  assert(alive && r && !u && !in && !out && !done);
  int rc = step(); if (!rc) r->started = true; return rc;
}
h2_pal_result_t h2_gizclaw_req_wait(h2_gizclaw_req_t *r, uint32_t timeout) {
  assert(alive && r->started && timeout == 30000); int rc = step();
  if (!rc) { r->done = true; if (r->set && mode != WRITE_NOT_PERSISTED) strcpy(durable, "readonly"); }
  return rc;
}
h2_pal_result_t h2_gizclaw_req_cancel(h2_gizclaw_req_t *r) { assert(alive && r); return 0; }
void h2_gizclaw_req_release(h2_gizclaw_req_t *r) { if (r) { assert(alive); alive = false; } }
static int parse(const h2_gizclaw_req_t *r, h2_gizclaw_debug_state_t *out) {
  assert(alive && r->done); int rc = step(); if (rc) return rc;
  strcpy(out->mode, mode == READ_WRONG_MODE ? "wrong" : r->set ? "readonly" : durable); return 0;
}
h2_pal_result_t h2_gizclaw_resp_parse_debug_set(const h2_gizclaw_req_t *r, h2_gizclaw_debug_state_t *o) { return parse(r,o); }
h2_pal_result_t h2_gizclaw_resp_parse_debug_get(const h2_gizclaw_req_t *r, h2_gizclaw_debug_state_t *o) { return parse(r,o); }
h2_pal_result_t h2_gizclaw_debug_snapshot(h2_gizclaw_service_t *s, h2_gizclaw_debug_snapshot_t *o) {
  assert(s); int rc = step(); if (rc) return rc;
  if (async_pending && ++busy_reads > 2u && mode != NEVER_FINISH) {
    snapshot.busy = false; snapshot.known = mode != UNKNOWN_SNAPSHOT;
    snapshot.last_result = mode == ASYNC_FAILED ? H2_PAL_ERR_IO : H2_PAL_OK;
    if (mode != STALE_SNAPSHOT) ++snapshot.revision;
    strcpy(snapshot.mode, desired); strcpy(durable, desired); async_pending = false;
  }
  *o = snapshot; return 0;
}
static int begin(const char *mode_name) {
  int rc = step(); if (rc) return rc;
  assert(!async_pending); strcpy(desired, mode_name); snapshot.busy = true;
  async_pending = true; busy_reads = 0; return 0;
}
h2_pal_result_t h2_gizclaw_debug_refresh(h2_gizclaw_service_t *s, uint32_t t) { assert(s && t == 30000); return begin(durable); }
h2_pal_result_t h2_gizclaw_debug_set_mode(h2_gizclaw_service_t *s, h2_gizclaw_str_t mode_name, uint32_t t) {
  assert(s && t == 30000 && mode_name.len == 3u); return begin("off");
}
static int run(unsigned test_mode, unsigned at) {
  mode = test_mode; fail_at = at; calls = 0; alive = async_pending = false;
  durable[0] = desired[0] = '\0'; snapshot = (h2_gizclaw_debug_snapshot_t){0};
  h2_app_test_time_init(&clock, 0u);
  h2_gizclaw_e2e_fixture_t f = {.time=&clock.api, .actors={{.service=(h2_gizclaw_service_t *)&service}}};
  int rc = h2_gizclaw_e2e_run_debug(&f); assert(!alive); return rc;
}
int main(void) {
  assert(run(NORMAL, 0) == 0); unsigned count = calls; assert(!strcmp(durable, "off"));
  for (unsigned i = 1; i < BUDGET_EXPIRED; ++i) if (i != API_ERROR) assert(run(i, 0) != 0);
  for (unsigned i = 1; i <= count; ++i) assert(run(API_ERROR, i) == H2_PAL_ERR_IO);
  assert(run(BUDGET_EXPIRED, 0) == H2_PAL_ERR_TIMEOUT);
  return 0;
}
