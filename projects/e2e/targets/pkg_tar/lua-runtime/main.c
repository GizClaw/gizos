#include "h2_lua_runtime_e2e.h"
#include "h2_lua_runtime_e2e_task_names.h"
#include "h2_smoke_host_runtime.h"
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

typedef struct web_e2e_context {
  h2_runtime_t *runtime;
  h2_lua_runtime_e2e_report_t report;
  h2_pal_result_t result;
  _Atomic int done;
} web_e2e_context_t;

static void run_e2e(void *user) {
  web_e2e_context_t *context = user;
  context->result = h2_lua_runtime_e2e_run(
      context->runtime,
      &(h2_lua_runtime_e2e_config_t){
          .scheduler = "multi-worker",
                                               .worker_count = 2u,
      },
      &context->report);
  context->done = 1;
}

int main(void) {
  const h2_web_platform_config_t platform_config = {
      .display_width = 1,
      .display_height = 1,
  };
  h2_web_platform_t *platform = h2_web_platform_create(&platform_config);
  h2_runtime_t *runtime = NULL;
  web_e2e_context_t context = {0};
  h2_pal_task_t *app_task = NULL;
  h2_pal_result_t result;
  size_t i;
  if (platform == NULL)
    return 1;
  h2_runtime_config_t config = h2_smoke_host_runtime_config(
      "browser", "webassembly", "wasm32", h2_web_platform_mem_api(),
      h2_web_platform_time_api(platform), h2_web_platform_queue_api(platform),
      h2_pal_unsupported_display_api());
  config.log = h2_web_platform_log_api();
  config.timer = h2_web_platform_timer_api(platform);
  config.task = h2_web_platform_task_api(platform);
  config.sync = h2_web_platform_sync_api(platform);
  config.webrtc = h2_web_platform_webrtc_api(platform);
  config.periph = h2_lua_runtime_e2e_periph_api();
  config.component_mapper = h2_lua_runtime_e2e_component_mapper();
  result = h2_runtime_init(&config, &runtime);
  if (result == H2_PAL_OK) {
    const h2_pal_task_options_t task_options = {
        .name = h2_lua_runtime_e2e_runner_task_name,
    };
    context.runtime = runtime;
    result = h2_pal_task_start(h2_web_platform_task_api(platform),
                               &task_options, run_e2e, &context, &app_task);
  }
  while (result == H2_PAL_OK && !context.done) {
    result = h2_web_platform_pump(platform, 64u, NULL);
    if (result == H2_PAL_OK)
      h2_web_worker_sleep(1u);
  }
  if (result == H2_PAL_OK) {
    result = h2_pal_task_join(h2_web_platform_task_api(platform), app_task);
  }
  if (result == H2_PAL_OK)
    result = context.result;
  for (i = 0u; i < context.report.case_count; ++i) {
    printf("H2_LUA_E2E_CASE id=%s result=%s rc=%d evidence=%llu\n",
           context.report.cases[i].id,
           context.report.cases[i].result == H2_PAL_OK ? "PASS" : "FAIL",
           context.report.cases[i].result,
           (unsigned long long)context.report.cases[i].evidence);
  }
  printf("H2_LUA_E2E result=%s scheduler=%s passed=%zu total=%zu\n",
         result == H2_PAL_OK ? "PASS" : "FAIL",
         context.report.scheduler == NULL ? "unknown"
                                          : context.report.scheduler,
         context.report.passed, context.report.case_count);
  (void)h2_web_main_call(
      web_set_result, (const void *[]){&(int){(int)context.report.passed},
                                       &(size_t){context.report.case_count}});
  if (runtime != NULL)
    h2_runtime_deinit(runtime);
  h2_web_platform_destroy(platform);
  return result == H2_PAL_OK ? 0 : 1;
}
