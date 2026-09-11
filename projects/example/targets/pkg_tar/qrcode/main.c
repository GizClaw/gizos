#include "h2_qrcode_example.h"
#include "h2_web_app_host.h"

static h2_pal_result_t run_qrcode(h2_web_app_host_t *host,
                                  h2_runtime_t *runtime, void *user) {
  (void)user;
  const h2_qrcode_example_config_t config = {
      .text = "https://github.com/GizClaw/gizos",
      .on_ready = h2_web_app_host_ready,
      .on_ready_user = host,
  };
  return h2_qrcode_example_run(runtime, &config);
}

int main(void) {
  const h2_web_app_host_config_t config = {
      .name = "qrcode",
      .display_width = 240,
      .display_height = 240,
  };
  return h2_web_app_host_run(&config, run_qrcode, NULL);
}
