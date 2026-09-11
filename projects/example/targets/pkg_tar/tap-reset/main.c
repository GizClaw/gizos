#include "h2_web_app_host.h"
#include "h2_web_platform.h"
#include "h2_web_tap_reset_app.h"

static h2_pal_result_t run_tap_reset(h2_web_app_host_t *host,
                                     h2_runtime_t *runtime, void *user) {
  (void)user;
  h2_web_platform_t *platform = h2_web_app_host_platform(host);
  h2_web_platform_install_pointer(platform);
  const h2_web_tap_reset_app_config_t config = {
      .read_pointer = h2_web_platform_read_pointer,
      .pointer_user = platform,
      .should_stop = h2_web_app_host_should_stop,
      .stop_user = host,
  };
  return h2_web_tap_reset_app_run(runtime, &config);
}

int main(void) {
  const h2_web_app_host_config_t config = {
      .name = "tap-reset",
      .display_width = H2_WEB_TAP_RESET_WIDTH,
      .display_height = H2_WEB_TAP_RESET_HEIGHT,
      .run_ms = 3000u,
      .lvgl = 1,
  };
  return h2_web_app_host_run(&config, run_tap_reset, NULL);
}
