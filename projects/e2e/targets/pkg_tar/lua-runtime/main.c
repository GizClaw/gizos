#include "h2_lua_runtime_e2e.h"
#include "h2_lua_runtime_e2e_task_names.h"
#include "h2_web_application_host.h"
#include "h2_web_main_thread.h"
#include "h2_web_platform.h"

#include <emscripten.h>
#include <stdatomic.h>
#include <stdio.h>

/* clang-format off */
EM_JS(void, web_set_result,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["i32", "u32"], null,
    (passed, count) => {
  const element = globalThis.document && document.getElementById('result');
  if (element) {
    element.textContent =
        passed == count ? `PASS ${passed} / ${count} scheduler = multi-worker`
                          : `FAIL ${passed} / ${count} scheduler = multi-worker`;
    element.dataset.terminal = passed == count ? 'pass' : 'fail';
  }
});
});
/* clang-format on */

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
  const h2_web_application_host_config_t config = {
      .name = "lua-runtime", .display_width = 1, .display_height = 1};
  const h2_application_host_config_t app = {.entry = run, .configure = configure};
  h2_application_host_result_t lifecycle = {0};
  int result = h2_web_application_host_run_app(&config, &app, &lifecycle);
  size_t i;
  for (i = 0u; i < report.case_count; ++i) {
    printf("H2_LUA_E2E_CASE id=%s result=%s rc=%d evidence=%llu\n",
           report.cases[i].id,
           report.cases[i].result == H2_PAL_OK ? "PASS" : "FAIL",
           report.cases[i].result,
           (unsigned long long)report.cases[i].evidence);
  }
  printf("H2_LUA_E2E result=%s scheduler=%s passed=%zu total=%zu\n",
         result == H2_PAL_OK ? "PASS" : "FAIL",
         report.scheduler == NULL ? "unknown"
                                          : report.scheduler,
         report.passed, report.case_count);
  (void)h2_web_main_call(
      web_set_result, (const void *[]){&(int){(int)report.passed},
                                       &(size_t){report.case_count}});
  return result == 0 && lifecycle.cleanup == 0 && !lifecycle.retained ? 0 : 1;
}
