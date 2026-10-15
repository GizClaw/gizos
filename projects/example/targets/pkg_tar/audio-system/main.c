#include "h2_smoke_audio_system.h"
#include "h2_smoke_host_runtime.h"
#include "h2_web_fs.h"
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
  const char *const readonly[] = {"/assets"};
  const h2_web_fs_config_t fs_config = {.persistent_root = "/data",
                                        .readonly_roots = readonly,
                                        .readonly_root_count = 1u};
  h2_web_fs_t *fs = NULL;
  h2_runtime_t *runtime = NULL;
  int rc = h2_web_fs_open(platform, &fs_config, &fs);
  rcfg.fs = h2_web_fs_api(fs);
  rcfg.audio_decoder = h2_web_platform_audio_decoder_api(platform);
  if (rc == H2_PAL_OK)
    rc = h2_runtime_init(&rcfg, &runtime);
  if (rc == H2_PAL_OK) {
    const h2_smoke_audio_system_config_t app = {
        .music_path = "/assets/audio/music_loop.ogg"};
    rc = h2_smoke_audio_system_run(runtime, &app);
    printf("H2_WEB_AUDIO_SYSTEM run rc=%d\n", rc);
    stop_at = emscripten_get_now() + 4000.0;
    if (rc == H2_PAL_OK)
      puts("H2_WEB_APP name=audio-system stage=running");
    while (rc == H2_PAL_OK && !should_stop(NULL))
      rc = h2_pal_time_sleep_ms(runtime->time, 20u);
    int stopped = h2_smoke_audio_system_stop();
    printf("H2_WEB_AUDIO_SYSTEM stop rc=%d\n", stopped);
    if (stopped != H2_PAL_OK) {
      printf("H2_WEB_APP name=audio-system result=FAIL rc=%d retained=1\n",
             stopped);
      return 1;
    }
  }
  int cleanup = runtime != NULL ? h2_runtime_input_stop(runtime) : H2_PAL_OK;
  if (cleanup != H2_PAL_OK) {
    printf("H2_WEB_APP name=audio-system result=FAIL rc=%d retained=1\n",
           cleanup);
    return 1;
  }
  h2_runtime_deinit(runtime);
  int fs_result = h2_web_fs_close(fs);
  if (fs_result == H2_PAL_ERR_BUSY || fs_result == H2_PAL_ERR_TIMEOUT) {
    printf("H2_WEB_APP name=audio-system result=FAIL rc=%d retained=1\n",
           fs_result);
    return 1;
  }
  int destroyed = h2_web_platform_destroy(platform);
  if (rc == H2_PAL_OK)
    rc = cleanup != H2_PAL_OK ? cleanup : fs_result;
  if (rc == H2_PAL_OK)
    rc = destroyed;
  printf("H2_WEB_APP name=audio-system result=%s rc=%d fs=%d destroy=%d\n",
         rc == H2_PAL_OK ? "PASS" : "FAIL", rc, fs_result, destroyed);
  return rc == H2_PAL_OK ? 0 : 1;
}
