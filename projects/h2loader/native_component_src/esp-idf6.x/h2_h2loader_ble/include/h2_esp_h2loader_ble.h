#ifndef H2_ESP_H2LOADER_BLE_H
#define H2_ESP_H2LOADER_BLE_H

#include "h2_esp_h2loader_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Runs the blocking ESP H2Loader with the BLE command service attached. */
void h2_esp_h2loader_run_with_ble_config(
    h2_runtime_t *runtime,
    const h2_esp_h2loader_config_t *config);

/** App command identity and partition config; strings remain borrowed while services run. */
typedef struct h2_esp_h2loader_app_commands_config {
    const char *active_name;
    const char *active_version;
    uint32_t hardware_capabilities;
    uint32_t h2loader_partition_id;
    uint32_t app_partition_id;
    uint32_t coredump_partition_id;
} h2_esp_h2loader_app_commands_config_t;

/**
 * Starts the singleton serial recovery command service before Runtime init.
 * The runtime config and command strings remain borrowed while the service runs.
 */
int h2_esp_h2loader_app_commands_prepare_serial_with_config(
    const h2_runtime_config_t *runtime_config,
    const h2_esp_h2loader_app_commands_config_t *config);

/** Starts the serial recovery command service before Runtime init. */
int h2_esp_h2loader_app_commands_prepare_serial(
    const h2_runtime_config_t *runtime_config,
    const char *active_name,
    uint32_t h2loader_partition_id,
    uint32_t coredump_partition_id);

/**
 * Attaches the BLE command service after Runtime init.
 * The serial service must already have been prepared. BLE-only startup
 * failures retry up to six times with bounded backoff while serial recovery
 * remains available.
 */
int h2_esp_h2loader_app_commands_start_with_config(
    h2_runtime_t *runtime,
    const h2_esp_h2loader_app_commands_config_t *config);

/** Attaches the BLE H2Loader command service after Runtime init. */
int h2_esp_h2loader_app_commands_start(
    h2_runtime_t *runtime,
    const char *active_name,
    uint32_t h2loader_partition_id,
    uint32_t coredump_partition_id);

/**
 * Persist Loader boot intent through the prepared App command client and
 * reboot into the H2Loader partition. The serial service must be prepared.
 */
int h2_esp_h2loader_app_commands_reboot_loader(void);

/** Pauses the App command service advertisement while a Central scan runs. */
int h2_esp_h2loader_app_commands_pause_ble_advertising(void);

/** Resumes the App command service advertisement after the Central scan. */
int h2_esp_h2loader_app_commands_resume_ble_advertising(void);

/** Add one App-owned GATT service UUID to the shared command advertisement. */
int h2_esp_h2loader_app_commands_advertise_ble_service(
    const h2_pal_ble_uuid_t *service_uuid);

#ifdef __cplusplus
}
#endif

#endif
