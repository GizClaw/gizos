#ifndef H2_LIBCO_SMOKE_TEST_SDK_H
#define H2_LIBCO_SMOKE_TEST_SDK_H

#include "h2_libco_smoke.h"
#include "h2_loader_app_client.h"
#include "result_console.h"

#include <stdbool.h>
#include <stdint.h>

typedef uint8_t StackType_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
#define tskNO_AFFINITY (-1)
#define pdMS_TO_TICKS(ms) (ms)

typedef struct h2_esp_h2loader_app_commands_config {
  const char *active_name;
  const char *active_version;
  uint32_t hardware_capabilities;
  uint32_t h2loader_partition_id;
  uint32_t coredump_partition_id;
  const char *ble_local_name;
} h2_esp_h2loader_app_commands_config_t;

BaseType_t xTaskGetCoreID(void *task);
BaseType_t xPortGetCoreID(void);
StackType_t *xTaskGetStackStart(void *task);
uintptr_t esp_cpu_get_sp(void);
bool heap_caps_check_integrity_all(bool print_errors);
UBaseType_t uxTaskGetStackHighWaterMark(void *task);
void vTaskDelay(uint32_t ms);
void esp_restart(void);
int h2_esp_target_task_policy_install(void);
int h2_esp_board_runtime_config(h2_runtime_config_t *config);
int h2_esp_h2loader_app_commands_prepare_serial_with_config(
    const h2_runtime_config_t *runtime_config,
    const h2_esp_h2loader_app_commands_config_t *config);
int h2_esp_e2e_result_log_prepare(h2_esp_e2e_result_log_t *state,
    h2_runtime_t *runtime, const char *scope, const char *second_scope);
int h2_esp_h2loader_app_commands_get_config(
    h2_loader_app_client_config_t *config);
int h2_esp_h2loader_app_confirm(h2_runtime_t *runtime);

#endif
