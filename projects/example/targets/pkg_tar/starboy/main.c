#include "h2_smoke_host_runtime.h"
#include "h2_starboy.h"
#include "h2_web_main_thread.h"
#include "h2_web_platform.h"
#include <emscripten.h>
#include <stdio.h>

static double stop_at;
/* clang-format off */
EM_JS(void, page_stop,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, [], "i32", () => Module.h2WebStopRequested ? 1 : 0);
});
/* clang-format on */
static int should_stop(void *user) {
  (void)user;
  return h2_web_main_call(page_stop, NULL).i32 ||
         (stop_at > 0.0 && emscripten_get_now() >= stop_at);
}

static h2_pal_result_t ready(void *user) {
  (void)user;
  puts("H2_WEB_APP name=starboy stage=ready");
  return H2_PAL_OK;
}

int main(void) {
  const h2_web_platform_config_t config = {240, 240};
  h2_web_platform_t *platform = h2_web_platform_create(&config);
  if (platform == NULL)
    return 1;
  h2_runtime_config_t rcfg = h2_smoke_host_runtime_config(
      "browser", "webassembly", "wasm32", h2_web_platform_mem_api(),
      h2_web_platform_time_api(platform), h2_web_platform_queue_api(platform),
      h2_web_platform_display_api(platform));
  rcfg.log = h2_web_platform_log_api();
  rcfg.task = h2_web_platform_task_api(platform);
  rcfg.sync = h2_web_platform_sync_api(platform);
  rcfg.timer = h2_web_platform_timer_api(platform);
  rcfg.touch = h2_web_platform_touch_api(platform);
  rcfg.audio = h2_web_platform_audio_api(platform);
  h2_runtime_t *runtime = NULL;
  int rc = h2_runtime_init(&rcfg, &runtime);
  if (rc == H2_PAL_OK) {
    stop_at = emscripten_get_now() + 3000.0;
    const h2_starboy_config_t app = {.frame_interval_ms = 16u,
                                     .initial_pupil_style =
                                         H2_STARBOY_PUPIL_STYLE_ACORN,
                                     .ready = ready,
                                     .should_stop = should_stop};
    puts("H2_WEB_APP name=starboy stage=running");
    rc = h2_starboy_run(runtime, &app);
  }
  int cleanup = runtime != NULL ? h2_runtime_input_stop(runtime) : H2_PAL_OK;
  if (cleanup != H2_PAL_OK) {
    printf("H2_WEB_APP name=starboy result=FAIL rc=%d retained=1\n", cleanup);
    return 1;
  }
  h2_runtime_deinit(runtime);
  int destroyed = h2_web_platform_destroy(platform);
  if (rc == H2_PAL_OK)
    rc = cleanup != H2_PAL_OK ? cleanup : destroyed;
  printf("H2_WEB_APP name=starboy result=%s rc=%d fs=0 destroy=%d\n",
         rc == H2_PAL_OK ? "PASS" : "FAIL", rc, destroyed);
  return rc == H2_PAL_OK ? 0 : 1;
}
