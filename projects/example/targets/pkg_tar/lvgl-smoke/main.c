#include "h2_lvgl_smoke.h"
#include "h2_smoke_host_runtime.h"
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
static int should_stop(void* user) {
  (void)user;
  return h2_web_main_call(page_stop, NULL).i32 ||
         (stop_at > 0.0 && emscripten_get_now() >= stop_at);
}

static h2_pal_result_t ready(void* user) {
  (void)user;
  puts("H2_WEB_APP name=lvgl-smoke stage=ready");
  return H2_PAL_OK;
}

static h2_runtime_t* app_runtime;
static int app_result;
static void run_app(void* user) {
  (void)user;
  const h2_lvgl_smoke_config_t app = {.mem = app_runtime->mem, .ready = ready};
  app_result = h2_lvgl_smoke_run(app_runtime, &app);
}
int main(void) {
  const h2_web_platform_config_t config = {240, 240};
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
  h2_runtime_t* runtime = NULL;
  int rc = h2_runtime_init(&rcfg, &runtime);
  h2_pal_task_t* task = NULL;
  if (rc == H2_PAL_OK) {
    app_runtime = runtime;
    const h2_pal_task_options_t options = {.name = "lvgl-smoke",
                                           .min_stack_size = 65536u};
    stop_at = emscripten_get_now() + 2000.0;
    rc = h2_pal_task_start(runtime->task, &options, run_app, NULL, &task);
    while (rc == H2_PAL_OK && !should_stop(NULL))
      rc = h2_pal_time_sleep_ms(runtime->time, 10u);
    if (task != NULL) {
      int cancelled = h2_web_platform_task_cancel(platform, task);
      int joined = h2_pal_task_join(runtime->task, task);
      if (joined != H2_PAL_OK) {
        printf("H2_WEB_APP name=lvgl-smoke result=FAIL rc=%d retained=1\n",
               joined);
        return 1;
      }
      if (rc == H2_PAL_OK) rc = cancelled;
      if (rc == H2_PAL_OK && app_result != H2_PAL_EXIT) rc = app_result;
    }
    app_runtime = NULL;
  }
  int cleanup = runtime != NULL ? h2_runtime_input_stop(runtime) : H2_PAL_OK;
  if (cleanup != H2_PAL_OK) {
    printf("H2_WEB_APP name=lvgl-smoke result=FAIL rc=%d retained=1\n",
           cleanup);
    return 1;
  }
  h2_runtime_deinit(runtime);
  int destroyed = h2_web_platform_destroy(platform);
  if (rc == H2_PAL_OK) rc = cleanup != H2_PAL_OK ? cleanup : destroyed;
  printf("H2_WEB_APP name=lvgl-smoke result=%s rc=%d fs=0 destroy=%d\n",
         rc == H2_PAL_OK ? "PASS" : "FAIL", rc, destroyed);
  return rc == H2_PAL_OK ? 0 : 1;
}
