#include "esp_log.h"
#include "h2_chip_board.h"
#include "h2_esp_target_task_policy.h"
#include "h2_lua_platform_smoke.h"

void app_main(void) {
  h2_runtime_config_t config = {0};
  h2_runtime_t* runtime = NULL;
  h2_pal_result_t rc = h2_esp_target_task_policy_install();
  if (rc == H2_PAL_OK) rc = h2_chip_board_runtime_config(&config);
  config.input_source_capacity = 3;
  config.component_mapping_capacity = 3;
  config.event_queue_capacity = 8;
  if (rc == H2_PAL_OK) rc = h2_runtime_init(&config, &runtime);
  if (rc == H2_PAL_OK) rc = h2_runtime_input_start(runtime, NULL);
  if (rc == H2_PAL_OK) rc = h2_lua_platform_smoke_run(runtime);
  ESP_LOGI("C3_SMOKE", "runtime_lua_result=%d", rc);
  if (runtime != NULL) h2_runtime_deinit(runtime);
}
