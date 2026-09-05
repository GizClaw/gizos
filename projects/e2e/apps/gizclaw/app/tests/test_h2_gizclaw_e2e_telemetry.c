#include "h2_gizclaw_e2e_telemetry.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Execute the real case with public API doubles, not a server or transport. */
struct h2_gizclaw_req {
  bool submitted, complete;
};
static struct {
  struct h2_gizclaw_req request;
  bool alive, emit;
  unsigned step, fail_at, deadlines, expire_at, clocks, clock_fail_at;
  unsigned creates, releases, cancels, proofs, failed_proofs;
  unsigned rpc_calls, rpc_would_blocks, sleeps;
  uint64_t wall_ms;
  int cancel_rc;
} state;

static int step(void) {
  return ++state.step == state.fail_at ? H2_PAL_ERR_IO : H2_PAL_OK;
}

static h2_pal_result_t wall(void *user, uint64_t *out) {
  assert(user == &state && out != NULL);
  if (++state.clocks == state.clock_fail_at)
    return H2_PAL_ERR_IO;
  *out = state.wall_ms;
  return H2_PAL_OK;
}
static h2_pal_result_t sleep_ms(void *user, uint32_t ms) {
  assert(user == &state && ms == 1u);
  ++state.sleeps;
  return H2_PAL_OK;
}
static const h2_pal_time_vtable_t time_vtable = {
    .get_wall_ms = wall, .sleep_ms = sleep_ms};
static const h2_pal_time_api_t time_api = {.user = &state, .vtable = &time_vtable};

bool h2_gizclaw_e2e_fixture_has_time(const h2_gizclaw_e2e_fixture_t *fixture,
                                    uint32_t ms) {
  assert(fixture != NULL && ms == 30000u && !state.alive);
  return ++state.deadlines != state.expire_at;
}

void h2_gizclaw_e2e_evidence(const char *symbol, const char *stage, int rc) {
  assert(symbol != NULL && stage != NULL);
  if (strcmp(stage, "telemetry_send-assert") == 0) {
    assert(!state.alive);
    if (rc == H2_PAL_OK)
      ++state.proofs;
    else
      ++state.failed_proofs;
  }
  if (state.emit)
    printf("H2_GIZCLAW_E2E symbol=%s stage=%s result=%s rc=%d\n", symbol,
           stage, rc == H2_PAL_OK ? "PASS" : "FAIL", rc);
}

static void check_frame(const h2_gizclaw_telemetry_frame_t *frame,
                         unsigned sequence) {
  assert(frame != NULL && frame->sequence == sequence);
  assert(frame->observed_at_unix_ms == (int64_t)state.wall_ms);
  assert(frame->observation_count == 2u && frame->observations != NULL);
  const h2_gizclaw_telemetry_observation_t *battery = &frame->observations[0];
  const h2_gizclaw_telemetry_observation_t *system = &frame->observations[1];
  assert(battery->kind == H2_GIZCLAW_TELEMETRY_BATTERY);
  assert(battery->value.battery.has_percent &&
         battery->value.battery.percent == 64.4);
  assert(battery->value.battery.has_charging &&
         !battery->value.battery.charging);
  assert(system->kind == H2_GIZCLAW_TELEMETRY_SYSTEM);
  assert(system->value.system.has_software_version);
  assert(system->value.system.software_version.len == 11u);
  assert(memcmp(system->value.system.software_version.data, "e2e-fixture", 11u) == 0);
  assert(!system->value.system.has_uptime_seconds);
}

h2_pal_result_t h2_gizclaw_req_create_telemetry_send(
    h2_gizclaw_service_t *service, uint64_t identity,
    const h2_gizclaw_telemetry_frame_t *frame, uint32_t ms,
    h2_gizclaw_req_t **out) {
  assert(service != NULL && identity == 10u && ms == 30000u);
  assert(out != NULL && *out == NULL && !state.alive);
  check_frame(frame, 1u);
  int rc = step();
  if (rc == H2_PAL_OK) {
    state.alive = true;
    ++state.creates;
    *out = &state.request;
  }
  return rc;
}
h2_pal_result_t h2_gizclaw_req_do(h2_gizclaw_req_t *request,
                                 void *user,
                                 h2_gizclaw_req_input_read_fn input_read,
                                 h2_gizclaw_req_output_write_fn output_write,
                                  h2_gizclaw_req_complete_fn on_complete) {
  (void)on_complete;
  assert(state.alive && request == &state.request);
  assert(user == NULL && input_read == NULL && output_write == NULL &&
         !request->submitted);
  int rc = step();
  request->submitted = rc == H2_PAL_OK;
  return rc;
}
h2_pal_result_t h2_gizclaw_req_wait(h2_gizclaw_req_t *request, uint32_t ms) {
  assert(state.alive && request == &state.request);
  assert(request->submitted && ms == 30000u && !request->complete);
  int rc = step();
  request->complete = rc == H2_PAL_OK;
  return rc;
}
h2_pal_result_t h2_gizclaw_resp_parse_telemetry_send(
    const h2_gizclaw_req_t *request) {
  assert(state.alive && request == &state.request && request->complete);
  return step();
}
h2_pal_result_t h2_gizclaw_req_cancel(h2_gizclaw_req_t *request) {
  assert(state.alive && request == &state.request);
  ++state.cancels;
  return state.cancel_rc;
}
void h2_gizclaw_req_release(h2_gizclaw_req_t *request) {
  assert(state.alive && request == &state.request);
  ++state.releases;
  state.alive = false;
  memset(request, 0xa5, sizeof(*request));
}
h2_pal_result_t h2_gizclaw_rpc_telemetry_send(
    h2_gizclaw_service_t *service, const h2_gizclaw_telemetry_frame_t *frame,
    uint32_t ms) {
  assert(service != NULL && ms == 30000u && !state.alive);
  check_frame(frame, 2u);
  ++state.rpc_calls;
  if (state.rpc_would_blocks != 0u) {
    --state.rpc_would_blocks;
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  return step();
}

static h2_gizclaw_e2e_fixture_t fixture(void) {
  memset(&state, 0, sizeof(state));
  state.wall_ms = UINT64_C(1800000000000);
  h2_gizclaw_e2e_fixture_t result = {0};
  result.time = &time_api;
  result.actors[H2_GIZCLAW_E2E_OWNER].service = (void *)&state;
  return result;
}

int main(int argc, char **argv) {
  (void)argv;
  h2_gizclaw_e2e_fixture_t f = fixture();
  state.emit = argc > 1;
  if (state.emit) {
    puts("H2_GIZCLAW_E2E stage=coverage-begin case=rpc");
    puts("H2_GIZCLAW_E2E stage=coverage-begin case=rpc/telemetry");
  }
  assert(h2_gizclaw_e2e_run_telemetry(&f, NULL) == H2_PAL_OK);
  assert(state.step == 5u && state.proofs == 2u && state.failed_proofs == 0u);
  assert(state.deadlines == 2u && state.clocks == 2u);
  assert(state.creates == 1u && state.releases == 1u && state.cancels == 0u);
  if (state.emit) {
    puts("H2_GIZCLAW_E2E stage=coverage-end case=rpc/telemetry status=PASS rc=0 cleanup_rc=0");
    puts("H2_GIZCLAW_E2E stage=coverage-end case=rpc status=PASS rc=0 cleanup_rc=0");
    return 0;
  }
  f = fixture();
  state.rpc_would_blocks = 2u;
  assert(h2_gizclaw_e2e_run_telemetry(&f, NULL) == H2_PAL_OK);
  assert(state.rpc_calls == 3u && state.sleeps == 2u);
  assert(state.deadlines == 4u && state.proofs == 2u &&
         state.failed_proofs == 0u);
  for (unsigned failure = 1u; failure <= 5u; ++failure) {
    f = fixture();
    state.fail_at = failure;
    state.cancel_rc = H2_PAL_ERR_CLOSED; // Cleanup must not replace first error.
    assert(h2_gizclaw_e2e_run_telemetry(&f, NULL) == H2_PAL_ERR_IO);
    assert(state.step == failure && !state.alive);
    assert(state.creates == state.releases);
    assert(state.cancels == (failure > 1u && failure < 5u ? 1u : 0u));
    assert(state.proofs == (failure == 5u ? 1u : 0u));
    assert(state.failed_proofs == 1u);
  }
  for (unsigned at = 1u; at <= 2u; ++at) {
    f = fixture();
    state.expire_at = at;
    assert(h2_gizclaw_e2e_run_telemetry(&f, NULL) == H2_PAL_ERR_TIMEOUT);
    assert(state.clocks == at - 1u && !state.alive);
    assert(state.proofs == at - 1u);
    f = fixture();
    state.clock_fail_at = at;
    assert(h2_gizclaw_e2e_run_telemetry(&f, NULL) == H2_PAL_ERR_IO);
    assert(state.clocks == at && !state.alive && state.proofs == at - 1u);
  }
  const uint64_t invalid_times[] = {0u, (uint64_t)INT64_MAX + 1u, UINT64_MAX};
  for (unsigned i = 0u; i < sizeof(invalid_times) / sizeof(invalid_times[0]); ++i) {
    f = fixture();
    state.wall_ms = invalid_times[i];
    assert(h2_gizclaw_e2e_run_telemetry(&f, NULL) == H2_PAL_ERR_INVALID_STATE);
    assert(state.step == 0u && state.proofs == 0u);
  }
  f = fixture();
  state.wall_ms = INT64_MAX;
  assert(h2_gizclaw_e2e_run_telemetry(&f, NULL) == H2_PAL_OK);
  f = fixture();
  f.time = NULL;
  assert(h2_gizclaw_e2e_run_telemetry(&f, NULL) == H2_PAL_ERR_INVALID_ARG);
  f = fixture();
  f.actors[H2_GIZCLAW_E2E_OWNER].service = NULL;
  assert(h2_gizclaw_e2e_run_telemetry(&f, NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_e2e_run_telemetry(NULL, NULL) == H2_PAL_ERR_INVALID_ARG);
  puts("Telemetry E2E case boundary tests passed (not live E2E)");
  return 0;
}
