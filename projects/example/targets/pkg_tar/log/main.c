#include "h2_log_example.h"
#include "h2_web_app_host.h"

static h2_pal_result_t run_log(h2_web_app_host_t *host, h2_runtime_t *runtime,
                               void *user) {
  (void)host;
  (void)user;
  return (h2_pal_result_t)h2_log_example_run(runtime);
}

int main(void) {
  const h2_web_app_host_config_t config = {
      .name = "log",
      .display_width = 1,
      .display_height = 1,
  };
  return h2_web_app_host_run(&config, run_log, NULL);
}
