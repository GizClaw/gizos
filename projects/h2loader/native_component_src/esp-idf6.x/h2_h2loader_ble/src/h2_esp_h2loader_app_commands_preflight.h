#ifndef H2_ESP_H2LOADER_APP_COMMANDS_PREFLIGHT_H
#define H2_ESP_H2LOADER_APP_COMMANDS_PREFLIGHT_H

#include "h2_runtime.h"

typedef struct h2_esp_h2loader_app_commands_preflight {
    h2_pal_mutex_t *operation_mutex;
    h2_pal_firmware_info_t firmware_info;
    uint32_t app_partition_id;
} h2_esp_h2loader_app_commands_preflight_t;

int h2_esp_h2loader_app_commands_preflight(
    const h2_runtime_config_t *runtime_config,
    h2_esp_h2loader_app_commands_preflight_t *out_preflight);

#endif
