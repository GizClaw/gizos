#include "h2_lvgl_smoke.h"
#include "h2_web_application_host.h"

static h2_pal_result_t run_lvgl_smoke(h2_application_host_t *host,
                                      h2_runtime_t *runtime, void *user) {
  (void)user;
  // Cooperative stop releases the LVGL objects before the Host tears down.
  const h2_lvgl_smoke_config_t config = {
      .mem = runtime->mem,
      .ready = h2_application_host_ready,
      .ready_user = host,
      .should_stop = h2_application_host_should_stop,
      .stop_user = host,
  };
  return h2_lvgl_smoke_run(runtime, &config);
}

int main(void) {
  const h2_web_application_host_config_t config = {
      .name = "lvgl-smoke",
      .display_width = 240,
      .display_height = 240,
      .run_ms = 2000u,
  };
  return h2_web_application_host_run(&config, run_lvgl_smoke, NULL);
}
