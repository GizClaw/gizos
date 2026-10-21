#include "h2_lvgl_platform.h"
#include "h2_smoke_host_runtime.h"
#include "h2_web_main_thread.h"
#include "h2_web_platform.h"
#include "h2_web_tap_reset_app.h"
#include <emscripten.h>
#include <stdio.h>

static double stop_at;
/* clang-format off */
EM_JS(void, page_stop,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, [], "i32", () => Module.h2WebStopRequested ? 1 : 0);
});
/* clang-format on */
static int should_stop(void* user) {
  (void)user;
  return h2_web_main_call(page_stop, NULL).i32 ||
         (stop_at > 0.0 && emscripten_get_now() >= stop_at);
}

int main(void) {
  const h2_web_platform_config_t config = {H2_WEB_TAP_RESET_WIDTH,
                                           H2_WEB_TAP_RESET_HEIGHT};
  h2_web_platform_t* platform = h2_web_platform_create(&config);
  if (platform == NULL) return 1;
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
  const h2_lvgl_platform_config_t lvgl = {.allocator = rcfg.mem,
                                          .task_api = rcfg.task,
                                          .sync_api = rcfg.sync,
                                          .queue_api = rcfg.queue,
                                          .time_api = rcfg.time};
  int rc = h2_lvgl_platform_init(&lvgl);
  h2_runtime_t* runtime = NULL;
  if (rc == H2_PAL_OK) rc = h2_runtime_init(&rcfg, &runtime);
  if (rc == H2_PAL_OK) {
    stop_at = emscripten_get_now() + 3000.0;
    h2_web_platform_install_pointer(platform);
    const h2_web_tap_reset_app_config_t app = {
        .read_pointer = h2_web_platform_read_pointer,
        .pointer_user = platform,
        .should_stop = should_stop};
    puts("H2_WEB_APP name=tap-reset stage=running");
    rc = h2_web_tap_reset_app_run(runtime, &app);
  }
  h2_lvgl_platform_deinit();
  int cleanup = runtime != NULL ? h2_runtime_input_stop(runtime) : H2_PAL_OK;
  if (cleanup != H2_PAL_OK) {
    printf("H2_WEB_APP name=tap-reset result=FAIL rc=%d retained=1\n", cleanup);
    return 1;
  }
  h2_runtime_deinit(runtime);
  int destroyed = h2_web_platform_destroy(platform);
  if (rc == H2_PAL_OK) rc = cleanup != H2_PAL_OK ? cleanup : destroyed;
  printf("H2_WEB_APP name=tap-reset result=%s rc=%d fs=0 destroy=%d\n",
         rc == H2_PAL_OK ? "PASS" : "FAIL", rc, destroyed);
  return rc == H2_PAL_OK ? 0 : 1;
}
