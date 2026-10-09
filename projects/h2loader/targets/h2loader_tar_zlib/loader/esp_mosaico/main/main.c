#include "h2_esp_board.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_target_task_policy.h"
#include "esp_log.h"

void app_main(void) {
    if (h2_esp_target_task_policy_install() != H2_PAL_OK) return;
    h2_runtime_config_t config;
    h2_runtime_t *runtime = NULL;
    h2_pal_result_t rc = h2_esp_board_runtime_config(&config);
    if (rc == H2_PAL_OK) rc = h2_runtime_init(&config, &runtime);
    if (rc != H2_PAL_OK) {
        ESP_LOGE("mosaico", "Runtime startup failed: %d", rc);
        (void)h2_esp_board_runtime_deinit();
        return;
    }
    h2_esp_h2loader_run(runtime, "esp_mosaico", NULL);
}
