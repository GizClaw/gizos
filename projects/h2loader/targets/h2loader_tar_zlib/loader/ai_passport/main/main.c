#include "h2_esp_board.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_target_task_policy.h"

#include <stdio.h>

void app_main(void) {
  int rc = h2_esp_target_task_policy_install();
  h2_runtime_config_t config;
  h2_runtime_t* runtime = NULL;
  if (rc == 0) rc = h2_esp_board_runtime_config(&config);
  if (rc == 0) {
    config.input_source_capacity = 4;
    config.component_mapping_capacity = 8;
    config.event_queue_capacity = 8;
    rc = h2_runtime_init(&config, &runtime);
  }
  if (rc == 0) {
    const h2_esp_h2loader_config_t loader = {
        .board = "ai_passport",
        .command_task_stack_size = 32768,
        .mount_file_point = h2_esp_board_fs_mount,
    };
    h2_esp_h2loader_run_with_config(runtime, &loader);
  } else {
    printf("H2_LOADER_INIT_FAIL board=ai_passport code=%d\n", rc);
  }
  if (runtime != NULL) h2_runtime_deinit(runtime);
  int cleanup = h2_esp_board_runtime_deinit();
  if (cleanup != 0) printf("H2_LOADER_CLEANUP_FAIL code=%d\n", cleanup);
}
