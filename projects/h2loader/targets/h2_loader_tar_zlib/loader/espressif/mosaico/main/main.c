#include "h2_esp_board.h"
#include "h2_mosaico_usb_console.h"
#include "h2_device_service.h"
#include "h2_esp_target_task_policy.h"
#include "bsp/esp_mosaico.h"
#include <stdio.h>

static h2_device_loader_t h2_loader;

static void image_entry(void* user) {
  (void)user;
  h2_runtime_config_t config;
  if (h2_esp_board_runtime_config(&config) != H2_PAL_OK) return;
  h2_runtime_t* runtime = NULL;
  if (h2_runtime_init(&config, &runtime) != H2_PAL_OK) return;
  const h2_device_loader_config_t loader_config = {
      .board = "esp_mosaico",
      .loader_partition_id = 1u,
      .app_partition_id = 2u,
      .coredump_partition_id = 3u,
      .hardware_capabilities = H2_LOADER_CAPABILITIES_ALL,
      .identity_policy = H2_DEVICE_IDENTITY_NATIVE,
      .digest_yield_ms = 1u,
  };
  const int loader_rc =
      h2_device_loader_run(&h2_loader, runtime, &loader_config, NULL, 0);
  if (loader_rc != H2_PAL_OK)
    printf("H2_LOADER_READY target=esp status=service_fail code=%d\n",
           loader_rc);
  h2_runtime_deinit(runtime);
}

void app_main(void) {
  bsp_board_variant_t variant;
  if (bsp_board_variant_get(&variant) != ESP_OK) return;
  if (h2_esp_target_task_policy_install() != H2_PAL_OK) return;
  if (h2_mosaico_usb_console_init() != H2_PAL_OK) return;
  /* The blocking install path nests archive validation and flash operations;
   * keep it off IDF's small app_main stack, as on the other ESP boards. */
  h2_pal_result_t rc =
      h2_esp_board_start_entry_task("mosaico/h2loader", image_entry, NULL);
  if (rc != H2_PAL_OK) {
    printf("H2_BOARD_ENTRY_FAIL board=esp_mosaico image=h2loader code=%d\n",
           rc);
  }
}
