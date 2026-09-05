#include "h2_gizclaw_e2e_catalog.h"
#include "h2_gizclaw_e2e_concurrency.h"
#include "h2_gizclaw_e2e_firmware.h"
#include "h2_gizclaw_e2e_rpc.h"
#include "h2_gizclaw_e2e_service.h"
#include "h2_gizclaw_e2e_voice.h"

static int run_voice(h2_gizclaw_e2e_fixture_t *fixture) {
  int rc = h2_gizclaw_e2e_prepare_voice(fixture);
  return rc == H2_PAL_OK ? h2_gizclaw_e2e_run_voice(fixture) : rc;
}

const e2e_case_t h2_gizclaw_e2e_cases[] = {
    {"connectivity", H2_GIZCLAW_E2E_SUITE_CONNECTIVITY, 2u, false,
     h2_gizclaw_e2e_run_connectivity},
    {"rpc", H2_GIZCLAW_E2E_SUITE_RPC, H2_GIZCLAW_E2E_ACTOR_COUNT, true,
     h2_gizclaw_e2e_run_rpc},
    {"firmware", H2_GIZCLAW_E2E_SUITE_FIRMWARE, 1u, false,
     h2_gizclaw_e2e_run_firmware},
    {"voice", H2_GIZCLAW_E2E_SUITE_VOICE, 1u, true, run_voice},
    {"concurrency", H2_GIZCLAW_E2E_SUITE_CONCURRENCY, 1u, false,
     h2_gizclaw_e2e_run_concurrency},
    {"service", H2_GIZCLAW_E2E_SUITE_SERVICE, 1u, false,
     h2_gizclaw_e2e_run_service},
};

const size_t h2_gizclaw_e2e_case_count =
    sizeof(h2_gizclaw_e2e_cases) / sizeof(h2_gizclaw_e2e_cases[0]);
