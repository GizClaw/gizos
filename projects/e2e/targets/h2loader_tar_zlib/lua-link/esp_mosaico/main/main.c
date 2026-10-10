#include "h2_mosaico_loader_usb.h"
#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_platform_core.h"
#include "h2_esp_platform_safe_call.h"
#include "esp_ota_ops.h"
#include "h2_esp_target_task_policy.h"
#include "h2_lua_link_e2e.h"

#include "esp_system.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"

#include <stdio.h>

/* The host entry defaults to host; the peer artifact compiles the same source
 * with the opposite role. Both execute the unchanged portable script. */
#define H2_LUA_LINK_E2E_ROUNDS 5u
#ifndef H2_LUA_LINK_E2E_ROLE
#define H2_LUA_LINK_E2E_ROLE "host"
#endif

typedef struct ota_observation {
  uint32_t running;
  uint32_t next;
  int loader_state;
  int app_state;
  int loader_state_rc;
  int app_state_rc;
} ota_observation_t;
static void observe_ota(void *user) {
  ota_observation_t *out = user;
  const esp_partition_t *p = esp_ota_get_running_partition();
  out->running = p != NULL ? p->address : 0;
  p = esp_ota_get_boot_partition();
  out->next = p != NULL ? p->address : 0;
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
  out->loader_state_rc = p != NULL ? esp_ota_get_state_partition(p, &state) : ESP_ERR_NOT_FOUND;
  out->loader_state = state;
  state = ESP_OTA_IMG_UNDEFINED;
  p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
  out->app_state_rc = p != NULL ? esp_ota_get_state_partition(p, &state) : ESP_ERR_NOT_FOUND;
  out->app_state = state;
}
static void report_ota(const char *phase) {
  ota_observation_t observation = {0};
  int rc = h2_esp_platform_safe_call(observe_ota, &observation, sizeof(observation), 4096u);
  printf("H2_LUA_OTA_TRACE phase=%s rc=%d running=%lx next=%lx p1_state=%d p2_state=%d p1_rc=%d p2_rc=%d\n",
         phase, rc, (unsigned long)observation.running, (unsigned long)observation.next,
         observation.loader_state, observation.app_state,
         observation.loader_state_rc, observation.app_state_rc);
  fflush(stdout);
}
/* Observe the normal shared reboot path; do not change its selection or result. */
int h2_esp_platform_power_before_reboot(uint32_t reason) {
  (void)reason;
  report_ota("before-reboot");
  return H2_PAL_OK;
}

/* Diagnostic entry owns reliable console delivery, not the portable script. */
static const h2_pal_log_api_t *board_log;
static int observed_log(void *user, h2_pal_log_level_t level,
                        const char *scope, const char *message) {
  (void)user;
  int rc = h2_pal_log_write(board_log, level, scope, message);
  if (fflush(stdout) != 0 && rc == H2_PAL_OK) rc = H2_PAL_ERR_IO;
  return rc;
}
static const h2_pal_log_vtable_t observed_log_vtable = {.write = observed_log};
static const h2_pal_log_api_t observed_log_api = {.vtable = &observed_log_vtable};

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
  board_log = config.log;
  config.log = &observed_log_api;
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
  /* Several sessions in a row: each round opens and closes the link, so the
   * retained GATT service is reattached on every host round. */
  unsigned passed = 0u;
  for (unsigned round = 1u; round <= H2_LUA_LINK_E2E_ROUNDS; ++round) {
    printf("H2_LUA_LINK_E2E stage=round board=%s role=%s round=%u\n",
           "esp_mosaico", H2_LUA_LINK_E2E_ROLE, round);
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
         "esp_mosaico", H2_LUA_LINK_E2E_ROLE, passed, H2_LUA_LINK_E2E_ROUNDS);
  fflush(stdout);
  if (passed != H2_LUA_LINK_E2E_ROUNDS)
    fail("rounds", H2_PAL_ERR_INVALID_STATE, 1);
  rc = h2_esp_h2loader_app_confirm(runtime);
  if (rc != H2_PAL_OK)
    fail("confirm", rc, 1);
  printf("H2_LUA_LINK_E2E stage=confirmed role=%s rc=0\n",
         H2_LUA_LINK_E2E_ROLE);
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

/* Artifact-owned startup stack policy; no board-owned application task. */
static void entry_task(void *unused) {
  (void)unused;
  vTaskDelay(pdMS_TO_TICKS(3000u));
  printf("H2_LUA_LINK_E2E stage=boot board=esp_mosaico role=%s version=%s\n",
         H2_LUA_LINK_E2E_ROLE, esp_app_get_description()->version);
  fflush(stdout);
  report_ota("boot");
  image_entry(NULL);
  (void)h2_esp_board_runtime_deinit();
  vTaskDeleteWithCaps(NULL);
}
void app_main(void) {
  int usb_rc = h2_mosaico_loader_usb_init();
  if (usb_rc != H2_PAL_OK) {
    printf("H2_MOSAICO_USB_FAIL rc=%d\n", usb_rc);
    return;
  }
  if (h2_esp_target_task_policy_install() != H2_PAL_OK) {
    return;
  }
  const h2_pal_result_t rc =
      (xTaskCreatePinnedToCoreWithCaps(entry_task, "esp_mosaico/lua-link-e2e", 65536u, NULL,
          tskIDLE_PRIORITY + 4u, NULL, tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
       == pdPASS ? H2_PAL_OK : H2_PAL_ERR_TASK);
  if (rc != H2_PAL_OK) {
    fail("entry_task", rc, 0);
  }
}
