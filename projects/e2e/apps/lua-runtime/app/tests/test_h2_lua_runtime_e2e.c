#include "h2_lua_runtime_e2e.h"

#include "h2_desktop_platform.h"
#include "h2_smoke_host_runtime.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  h2_runtime_t *runtime = NULL;
  h2_lua_runtime_e2e_report_t report;
  h2_runtime_config_t runtime_config = h2_smoke_host_runtime_config(
      "test", "desktop", "host", h2_desktop_platform_default_allocator(),
      h2_desktop_platform_time_api(), h2_desktop_platform_queue_api(),
      h2_pal_unsupported_display_api());
  runtime_config.task = h2_desktop_platform_task_api();
  runtime_config.sync = h2_desktop_platform_sync_api();
  runtime_config.periph = h2_lua_runtime_e2e_periph_api();
  runtime_config.component_mapper = h2_lua_runtime_e2e_component_mapper();
  assert(h2_runtime_init(&runtime_config, &runtime) == H2_PAL_OK);
  assert(h2_runtime_input_start(runtime, NULL) == H2_PAL_OK);
  assert(h2_lua_runtime_e2e_run(runtime, NULL, &report) ==
         H2_PAL_ERR_INVALID_ARG);
  h2_pal_result_t result =
      h2_lua_runtime_e2e_run(runtime,
                             &(h2_lua_runtime_e2e_config_t){
                                 .scheduler = "multi-worker",
                                 .worker_count = 2u,
                             },
                             &report);
  if (result != H2_PAL_OK) {
    for (size_t i = 0u; i < report.case_count; ++i) {
      fprintf(stderr, "%s: result=%d evidence=%llu\n", report.cases[i].id,
              (int)report.cases[i].result,
              (unsigned long long)report.cases[i].evidence);
    }
  }
  assert(result == H2_PAL_OK);
  assert(report.case_count == H2_LUA_RUNTIME_E2E_CASE_COUNT);
  assert(report.passed == report.case_count);
  assert(strcmp(report.scheduler, "multi-worker") == 0);
  for (size_t i = 0u; i < report.case_count; ++i) {
    assert(report.cases[i].id != NULL);
    assert(report.cases[i].result == H2_PAL_OK);
  }
  h2_runtime_deinit(runtime);
  return 0;
}
