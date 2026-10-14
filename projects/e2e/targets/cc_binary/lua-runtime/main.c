#include "h2_desktop_application_host.h"
#include "h2_desktop_platform.h"
#include "h2_lua_runtime_e2e.h"
#include "h2_pal.h"
#include "h2_runtime.h"

#include <stdio.h>

static h2_lua_runtime_e2e_report_t report;
static h2_pal_result_t configure(void *user, h2_runtime_config_t *config) {
  (void)user;
  config->periph = h2_lua_runtime_e2e_periph_api();
  config->component_mapper = h2_lua_runtime_e2e_component_mapper();
  return H2_PAL_OK;
}
static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *runtime, void *user) {
  (void)host; (void)user;
  return h2_lua_runtime_e2e_run(runtime,
      &(h2_lua_runtime_e2e_config_t){.scheduler = "multi-worker", .worker_count = 2u}, &report);
}
int main(void) {
  const h2_desktop_application_host_config_t platform = {0};
  const h2_application_host_config_t app = {.entry = run, .configure = configure};
  h2_application_host_result_t lifecycle = {0};
  int result = h2_desktop_application_host_run(&platform, &app, &lifecycle);
  size_t i;
  if (result != H2_PAL_OK && report.case_count == 0u) {
    fprintf(stderr, "H2_LUA_E2E result=FAIL rc=%d\n", result);
    return 1;
  }
  for (i = 0u; i < report.case_count; ++i) {
    printf("H2_LUA_E2E_CASE id=%s result=%s rc=%d evidence=%llu\n",
           report.cases[i].id,
           report.cases[i].result == H2_PAL_OK ? "PASS" : "FAIL",
           report.cases[i].result,
           (unsigned long long)report.cases[i].evidence);
  }
  printf("H2_LUA_E2E result=%s scheduler=%s passed=%zu total=%zu\n",
         result == H2_PAL_OK ? "PASS" : "FAIL", report.scheduler, report.passed,
         report.case_count);
  return result == H2_PAL_OK ? 0 : 1;
}
