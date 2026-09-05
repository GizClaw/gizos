#include "h2_gizclaw_e2e_telemetry.h"

#define TELEMETRY_TIMEOUT_MS 30000u

static int checked(const char *symbol, const char *stage, int rc) {
  h2_gizclaw_e2e_evidence(symbol, stage, rc);
  return rc;
}

int h2_gizclaw_e2e_run_telemetry(h2_gizclaw_e2e_fixture_t *fixture,
                                 h2_gizclaw_resp_storage_t *storage) {
  (void)storage; // One-way telemetry has no response body or storage.
  if (fixture == NULL || fixture->time == NULL ||
      fixture->actors[H2_GIZCLAW_E2E_OWNER].service == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_service_t *service = fixture->actors[H2_GIZCLAW_E2E_OWNER].service;
  // Synthetic test observations, not measurements of the host's hardware.
  const h2_gizclaw_telemetry_observation_t observations[] = {
      {.kind = H2_GIZCLAW_TELEMETRY_BATTERY,
       .value.battery = {.has_percent = true, .percent = 64.4,
                         .has_charging = true, .charging = false}},
      {.kind = H2_GIZCLAW_TELEMETRY_SYSTEM,
       .value.system = {.has_software_version = true,
                        .software_version = {.data = "e2e-fixture", .len = 11u}}},
  };
  for (unsigned api = 0u; api < 2u; ++api) {
    if (!h2_gizclaw_e2e_fixture_has_time(fixture, TELEMETRY_TIMEOUT_MS))
      return H2_PAL_ERR_TIMEOUT;
    uint64_t wall_ms = 0u;
    int rc = h2_pal_time_get_wall_ms(fixture->time, &wall_ms);
    if (rc != H2_PAL_OK)
      return rc;
    if (wall_ms == 0u || wall_ms > INT64_MAX)
      return H2_PAL_ERR_INVALID_STATE;
    const h2_gizclaw_telemetry_frame_t frame = {
        .sequence = api + 1u,
        .observed_at_unix_ms = (int64_t)wall_ms,
        .observations = observations,
        .observation_count = sizeof(observations) / sizeof(observations[0]),
    };
    const char *symbol = api == 0u ? "h2_gizclaw_resp_parse_telemetry_send"
                                   : "h2_gizclaw_rpc_telemetry_send";
    if (api == 0u) {
      h2_gizclaw_req_t *request = NULL;
      rc = checked("h2_gizclaw_req_create_telemetry_send", "telemetry-req",
                   h2_gizclaw_req_create_telemetry_send(
                       service, 10u, &frame, TELEMETRY_TIMEOUT_MS, &request));
      if (rc == H2_PAL_OK)
        rc = checked("h2_gizclaw_req_do", "telemetry-req",
                     h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL));
      if (rc == H2_PAL_OK)
        rc = checked("h2_gizclaw_req_wait", "telemetry-req",
                     h2_gizclaw_req_wait(request, TELEMETRY_TIMEOUT_MS));
      if (rc == H2_PAL_OK)
        rc = checked(symbol, "telemetry-req",
                     h2_gizclaw_resp_parse_telemetry_send(request));
      if (request != NULL) {
        if (rc != H2_PAL_OK)
          (void)checked("h2_gizclaw_req_cancel", "telemetry-cleanup",
                        h2_gizclaw_req_cancel(request));
        h2_gizclaw_req_release(request);
      }
    } else {
      /* The API is deliberately one-shot: WOULD_BLOCK means that packet was
       * not accepted and the library does not retry it.  A live acceptance
       * test may submit a fresh attempt after yielding to the network owner;
       * otherwise its position after the high-volume RPC cases makes the
       * result depend on transient DataChannel occupancy. */
      do {
        rc = checked(symbol, "telemetry-rpc",
                     h2_gizclaw_rpc_telemetry_send(service, &frame,
                                                  TELEMETRY_TIMEOUT_MS));
        if (rc != H2_PAL_ERR_WOULD_BLOCK)
          break;
        if (!h2_gizclaw_e2e_fixture_has_time(fixture, TELEMETRY_TIMEOUT_MS)) {
          rc = H2_PAL_ERR_TIMEOUT;
          break;
        }
        rc = h2_pal_time_sleep_ms(fixture->time, 1u);
      } while (rc == H2_PAL_OK);
    }
    // The API's entire success contract is transport acceptance. There is no
    // remote reply to inspect; this must NOT be read as a persistence assertion.
    checked(symbol, "telemetry_send-assert", rc);
    if (rc != H2_PAL_OK)
      return rc;
  }
  return H2_PAL_OK;
}
