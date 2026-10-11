#include "h2_gizclaw_e2e_catalog.h"
const e2e_case_t h2_gizclaw_e2e_cases[] = {
    {"lua-apps", H2_GIZCLAW_E2E_SUITE_LUA_APPS, 1u, false,
     h2_gizclaw_e2e_run_lua_apps, h2_gizclaw_e2e_prepare_lua_apps},
};
const size_t h2_gizclaw_e2e_case_count = 1u;
