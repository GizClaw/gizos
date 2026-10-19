#include "esp_app_desc.h"
#include "h2_mosaico_loader_usb.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_platform_core.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_lua_runtime_e2e.h"
#include "h2_esp_target_task_policy.h"

#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>

static void hold_for_recovery(void) {
  for (;;)
    vTaskDelay(pdMS_TO_TICKS(1000u));
}

static void report_case(void *user,
                        const h2_lua_runtime_e2e_case_result_t *case_result) {
  (void)user;
  printf("H2_LUA_E2E_CASE id=%s result=%s rc=%d evidence=%llu\n",
         case_result->id, case_result->result == H2_PAL_OK ? "PASS" : "FAIL",
         case_result->result, (unsigned long long)case_result->evidence);
}

static void image_entry(void *user) {
  h2_runtime_config_t config = {0};
  h2_runtime_t *runtime = NULL;
  h2_lua_runtime_e2e_report_t report = {0};
  h2_pal_result_t result;
  (void)user;
  printf("H2_LUA_E2E_BOOT board=esp_mosaico version=%s\n",
         esp_app_get_description()->version);
  result = h2_esp_board_runtime_config(&config);
  config.mem = h2_esp_platform_psram_allocator();
  config.periph = h2_lua_runtime_e2e_periph_api();
  config.component_mapper = h2_lua_runtime_e2e_component_mapper();
  if (result == H2_PAL_OK) {
    result = h2_esp_h2loader_app_commands_prepare_serial(
        &config, "lua-runtime-e2e", 1u, 3u);
  }
  if (result == H2_PAL_OK)
    result = h2_runtime_init(&config, &runtime);
  if (result == H2_PAL_OK) {
    result =
        h2_esp_h2loader_app_commands_start(runtime, "lua-runtime-e2e", 1u, 3u);
  }
  if (result == H2_PAL_OK) {
    printf("H2_LUA_E2E_START scheduler=multi-worker total=%u\n",
           (unsigned)H2_LUA_RUNTIME_E2E_CASE_COUNT);
    result = h2_lua_runtime_e2e_run(runtime,
                                    &(h2_lua_runtime_e2e_config_t){
                                        .scheduler = "multi-worker",
                                        .worker_count = 2u,
                                        .report_case = report_case,
                                    },
                                    &report);
  }
  printf("H2_LUA_E2E result=%s scheduler=%s passed=%u total=%u\n",
         result == H2_PAL_OK ? "PASS" : "FAIL",
         report.scheduler == NULL ? "unknown" : report.scheduler,
         (unsigned)report.passed, (unsigned)report.case_count);
  if (result == H2_PAL_OK)
    result = h2_esp_platform_confirm_running_app();
  if (result == H2_PAL_OK)
    result = h2_esp_h2loader_app_confirm(runtime);
  printf("H2_LUA_E2E_READY board=esp_mosaico version=%s rc=%d\n",
         esp_app_get_description()->version, (int)result);
  fflush(stdout);
  if (result != H2_PAL_OK && runtime == NULL)
    esp_restart();
  hold_for_recovery();
}

static void board_entry(void *unused) {
  image_entry(unused);
  (void)h2_esp_board_runtime_deinit();
  vTaskDeleteWithCaps(NULL);
}

/* The launcher owns its startup task; the public board owns no App task. */
void app_main(void) {
  int rc = h2_mosaico_loader_usb_init();
  if (rc != H2_PAL_OK) {
    printf("H2_MOSAICO_USB_FAIL rc=%d\n", rc);
    return;
  }
  vTaskDelay(pdMS_TO_TICKS(3000u));
  rc = h2_esp_target_task_policy_install();
  if (rc != H2_PAL_OK) {
    printf("H2_MOSAICO_TASK_POLICY_FAIL rc=%d\n", rc);
    return;
  }
  if (xTaskCreatePinnedToCoreWithCaps(board_entry, "esp_mosaico/lua-runtime",
          65536u, NULL, tskIDLE_PRIORITY + 4u, NULL, tskNO_AFFINITY,
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    printf("H2_MOSAICO_ENTRY_FAIL rc=%d\n", H2_PAL_ERR_TASK);
  }
}
