#pragma once
#include "h2_bk_board_application_host.h"
#include "h2_esp_board_application_host.h"
#include "h2_runtime.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct h2_esp_h2loader_app_commands_config {
  const char *active_name;
  const char *active_version;
  uint32_t hardware_capabilities;
  uint32_t h2loader_partition_id;
  uint32_t coredump_partition_id;
  const char *ble_local_name;
} h2_esp_h2loader_app_commands_config_t;
const h2_pal_mem_api_t *h2_esp_platform_default_allocator(void);
const h2_pal_mem_api_t *h2_bk_platform_default_allocator(void);
const h2_pal_time_api_t *h2_esp_platform_time_api(void);
const h2_pal_time_api_t *h2_bk_platform_time_api(void);
int h2_esp_h2loader_app_commands_prepare_serial_with_config(
    const h2_runtime_config_t *config,
    const h2_esp_h2loader_app_commands_config_t *commands);
int h2_bk_h2loader_start_app_iostreamikcp_with_capabilities(
    h2_runtime_t *runtime, const char *name, uint32_t capabilities);
int h2_esp_h2loader_app_commands_stop_serial(void);
int h2_bk_h2loader_stop_app_commands(void);
int h2_esp_h2loader_app_serial_is_started(void);
int h2_bk_h2loader_app_serial_is_started(void);
#ifdef __cplusplus
}
#endif
