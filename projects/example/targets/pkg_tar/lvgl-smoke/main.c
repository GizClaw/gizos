#include "h2_lvgl_smoke.h"
#include "h2_web_app_host.h"

static h2_pal_result_t run_lvgl_smoke(h2_web_app_host_t *host,
                                      h2_runtime_t *runtime, void *user) {
  (void)user;
  // The App animates forever; the host's run_ms stop cancels its task.
  const h2_lvgl_smoke_config_t config = {
      .mem = runtime->mem,
      .ready = h2_web_app_host_ready,
      .ready_user = host,
  };
  return h2_lvgl_smoke_run(runtime, &config);
}

int main(void) {
  const h2_web_app_host_config_t config = {
      .name = "lvgl-smoke",
      .display_width = 240,
      .display_height = 240,
      .run_ms = 2000u,
  };
  return h2_web_app_host_run(&config, run_lvgl_smoke, NULL);
}
