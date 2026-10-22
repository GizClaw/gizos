#include "h2_esp_board.h"
#include "h2_mosaico_usb_console.h"
#include "h2_mosaico_loader_usb.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_target_task_policy.h"
#include "bsp/esp_mosaico.h"
#include <stdio.h>
void app_main(void) {
  bsp_board_variant_t variant;
  if (bsp_board_variant_get(&variant) != ESP_OK) return;
  if (h2_esp_target_task_policy_install() != H2_PAL_OK) return;
  if (h2_mosaico_usb_console_init() != H2_PAL_OK) return;
  if (h2_mosaico_loader_usb_init() != H2_PAL_OK) return;
  h2_runtime_config_t config;
  if (h2_esp_board_runtime_config(&config) != H2_PAL_OK) return;
  h2_runtime_t* runtime = NULL;
  if (h2_runtime_init(&config, &runtime) != H2_PAL_OK) return;
  h2_esp_h2loader_run(runtime, "esp_mosaico", NULL);
  h2_runtime_deinit(runtime);
  (void)h2_esp_board_runtime_deinit();
}
