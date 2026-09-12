#include "h2_touch_smoke.h"
#include "h2_web_app_host.h"

// The action Button is component 1 on every touch-smoke target.
static const h2_web_app_host_button_t k_buttons[] = {{1u, "Enter", NULL}};

static h2_pal_result_t run_touch(h2_web_app_host_t *host,
                                 h2_runtime_t *runtime, void *user) {
  (void)user;
  const h2_touch_smoke_config_t config = {
      .width = 800u,
      .height = 480u,
      .should_stop = h2_web_app_host_should_stop,
      .stop_user = host,
  };
  return h2_touch_smoke_run(runtime, &config);
}

int main(void) {
  const h2_web_app_host_config_t config = {
      .name = "touch",
      .display_width = 800,
      .display_height = 480,
      .run_ms = 4000u,
      .buttons = k_buttons,
      .button_count = 1u,
  };
  return h2_web_app_host_run(&config, run_touch, NULL);
}
