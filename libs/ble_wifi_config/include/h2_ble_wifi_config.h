#ifndef H2_BLE_WIFI_CONFIG_H
#define H2_BLE_WIFI_CONFIG_H

#include "h2_ble_wifi_config_protocol.h"
#include "h2_ble_wifi_config_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file
 * @brief BLE Wi-Fi provisioning service.
 *
 * The service publishes the GATT profile a phone application uses to scan for
 * access points and hand over credentials while the device is still offline.
 * The provisioning window itself is the authorization: the service neither
 * authenticates the peer nor encrypts the credential frame, so the caller
 * opens it only while the device is unprovisioned or a local gesture asked
 * for it, and closes it afterwards.
 *
 * Default GATT profile:
 *
 * | Characteristic | UUID | Properties |
 * | --- | --- | --- |
 * | Service | 0000a100-0000-1000-8000-00805f9b34fb | - |
 * | Command | 0000a101-0000-1000-8000-00805f9b34fb | Write |
 * | Scan | 0000a102-0000-1000-8000-00805f9b34fb | Notify |
 * | Provisioning | 0000a103-0000-1000-8000-00805f9b34fb | Write, Notify |
 *
 * The wire format of every frame is documented in
 * h2_ble_wifi_config_protocol.h.
 *
 * Scans and connect attempts run on one worker task, so they are serialized
 * against each other. GATT write callbacks only validate a frame and hand the
 * work to that task, so they never block the Host.
 *
 * The service follows one peripheral connection at a time: the first
 * peripheral connection is adopted, and writes carrying any other connection
 * handle are refused with H2_PAL_ERR_INVALID_STATE until that connection ends.
 * Wi-Fi work outlives the ATT write that started it, so every notification is
 * bound to the connection that requested it and is dropped once that
 * connection ends, even when a later peer reuses its connection handle. No
 * service lock is held across a BLE Host call, so a provider that dispatches
 * a connection event from inside a notification cannot deadlock the service;
 * such an event is applied once the notification returns.
 */

/** Borrowed default service UUID, 128-bit, little-endian ATT byte order. */
extern const uint8_t h2_ble_wifi_config_default_service_uuid[16];
/** Borrowed default command characteristic UUID. */
extern const uint8_t h2_ble_wifi_config_default_command_uuid[16];
/** Borrowed default scan characteristic UUID. */
extern const uint8_t h2_ble_wifi_config_default_scan_uuid[16];
/** Borrowed default provisioning characteristic UUID. */
extern const uint8_t h2_ble_wifi_config_default_provision_uuid[16];

/**
 * @brief Open the provisioning service and start its worker task.
 *
 * Unless h2_ble_wifi_config_config_t::gatt_service_registered_by_caller is
 * set, the service registers its GATT schema on the Host, which replaces any
 * schema registered earlier on that Host. The BLE Host must already be
 * started. Every API object in @p api is borrowed and must outlive the
 * service.
 *
 * @param api Required BLE Host, Wi-Fi station, task, sync, system event and
 * memory capabilities.
 * @param config Borrowed configuration, copied before returning, or NULL for
 * the defaults.
 * @param out_service Set to NULL first, then to the new service on success.
 * @return H2_PAL_OK, H2_PAL_ERR_INVALID_ARG, H2_PAL_ERR_NO_MEMORY,
 * H2_PAL_ERR_UNSUPPORTED when a required capability is missing, or the
 * underlying PAL failure.
 */
int h2_ble_wifi_config_open(
    const h2_ble_wifi_config_api_t *api,
    const h2_ble_wifi_config_config_t *config,
    h2_ble_wifi_config_t **out_service);

/**
 * @brief Stop the worker task, release the GATT schema and free the service.
 *
 * A scan or connect attempt already running on the worker task is awaited;
 * the service does not cancel a PAL operation in flight. Advertising started
 * through h2_ble_wifi_config_start_advertising() is stopped. Passing NULL is
 * an error, and calling this twice on the same instance is undefined.
 *
 * @param service Service to close.
 * @return H2_PAL_OK, H2_PAL_ERR_INVALID_ARG, or the first PAL failure. The
 * service is freed unless joining the worker task or releasing the GATT
 * schema failed; the Host borrows that schema until it is released, so the
 * instance stays alive and the caller may retry.
 */
int h2_ble_wifi_config_close(h2_ble_wifi_config_t *service);

/**
 * @brief Publish the advertising data and start the provisioning window.
 *
 * The service remembers @p params so it can restart advertising after a scan
 * or connect attempt when
 * h2_ble_wifi_config_config_t::pause_advertising_during_wifi is set.
 *
 * @param service Service that owns the advertising window.
 * @param data Borrowed advertising data applied before advertising starts, or
 * NULL to keep whatever the Host already holds.
 * @param params Borrowed advertising parameters, copied before returning.
 * @return H2_PAL_OK, H2_PAL_ERR_INVALID_ARG, or the underlying PAL failure.
 */
int h2_ble_wifi_config_start_advertising(
    h2_ble_wifi_config_t *service,
    const h2_pal_ble_adv_data_t *data,
    const h2_pal_ble_adv_params_t *params);

/**
 * @brief Close the provisioning window opened by this service.
 *
 * Stopping advertising does not drop an established connection.
 *
 * @param service Service that owns the advertising window.
 * @return H2_PAL_OK, H2_PAL_ERR_INVALID_ARG, or the underlying PAL failure.
 * Stopping advertising that was never started returns H2_PAL_OK.
 */
int h2_ble_wifi_config_stop_advertising(h2_ble_wifi_config_t *service);

/**
 * @brief Return the borrowed GATT service declaration.
 *
 * The declaration, its characteristics and its handle storage live in the
 * service and stay valid until h2_ble_wifi_config_close(). Callers that set
 * h2_ble_wifi_config_config_t::gatt_service_registered_by_caller pass it to
 * h2_pal_ble_register_gatt_services() together with their own services, and
 * must keep that registration alive for the life of the service.
 *
 * @param service Service that owns the declaration.
 * @return The borrowed declaration, or NULL when @p service is NULL.
 */
const h2_pal_ble_gatt_service_t *h2_ble_wifi_config_gatt_service(
    const h2_ble_wifi_config_t *service);

/**
 * @brief Copy the current counters.
 *
 * @param service Service to inspect.
 * @param out_stats Cleared first, then filled on success.
 * @return H2_PAL_OK or H2_PAL_ERR_INVALID_ARG.
 */
int h2_ble_wifi_config_get_stats(
    h2_ble_wifi_config_t *service,
    h2_ble_wifi_config_stats_t *out_stats);

#ifdef __cplusplus
}
#endif

#endif
