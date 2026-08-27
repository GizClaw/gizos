#include "h2_pal_e2e.h"
#include "h2_smoke_host_runtime.h"
#include "h2_web_platform.h"

#include <emscripten.h>
#include <stdio.h>

typedef struct h2_web_pal_app {
  h2_runtime_t *runtime;
  h2_pal_e2e_result_t result;
  h2_pal_result_t run_result;
} h2_web_pal_app_t;

static void h2_web_pal_run(void *user) {
  h2_web_pal_app_t *app = user;
  const h2_pal_e2e_config_t config = {
      .suite_mask = H2_PAL_E2E_SUITE_CORE,
  };
  app->run_result = h2_pal_e2e_run(app->runtime, &config, &app->result);
}

EM_JS(void, h2_web_pal_result, (int result, size_t passed, size_t failed), {
  const element = globalThis.document && document.getElementById('result');
  if (element) {
    element.textContent = result === 0
      ? `PASS cases=${passed}`
      : `FAIL rc=${result} passed=${passed} failed=${failed}`;
    element.dataset.terminal = result === 0 ? 'pass' : 'fail';
  }
});

int main(void) {
  const h2_web_platform_config_t platform_config = {
      .display_width = 1,
      .display_height = 1,
  };
  h2_web_platform_t *platform = h2_web_platform_create(&platform_config);
  if (platform == NULL) {
    return 1;
  }
  h2_runtime_config_t config = h2_smoke_host_runtime_config(
      "browser", "webassembly", "wasm32", h2_web_platform_mem_api(),
      h2_web_platform_time_api(platform),
      h2_web_platform_queue_api(platform),
      h2_web_platform_display_api(platform));
  config.log = h2_web_platform_log_api();
  config.timer = h2_web_platform_timer_api(platform);
  config.task = h2_web_platform_task_api(platform);
  config.sync = h2_web_platform_sync_api(platform);
  config.touch = h2_web_platform_touch_api(platform);
  config.webrtc = h2_web_platform_webrtc_api(platform);
  config.webrtc_media_track = h2_web_platform_webrtc_audio_track(platform);
  h2_runtime_t *runtime = NULL;
  h2_pal_result_t result = h2_runtime_init(&config, &runtime);
  h2_web_pal_app_t app = {
      .runtime = runtime,
      .run_result = result,
  };
  h2_pal_task_t *task = NULL;
  if (result == H2_PAL_OK) {
    result = h2_pal_task_start(config.task, NULL, h2_web_pal_run, &app, &task);
  }
  int joined = 0;
  for (int turn = 0; result == H2_PAL_OK && turn < 128 && !joined; ++turn) {
    result = h2_web_platform_pump(platform, 16u, NULL);
    if (result != H2_PAL_OK) {
      break;
    }
    const h2_pal_result_t join_result = h2_pal_task_join(config.task, task);
    if (join_result == H2_PAL_OK) {
      joined = 1;
    } else if (join_result == H2_PAL_ERR_BUSY) {
      emscripten_sleep(1u);
    } else {
      result = join_result;
      break;
    }
  }
  if (result == H2_PAL_OK && (!joined || app.run_result != H2_PAL_OK)) {
    result = joined ? app.run_result : H2_PAL_ERR_TIMEOUT;
  }
  for (int turn = 0; app.result.retained_cleanup != NULL && turn < 128;
       ++turn) {
    const h2_pal_result_t pump_result =
        h2_web_platform_pump(platform, 16u, NULL);
    if (pump_result != H2_PAL_OK) {
      result = pump_result;
      break;
    }
    const h2_pal_result_t cleanup_result =
        h2_pal_e2e_cleanup(runtime, &app.result);
    if (cleanup_result == H2_PAL_OK) break;
    if (cleanup_result != H2_PAL_ERR_BUSY) {
      result = cleanup_result;
      break;
    }
    emscripten_sleep(1u);
  }
  if (app.result.retained_cleanup != NULL) result = H2_PAL_ERR_TIMEOUT;
  for (size_t index = 0u; index < app.result.case_count; ++index) {
    printf("H2_WEB_PAL_CASE id=%d rc=%d\n",
           app.result.cases[index].case_id, app.result.cases[index].result);
  }
  printf("H2_WEB_PAL_E2E result=%s rc=%d passed=%zu failed=%zu cleanup=%d\n",
         result == H2_PAL_OK ? "PASS" : "FAIL", result,
         app.result.passed, app.result.failed, app.result.cleanup_result);
  h2_web_pal_result(result, app.result.passed, app.result.failed);
  if (runtime != NULL && app.result.retained_cleanup == NULL) {
    h2_runtime_deinit(runtime);
  }
  if (app.result.retained_cleanup == NULL) {
    h2_web_platform_destroy(platform);
  }
  return result == H2_PAL_OK ? 0 : 1;
}
