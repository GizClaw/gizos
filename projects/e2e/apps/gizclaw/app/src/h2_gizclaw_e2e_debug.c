#include "h2_gizclaw_e2e_debug.h"
#include <string.h>

#define TIMEOUT_MS 30000u
static int evidence(const char *symbol, const char *stage, int rc) {
  h2_gizclaw_e2e_evidence(symbol, stage, rc);
  return rc;
}

static int request(h2_gizclaw_e2e_fixture_t *f, bool set, const char *expected) {
  if (!h2_gizclaw_e2e_fixture_has_time(f, TIMEOUT_MS)) return H2_PAL_ERR_TIMEOUT;
  h2_gizclaw_req_t *r = NULL;
  h2_gizclaw_service_t *s = f->actors[0].service;
  int rc = set ? evidence("h2_gizclaw_req_create_debug_set", "debug",
      h2_gizclaw_req_create_debug_set(s, 70u, h2_gizclaw_e2e_str(expected), TIMEOUT_MS, &r))
    : evidence("h2_gizclaw_req_create_debug_get", "debug",
      h2_gizclaw_req_create_debug_get(s, 71u, TIMEOUT_MS, &r));
  if (rc == H2_PAL_OK) rc = evidence("h2_gizclaw_req_do", "debug",
      h2_gizclaw_req_do(r, NULL, NULL, NULL, NULL));
  if (rc == H2_PAL_OK) rc = evidence("h2_gizclaw_req_wait", "debug",
      h2_gizclaw_req_wait(r, TIMEOUT_MS));
  h2_gizclaw_debug_state_t state = {0};
  const char *parse = set ? "h2_gizclaw_resp_parse_debug_set" : "h2_gizclaw_resp_parse_debug_get";
  if (rc == H2_PAL_OK) rc = evidence(parse, "debug", set
      ? h2_gizclaw_resp_parse_debug_set(r, &state)
      : h2_gizclaw_resp_parse_debug_get(r, &state));
  if (rc != H2_PAL_OK && r) (void)h2_gizclaw_req_cancel(r);
  h2_gizclaw_req_release(r);
  if (rc == H2_PAL_OK && (!memchr(state.mode, 0, sizeof(state.mode)) ||
                          strcmp(state.mode, expected))) rc = H2_PAL_ERR_INVALID_STATE;
  return evidence(parse, set ? "debug_set-assert" : "debug_get-assert", rc);
}

static int observe(h2_gizclaw_e2e_fixture_t *f, uint32_t before, const char *mode) {
  uint64_t start = 0u;
  int rc = h2_pal_time_get_monotonic_ms(f->time, &start);
  while (rc == H2_PAL_OK) {
    h2_gizclaw_debug_snapshot_t s = {0};
    rc = h2_gizclaw_debug_snapshot(f->actors[0].service, &s);
    if (rc != H2_PAL_OK || !s.busy)
      evidence("h2_gizclaw_debug_snapshot", "debug", rc);
    if (rc != H2_PAL_OK) break;
    if (!s.busy) {
      if (!s.known || s.last_result != H2_PAL_OK || s.revision == before ||
          !memchr(s.mode, 0, sizeof(s.mode)) || strcmp(s.mode, mode))
        rc = H2_PAL_ERR_INVALID_STATE;
      break;
    }
    uint64_t now = 0u;
    rc = h2_pal_time_get_monotonic_ms(f->time, &now);
    if (rc == H2_PAL_OK && (now < start || now - start >= TIMEOUT_MS ||
        !h2_gizclaw_e2e_fixture_has_time(f, 10u))) rc = H2_PAL_ERR_TIMEOUT;
    if (rc == H2_PAL_OK) rc = h2_pal_time_sleep_ms(f->time, 10u);
  }
  return rc;
}

int h2_gizclaw_e2e_run_debug(h2_gizclaw_e2e_fixture_t *f) {
  if (!f || !f->time || !f->actors[0].service) return H2_PAL_ERR_INVALID_ARG;
  int rc = request(f, true, "readonly");
  if (rc == H2_PAL_OK) rc = request(f, false, "readonly");
  h2_gizclaw_debug_snapshot_t initial = {0};
  if (rc == H2_PAL_OK) rc = h2_gizclaw_debug_snapshot(f->actors[0].service, &initial);
  if (rc == H2_PAL_OK) rc = evidence("h2_gizclaw_debug_refresh", "debug",
      h2_gizclaw_debug_refresh(f->actors[0].service, TIMEOUT_MS));
  if (rc == H2_PAL_OK) rc = observe(f, initial.revision, "readonly");
  evidence("h2_gizclaw_debug_snapshot", "debug_refresh-assert", rc);
  if (rc == H2_PAL_OK) rc = h2_gizclaw_debug_snapshot(f->actors[0].service, &initial);
  if (rc == H2_PAL_OK) rc = evidence("h2_gizclaw_debug_set_mode", "debug",
      h2_gizclaw_debug_set_mode(f->actors[0].service, h2_gizclaw_e2e_str("off"), TIMEOUT_MS));
  if (rc == H2_PAL_OK) rc = observe(f, initial.revision, "off");
  if (rc == H2_PAL_OK) rc = request(f, false, "off");
  evidence("h2_gizclaw_debug_snapshot", "debug_set_mode-assert", rc);
  evidence("h2_gizclaw_debug_snapshot", "debug_snapshot-assert", rc);
  return rc;
}
