#include "h2_esp_board.h"
#include "h2_device_service.h"
#include "h2_esp_target_task_policy.h"

#include <stdio.h>

static h2_device_loader_t device_loader;

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
    const h2_device_loader_config_t loader = {
        .board = "ai_passport",
        .loader_partition_id = 1u,
        .app_partition_id = 2u,
        .coredump_partition_id = 3u,
        .hardware_capabilities = H2_LOADER_CAPABILITIES_ALL,
        .identity_policy = H2_DEVICE_IDENTITY_NATIVE,
        .digest_yield_ms = 1u,
        .command_task_stack_size = 32768,
    };
    rc = h2_device_loader_run(&device_loader, runtime, &loader, NULL, 0);
    if (rc != 0) printf("H2_LOADER_SERVICE_FAIL code=%d\n", rc);
  } else {
    printf("H2_LOADER_INIT_FAIL board=ai_passport code=%d\n", rc);
  }
  if (runtime != NULL) h2_runtime_deinit(runtime);
  int cleanup = h2_esp_board_runtime_deinit();
  if (cleanup != 0) printf("H2_LOADER_CLEANUP_FAIL code=%d\n", cleanup);
}
