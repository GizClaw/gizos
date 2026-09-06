#include "h2_gizclaw_e2e_catalog.h"
const e2e_case_t h2_gizclaw_e2e_cases[] = {
    {"device-api", H2_GIZCLAW_E2E_SUITE_DEVICE, 1, false, h2_gizclaw_e2e_run_device,
     h2_gizclaw_e2e_prepare_device},
};
const size_t h2_gizclaw_e2e_case_count = 1;
