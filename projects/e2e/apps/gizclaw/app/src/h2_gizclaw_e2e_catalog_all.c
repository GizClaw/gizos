#include "h2_gizclaw_e2e_catalog.h"
#include "h2_gizclaw_e2e_concurrency.h"
#include "h2_gizclaw_e2e_firmware.h"
#include "h2_gizclaw_e2e_fence.h"
#include "h2_gizclaw_e2e_rpc.h"
#include "h2_gizclaw_e2e_service.h"
#include "h2_gizclaw_e2e_voice.h"
#include "h2_gizclaw_e2e_resource.h"

static int prepare_session(h2_gizclaw_e2e_fixture_t *fixture) {
  fixture->use_session = true;
  return H2_PAL_OK;
}

static int run_voice(h2_gizclaw_e2e_fixture_t *fixture) {
  int rc = h2_gizclaw_e2e_prepare_voice(fixture);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_e2e_run_voice(fixture);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_e2e_run_session_cancel(fixture);
  /* Session owns its route; raw Conversation and Service audio APIs require
   * their own run after Session teardown. Its internal calls are not evidence
   * that an application can use those public APIs directly. */
  if (rc == H2_PAL_OK) {
    fixture->use_session = false;
    rc = h2_gizclaw_e2e_fixture_reconnect_actor(fixture, H2_GIZCLAW_E2E_OWNER);
    if (rc == H2_PAL_OK)
      rc = h2_gizclaw_e2e_run_voice(fixture);
  }
  return rc;
}

const e2e_case_t h2_gizclaw_e2e_cases[] = {
    {"resource", H2_GIZCLAW_E2E_SUITE_RESOURCE, 1u, false,
     h2_gizclaw_e2e_run_resource, NULL},
    {"device-api", H2_GIZCLAW_E2E_SUITE_DEVICE, 1u, false,
     h2_gizclaw_e2e_run_device, h2_gizclaw_e2e_prepare_device},
    {"workspace-fence", H2_GIZCLAW_E2E_SUITE_FENCE, 1u, false,
     h2_gizclaw_e2e_run_fence, NULL},
    {"connectivity", H2_GIZCLAW_E2E_SUITE_CONNECTIVITY, 2u, false,
     h2_gizclaw_e2e_run_connectivity, NULL},
    {"rpc", H2_GIZCLAW_E2E_SUITE_RPC, H2_GIZCLAW_E2E_ACTOR_COUNT, true,
     h2_gizclaw_e2e_run_rpc, NULL},
    {"firmware", H2_GIZCLAW_E2E_SUITE_FIRMWARE, 1u, false,
     h2_gizclaw_e2e_run_firmware, NULL},
    {"voice", H2_GIZCLAW_E2E_SUITE_VOICE, 1u, true, run_voice, prepare_session},
    {"concurrency", H2_GIZCLAW_E2E_SUITE_CONCURRENCY, 1u, false,
     h2_gizclaw_e2e_run_concurrency, NULL},
    {"service", H2_GIZCLAW_E2E_SUITE_SERVICE, 1u, false,
     h2_gizclaw_e2e_run_service, NULL},
};

const size_t h2_gizclaw_e2e_case_count =
    sizeof(h2_gizclaw_e2e_cases) / sizeof(h2_gizclaw_e2e_cases[0]);
