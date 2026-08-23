#include "h2_smoke_host_runtime.h"
#include "h2_web_platform.h"
#include "h2_web_tap_reset_app.h"

#include <emscripten.h>

EM_JS(void, web_set_status, (const char *status), {
  const element = document.getElementById('status');
  if (element) {
    element.textContent = UTF8ToString(status);
  }
});

static int web_should_stop(void *user) {
  (void)user;
  return 0;
}

int main(void) {
  const h2_web_platform_config_t platform_config = {
      .display_width = H2_WEB_TAP_RESET_WIDTH,
      .display_height = H2_WEB_TAP_RESET_HEIGHT,
  };
  h2_web_platform_t *host = h2_web_platform_create(&platform_config);
  if (host == NULL) {
    web_set_status("Host allocation failed");
    return 1;
  }
  h2_runtime_t *runtime = NULL;
  h2_runtime_config_t runtime_config = h2_smoke_host_runtime_config(
      "browser", "webassembly", "wasm32", h2_web_platform_mem_api(),
      h2_web_platform_time_api(host), h2_web_platform_queue_api(host),
      h2_web_platform_display_api(host));
  runtime_config.log = h2_web_platform_log_api();
  runtime_config.timer = h2_web_platform_timer_api(host);
  runtime_config.task = h2_web_platform_task_api(host);
  runtime_config.sync = h2_web_platform_sync_api(host);
  runtime_config.touch = h2_web_platform_touch_api(host);
  if (h2_runtime_init(&runtime_config, &runtime) != H2_PAL_OK) {
    web_set_status("Runtime initialization failed");
    h2_web_platform_destroy(host);
    return 1;
  }
  h2_web_platform_install_pointer(host);
  web_set_status("Running portable LVGL App in WebAssembly");
  const h2_web_tap_reset_app_config_t config = {
      .read_pointer = h2_web_platform_read_pointer,
      .pointer_user = host,
      .should_stop = web_should_stop,
      .stop_user = NULL,
  };
  const h2_pal_result_t result = h2_web_tap_reset_app_run(runtime, &config);
  h2_runtime_deinit(runtime);
  h2_web_platform_destroy(host);
  web_set_status(result == H2_PAL_OK ? "Stopped" : "Portable App failed");
  return result == H2_PAL_OK ? 0 : 1;
}
