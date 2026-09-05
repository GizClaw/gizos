#ifndef H2_SMOKE_BLE_WIFI_CONFIG_H
#define H2_SMOKE_BLE_WIFI_CONFIG_H

#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Longest the app keeps the provisioning window open, in milliseconds. */
#define H2_SMOKE_BLE_WIFI_CONFIG_WINDOW_MS 600000u

/**
 * @brief Open a BLE provisioning window and wait for a phone to use it.
 *
 * Publishes the `libs/ble_wifi_config` GATT profile, advertises it as a
 * connectable peripheral, and logs every stage as `H2_SMOKE_BLE_WIFI_CONFIG`
 * lines on the console. The call returns once the station reaches an address,
 * once the window expires, or on a setup failure; the window is closed before
 * returning either way.
 *
 * @param runtime Borrowed Runtime supplying the BLE Host, Wi-Fi station,
 * task, sync, system-event and memory capabilities.
 * @return H2_PAL_OK when a phone provisioned the device, H2_PAL_ERR_TIMEOUT
 * when the window expired unused, H2_PAL_ERR_UNSUPPORTED when the Runtime
 * lacks a required capability, or the underlying failure.
 */
int h2_smoke_ble_wifi_config_run(h2_runtime_t *runtime);

#ifdef __cplusplus
}
#endif

#endif
