#include "h2_gizclaw_e2e_catalog.h"
#include "h2_gizclaw_e2e_rpc.h"

const e2e_case_t h2_gizclaw_e2e_cases[] = {
    {"connectivity", H2_GIZCLAW_E2E_SUITE_CONNECTIVITY, 2u, false,
     h2_gizclaw_e2e_run_connectivity},
};

const size_t h2_gizclaw_e2e_case_count =
    sizeof(h2_gizclaw_e2e_cases) / sizeof(h2_gizclaw_e2e_cases[0]);
