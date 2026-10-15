#include "h2_mosaico_loader_usb.h"
#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_target_task_policy.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <stdlib.h>

static void hold(void) { for (;;) vTaskDelay(pdMS_TO_TICKS(1000u)); }
static void entry(void *unused) {
  (void)unused;
  vTaskDelay(pdMS_TO_TICKS(3000u));
  printf("H2LOADER_E2E_MEMORY phase=entry internal_free=%zu internal_largest=%zu\n",
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  h2_runtime_config_t config = {0};
  h2_runtime_t *runtime = NULL;
  int rc = h2_esp_board_runtime_config(&config);
  if (rc == H2_PAL_OK) rc = h2_esp_h2loader_app_commands_prepare_serial(&config, "h2loader-e2e", 1u, 3u);
  if (rc == H2_PAL_OK) rc = h2_runtime_init(&config, &runtime);
#ifdef H2_MOSAICO_E2E_CRASH_BEFORE_CONFIRM
  printf("H2LOADER_E2E_CRASH_BEFORE_CONFIRM version=%s setup=%d\n", esp_app_get_description()->version, rc);
  fflush(stdout);
  if (rc != H2_PAL_OK) hold();
  abort();
#else
  if (rc == H2_PAL_OK) rc = h2_esp_h2loader_app_commands_start(runtime, "h2loader-e2e", 1u, 3u);
  printf("H2LOADER_E2E_MEMORY phase=commands internal_free=%zu internal_largest=%zu\n",
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (rc == H2_PAL_OK) rc = h2_esp_h2loader_app_confirm(runtime);
  printf("H2LOADER_E2E_APP_READY version=%s status=%s rc=%d\n", esp_app_get_description()->version, rc == H2_PAL_OK ? "PASS" : "FAIL", rc);
  fflush(stdout);
  hold();
#endif
}
void app_main(void) {
  int rc = h2_mosaico_loader_usb_init();
  if (rc == H2_PAL_OK) rc = h2_esp_target_task_policy_install();
  if (rc != H2_PAL_OK) { printf("H2LOADER_E2E_SETUP_FAIL rc=%d\n", rc); fflush(stdout); return; }
  /* Keep the 64 KiB artifact entry stack in PSRAM, matching Loader and the
   * suite entries. SDK flash/confirmation operations use their internal safe
   * call stacks; Wi-Fi+BLE still need internal DMA-capable receive memory. */
  if (xTaskCreatePinnedToCoreWithCaps(entry, "mosaico/loader-e2e", 65536u, NULL,
      tskIDLE_PRIORITY + 4u, NULL, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    printf("H2LOADER_E2E_SETUP_FAIL stage=task rc=%d\n", H2_PAL_ERR_TASK);
    fflush(stdout);
  }
}
