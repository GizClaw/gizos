#include "h2_esp_h2loader_app_commands_preflight.h"

#include <string.h>

int h2_esp_h2loader_app_commands_preflight(
    const h2_runtime_config_t *runtime_config,
    h2_esp_h2loader_app_commands_preflight_t *out_preflight) {
    if (runtime_config == NULL || runtime_config->sync == NULL ||
        runtime_config->mem == NULL || runtime_config->firmware_info == NULL ||
        runtime_config->power == NULL || out_preflight == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_preflight, 0, sizeof(*out_preflight));
    const h2_pal_mutex_config_t mutex_config = {
        .name = "h2loader-app-operation",
        .allocator = runtime_config->mem,
        .flags = H2_PAL_MUTEX_FLAG_RECURSIVE,
    };
    int rc = h2_pal_mutex_create(
        runtime_config->sync, &mutex_config, &out_preflight->operation_mutex);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_pal_firmware_info_get_current(
        runtime_config->firmware_info, &out_preflight->firmware_info);
    if (rc == H2_PAL_OK) {
        h2_pal_power_boot_partition_t running_partition;
        rc = h2_pal_power_get_running_boot_partition(
            runtime_config->power, &running_partition);
        if (rc == H2_PAL_OK &&
            (running_partition.id == 0u ||
             (running_partition.flags &
              H2_PAL_POWER_BOOT_PARTITION_FLAG_APP) == 0u)) {
            rc = H2_PAL_ERR_INVALID_STATE;
        }
        if (rc == H2_PAL_OK) {
            out_preflight->app_partition_id = running_partition.id;
            return H2_PAL_OK;
        }
    }
    const int cleanup_rc = h2_pal_mutex_destroy(
        runtime_config->sync, out_preflight->operation_mutex);
    memset(out_preflight, 0, sizeof(*out_preflight));
    return cleanup_rc == H2_PAL_OK ? rc : cleanup_rc;
}
