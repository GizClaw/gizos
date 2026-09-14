#include "h2_example_raster2d.h"
#include "h2_web_app_host.h"

static h2_pal_result_t run_raster2d(h2_web_app_host_t *host,
                                    h2_runtime_t *runtime, void *user) {
  (void)host;
  (void)user;
  return h2_example_raster2d_run(runtime);
}

int main(void) {
  const h2_web_app_host_config_t config = {
      .name = "raster2d", .display_width = 240, .display_height = 240};
  return h2_web_app_host_run(&config, run_raster2d, NULL);
}
