#include "h2_smoke_display.h"
#include "h2_web_app_host.h"

static h2_pal_result_t run_display(h2_web_app_host_t *host,
                                   h2_runtime_t *runtime, void *user) {
  (void)host;
  (void)user;
  return (h2_pal_result_t)h2_smoke_display_run(runtime);
}

int main(void) {
  const h2_web_app_host_config_t config = {
      .name = "display",
      .display_width = 240,
      .display_height = 240,
  };
  return h2_web_app_host_run(&config, run_display, NULL);
}
