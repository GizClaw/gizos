#include "h2_esp_board.h"
#include "h2_device_service.h"
#include "h2_esp_target_task_policy.h"

#include <stdio.h>

static h2_device_loader_t h2_loader;

static void image_entry(void* user) {
  (void)user;
  h2_runtime_config_t runtime_config;
  h2_runtime_t* runtime = NULL;
  h2_pal_result_t rc = h2_esp_board_runtime_config(&runtime_config);
  if (rc != H2_PAL_OK) {
    printf("H2_LOADER_READY target=esp status=runtime_config_fail code=%d\n",
           rc);
    return;
  }
  rc = h2_runtime_init(&runtime_config, &runtime);
  if (rc != H2_PAL_OK) {
    printf("H2_LOADER_READY target=esp status=runtime_init_fail code=%d\n", rc);
    (void)h2_esp_board_runtime_deinit();
    return;
  }
  const h2_device_loader_config_t loader_config = {
      .loader_partition_id = 1u,
      .app_partition_id = 2u,
      .coredump_partition_id = 3u,
      .hardware_capabilities = H2_LOADER_CAPABILITIES_ALL,
      .identity_policy = H2_DEVICE_IDENTITY_NATIVE,
      .digest_yield_ms = 1u,
      .board = "waveshare_esp32p4_wifi6_touch_lcd_4_3",
  };
  const int loader_rc =
      h2_device_loader_run(&h2_loader, runtime, &loader_config, NULL, 1);
  if (loader_rc != H2_PAL_OK)
    printf("H2_LOADER_READY target=esp status=service_fail code=%d\n",
           loader_rc);
}

void app_main(void) {
  if (h2_esp_target_task_policy_install() != H2_PAL_OK) {
    return;
  }
  h2_pal_result_t rc = h2_esp_board_start_entry_task(
      "waveshare_esp32p4_wifi6_touch_lcd_4_3/h2loader", image_entry, NULL);
  if (rc != H2_PAL_OK) {
    printf(
        "H2_BOARD_ENTRY_FAIL board=waveshare_esp32p4_wifi6_touch_lcd_4_3 image=h2loader code=%d\n",
        rc);
  }
}
