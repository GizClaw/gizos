#include "h2_starboy.h"
#include "h2_web_app_host.h"

static h2_pal_result_t run_starboy(h2_web_app_host_t *host,
                                   h2_runtime_t *runtime, void *user) {
  (void)user;
  const h2_starboy_config_t config = {
      .frame_interval_ms = 16u,
      .initial_pupil_style = H2_STARBOY_PUPIL_STYLE_ACORN,
      .ready = h2_web_app_host_ready,
      .ready_user = host,
      .should_stop = h2_web_app_host_should_stop,
      .should_stop_user = host,
  };
  return (h2_pal_result_t)h2_starboy_run(runtime, &config);
}

int main(void) {
  const h2_web_app_host_config_t config = {
      .name = "starboy",
      .display_width = 240,
      .display_height = 240,
      .run_ms = 3000u,
  };
  return h2_web_app_host_run(&config, run_starboy, NULL);
}
