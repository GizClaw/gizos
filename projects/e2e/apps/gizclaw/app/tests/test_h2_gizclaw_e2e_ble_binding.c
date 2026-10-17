#include "h2_gizclaw_e2e_ble_binding.h"
#include "h2_app_test_time.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Execute the production App flow with controlled lifecycle failures. These
 * substitutes never produce device/central qualification evidence. */
enum { NORMAL, BAD_FORMAT, STALE_REVISION, WRONG_EXPOSURE, NO_PEER,
       NO_DISCONNECT, NO_RESTART, DUPLICATE_EXPOSURE, EXPIRED_BUDGET };
struct h2_gizclaw_api_key_state { h2_gizclaw_api_key_snapshot_t snapshot; } key;
struct h2_gizclaw_ble_binding {
  bool open, advertising, connected;
  size_t exposures;
} binding;
static h2_app_test_time_t clock;
static unsigned mode, phase, calls, fail_at, assertions;
static size_t url_size;
static int service;
static int step(void) { return ++calls == fail_at ? H2_PAL_ERR_IO : H2_PAL_OK; }

h2_gizclaw_str_t h2_gizclaw_e2e_str(const char *s) {
  return (h2_gizclaw_str_t){s, strlen(s)};
}
bool h2_gizclaw_e2e_fixture_has_time(const h2_gizclaw_e2e_fixture_t *f, uint32_t ms) {
  assert(f && ms); return mode != EXPIRED_BUDGET;
}
void h2_gizclaw_e2e_evidence(const char *symbol, const char *stage, int rc) {
  assert(symbol && stage);
  if (!rc && strstr(stage, "ble_binding_") && strstr(stage, "-assert")) ++assertions;
}
int h2_gizclaw_e2e_emit(const char *format, ...) {
  char output[1024]; va_list args; va_start(args, format);
  (void)vsnprintf(output, sizeof(output), format, args); va_end(args);
  assert(!strstr(output, "gizclaw_sk_v1_fixture"));
  return 0;
}
int h2_gizclaw_e2e_fixture_poll(h2_gizclaw_e2e_fixture_t *f,
    h2_gizclaw_e2e_actor_role_t actor, uint32_t ms) {
  assert(f && actor == H2_GIZCLAW_E2E_OWNER && ms);
  assert(h2_app_test_time_advance(&clock, ms) == 0);
  if (!binding.open || mode == NO_PEER || (mode == NO_DISCONNECT && phase == 1u)) return 0;
  ++phase;
  if (phase == 1u) { binding.connected = true; binding.advertising = false; binding.exposures = 1u; }
  if (phase == 2u || phase == 4u) { binding.connected = false; binding.advertising = false; }
  if (phase == 3u) {
    binding.connected = true; binding.advertising = false;
    if (mode == DUPLICATE_EXPOSURE) binding.exposures = 1u;
  }
  return 0;
}

h2_pal_result_t h2_gizclaw_api_key_state_create(
    const h2_gizclaw_api_key_state_config_t *config, h2_gizclaw_api_key_state_t **out) {
  assert(config->service && !config->reuse_existing); *out = NULL;
  int rc = step(); if (!rc) *out = &key; return rc;
}
h2_pal_result_t h2_gizclaw_api_key_state_request_refresh(h2_gizclaw_api_key_state_t *state, bool revoke) {
  assert(state && !revoke); int rc = step(); if (rc) return rc;
  key.snapshot = (h2_gizclaw_api_key_snapshot_t){.valid = true, .revision = 42u};
  strcpy(key.snapshot.key.name, "test-owned-key");
  strcpy(key.snapshot.key.secret, "gizclaw_sk_v1_fixture"); return 0;
}
h2_pal_result_t h2_gizclaw_api_key_state_snapshot(h2_gizclaw_api_key_state_t *state,
    h2_gizclaw_api_key_snapshot_t *out) {
  assert(state); int rc = step(); if (!rc) *out = key.snapshot; return rc;
}
h2_pal_result_t h2_gizclaw_api_key_state_request_revoke(h2_gizclaw_api_key_state_t *state) {
  assert(state); int rc = step(); if (!rc) memset(&key.snapshot, 0, sizeof(key.snapshot)); return rc;
}
h2_pal_result_t h2_gizclaw_api_key_state_close(h2_gizclaw_api_key_state_t *state) {
  assert(state); return step();
}
h2_pal_result_t h2_gizclaw_api_key_state_destroy(h2_gizclaw_api_key_state_t **state) {
  int rc = step(); if (!rc) *state = NULL; return rc;
}
h2_pal_result_t h2_gizclaw_ble_binding_format_url(h2_gizclaw_str_t origin,
    h2_gizclaw_str_t icon, h2_gizclaw_str_t name, h2_gizclaw_str_t secret,
    char *out, size_t capacity, size_t *length) {
  assert(origin.len && icon.len == 5u && name.len == 9u && secret.len);
  int rc = step(); if (rc) return rc;
  int count = snprintf(out, capacity, "%s/api-keys/%s?icon=gizos&name=GizOS%%20E2E",
                        origin.data, secret.data);
  assert(count > 0 && (size_t)count < capacity); *length = url_size = (size_t)count;
  if (mode == BAD_FORMAT) out[0] ^= 1; return 0;
}
h2_pal_result_t h2_gizclaw_ble_binding_open(const h2_gizclaw_ble_binding_config_t *config,
    h2_gizclaw_ble_binding_t **out) {
  assert(config->ble && config->system_event && config->api_key_state == &key);
  *out = NULL; int rc = step(); if (!rc) *out = &binding; return rc;
}
h2_pal_result_t h2_gizclaw_ble_binding_start(h2_gizclaw_ble_binding_t *b) {
  assert(b); int rc = step(); if (!rc) b->open = b->advertising = true; return rc;
}
h2_pal_result_t h2_gizclaw_ble_binding_snapshot(h2_gizclaw_ble_binding_t *b,
    h2_gizclaw_ble_binding_snapshot_t *out) {
  assert(b); int rc = step(); if (rc) return rc;
  *out = (h2_gizclaw_ble_binding_snapshot_t){.open = b->open,
      .advertising = b->advertising, .connected = b->connected,
      .ready = b->open && key.snapshot.valid, .revision = key.snapshot.revision,
      .url_len = b->open ? url_size : 0u, .pending_exposures = b->exposures,
      .info_flags = b->open ? H2_GIZCLAW_BLE_BINDING_READY : H2_GIZCLAW_BLE_BINDING_CLOSED};
  if (mode == STALE_REVISION && b->open) ++out->revision;
  return 0;
}
h2_pal_result_t h2_gizclaw_ble_binding_poll(h2_gizclaw_ble_binding_t *b) {
  assert(b && !b->connected); int rc = step();
  if (!rc && mode != NO_RESTART) b->advertising = true; return rc;
}
h2_pal_result_t h2_gizclaw_ble_binding_next_exposure(h2_gizclaw_ble_binding_t *b,
    h2_gizclaw_ble_binding_exposure_t *out) {
  assert(b); memset(out, 0, sizeof(*out)); int rc = step(); if (rc) return rc;
  if (!b->exposures) return H2_PAL_ERR_WOULD_BLOCK;
  --b->exposures; out->revision = key.snapshot.revision;
  strcpy(out->key_name, mode == WRONG_EXPOSURE ? "another-key" : key.snapshot.key.name); return 0;
}
h2_pal_result_t h2_gizclaw_ble_binding_stop(h2_gizclaw_ble_binding_t *b) {
  assert(b); int rc = step(); if (!rc) b->open = b->advertising = b->connected = false; return rc;
}
h2_pal_result_t h2_gizclaw_ble_binding_close(h2_gizclaw_ble_binding_t **b) {
  assert(*b && !(*b)->open && !(*b)->exposures); int rc = step(); if (!rc) *b = NULL; return rc;
}

static int run(unsigned scenario, unsigned fail) {
  mode = scenario; fail_at = fail; calls = assertions = phase = 0u;
  memset(&key, 0, sizeof(key)); memset(&binding, 0, sizeof(binding));
  h2_app_test_time_init(&clock, 100u);
  const h2_pal_ble_host_api_t ble = {0}; const h2_pal_system_event_api_t events = {0};
  h2_runtime_t runtime = {.ble_host = &ble, .system_event = &events};
  const h2_gizclaw_e2e_config_t config = {.device_api_url = "https://e2e.example"};
  h2_gizclaw_e2e_fixture_t fixture = {.runtime = &runtime, .config = &config,
      .time = &clock.api, .actors = {{.service = (h2_gizclaw_service_t *)&service}}};
  strcpy(fixture.run_prefix, "h2e2e-0123456789abcdef");
  int rc = h2_gizclaw_e2e_run_ble_binding(&fixture);
  if (!rc) assert(assertions == 8u && phase == 4u && !fixture.actors[0].ble_binding && !fixture.actors[0].api_key_state);
  if (fixture.actors[0].ble_binding) assert(fixture.actors[0].api_key_state);
  return rc;
}

int main(void) {
  assert(run(NORMAL, 0u) == 0); const unsigned count = calls;
  for (unsigned i = BAD_FORMAT; i <= EXPIRED_BUDGET; ++i) assert(run(i, 0u) != 0);
  for (unsigned i = 1u; i <= count; ++i) assert(run(NORMAL, i) == H2_PAL_ERR_IO);
  return 0;
}
