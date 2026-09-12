#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_platform_core.h"
#include "h2_esp_target_task_policy.h"
#include "h2_lua_link_e2e.h"

#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>

/* The amoled runs the join side; the other board runs the opposite role. */
#define H2_LUA_LINK_E2E_ROUNDS 5u
#define H2_LUA_LINK_E2E_ROLE "join"

static void hold(void) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000u));
  }
}

static void fail(const char *stage, int rc, int commands_started) {
  printf("H2_LUA_LINK_E2E stage=%s status=ERROR rc=%d\n", stage, rc);
  fflush(stdout);
  if (!commands_started) {
    esp_restart();
  }
  hold();
}

static void image_entry(void *user) {
  h2_runtime_config_t config = {0};
  h2_runtime_t *runtime = NULL;
  (void)user;
  int rc = h2_esp_board_runtime_config(&config);
  if (rc != H2_PAL_OK) {
    fail("runtime_config", rc, 0);
  }
  config.mem = h2_esp_platform_psram_allocator();
  rc = h2_esp_h2loader_app_commands_prepare_serial(&config, "lua-link-e2e", 1u,
                                                   3u);
  if (rc != H2_PAL_OK) {
    fail("command_prepare", rc, 0);
  }
  rc = h2_runtime_init(&config, &runtime);
  if (rc != H2_PAL_OK) {
    fail("runtime_init", rc, 0);
  }
  /* Starts the BLE Host for the H2Loader command service; the link uses the
   * same Host. Wi-Fi is never started. */
  rc = h2_esp_h2loader_app_commands_start(runtime, "lua-link-e2e", 1u, 3u);
  if (rc != H2_PAL_OK) {
    fail("command_start", rc, 0);
  }
  rc = h2_esp_h2loader_app_confirm(runtime);
  if (rc != H2_PAL_OK) {
    fail("confirm", rc, 1);
  }
  /* Several sessions in a row: each round opens and closes the link, so the
   * retained GATT service is reattached on every host round. */
  unsigned passed = 0u;
  for (unsigned round = 1u; round <= H2_LUA_LINK_E2E_ROUNDS; ++round) {
    printf("H2_LUA_LINK_E2E stage=round board=%s role=%s round=%u\n",
           "amoled", H2_LUA_LINK_E2E_ROLE, round);
    fflush(stdout);
    rc = h2_lua_link_e2e_run(runtime,
                             &(h2_lua_link_e2e_config_t){
                                 .role = H2_LUA_LINK_E2E_ROLE,
                                 .adv_type = H2_PAL_BLE_ADV_TYPE_EXTENDED,
                                 .scan_type = H2_PAL_BLE_SCAN_TYPE_EXTENDED,
                             });
    passed += rc == H2_PAL_OK;
    vTaskDelay(pdMS_TO_TICKS(3000u));
  }
  printf("H2_LUA_LINK_E2E stage=summary board=%s role=%s passed=%u rounds=%u\n",
         "amoled", H2_LUA_LINK_E2E_ROLE, passed, H2_LUA_LINK_E2E_ROUNDS);
  fflush(stdout);
  /* Final session stays up until the link drops (e.g. the peer resets). */
  (void)h2_lua_link_e2e_run(runtime, &(h2_lua_link_e2e_config_t){
                                         .role = H2_LUA_LINK_E2E_ROLE,
                                         .adv_type = H2_PAL_BLE_ADV_TYPE_EXTENDED,
                                         .scan_type = H2_PAL_BLE_SCAN_TYPE_EXTENDED,
                                         .hold = 1,
                                     });
  hold();
}

void app_main(void) {
  if (h2_esp_target_task_policy_install() != H2_PAL_OK) {
    return;
  }
  const h2_pal_result_t rc =
      h2_esp_board_start_entry_task("amoled/lua-link-e2e", image_entry, NULL);
  if (rc != H2_PAL_OK) {
    fail("entry_task", rc, 0);
  }
}
