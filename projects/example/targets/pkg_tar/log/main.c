#include "h2_log_example.h"
#include "h2_smoke_host_runtime.h"
#include "h2_web_platform.h"
#include <stdio.h>

int main(void) {
  const h2_web_platform_config_t config = {1, 1};
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
    puts("H2_WEB_APP name=log stage=running");
    rc = h2_log_example_run(runtime);
  }
  int cleanup = runtime != NULL ? h2_runtime_input_stop(runtime) : H2_PAL_OK;
  if (cleanup != H2_PAL_OK) {
    printf("H2_WEB_APP name=log result=FAIL rc=%d retained=1\n", cleanup);
    return 1;
  }
  h2_runtime_deinit(runtime);
  int destroyed = h2_web_platform_destroy(platform);
  if (rc == H2_PAL_OK)
    rc = cleanup != H2_PAL_OK ? cleanup : destroyed;
  printf("H2_WEB_APP name=log result=%s rc=%d fs=0 destroy=%d\n",
         rc == H2_PAL_OK ? "PASS" : "FAIL", rc, destroyed);
  return rc == H2_PAL_OK ? 0 : 1;
}
