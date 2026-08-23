#ifndef H2_BLEIKCP_SPEED_H
#define H2_BLEIKCP_SPEED_H

#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_bleikcp_speed_role {
    H2_BLEIKCP_SPEED_ROLE_SERVER = 0,
    H2_BLEIKCP_SPEED_ROLE_CLIENT = 1,
} h2_bleikcp_speed_role_t;

/** Control the launcher-owned H2Loader advertising set. */
typedef int (*h2_bleikcp_speed_advertising_control_fn)(void *user);

/** Add the borrowed Baseline service UUID to launcher-owned advertising. */
typedef int (*h2_bleikcp_speed_advertising_service_fn)(
    void *user,
    const h2_pal_ble_uuid_t *service_uuid);

/** Confirm the image after its display and BLE role are operational. */
typedef int (*h2_bleikcp_speed_ready_fn)(void *user);

/** Return true when a host application requests cooperative shutdown. */
typedef bool (*h2_bleikcp_speed_should_stop_fn)(void *user);

typedef struct h2_bleikcp_speed_config {
    h2_bleikcp_speed_role_t role;
    /** BLE advertising and scan modes selected by the target launcher. */
    h2_pal_ble_adv_type_t advertising_type;
    h2_pal_ble_scan_type_t scan_type;
    h2_bleikcp_speed_advertising_control_fn pause_management_advertising;
    h2_bleikcp_speed_advertising_control_fn resume_management_advertising;
    h2_bleikcp_speed_advertising_service_fn advertise_server_service;
    void *management_advertising_user;
    h2_bleikcp_speed_ready_fn ready;
    void *ready_user;
    h2_bleikcp_speed_should_stop_fn should_stop;
    void *stop_user;
} h2_bleikcp_speed_config_t;

/**
 * Run the persistent device-to-device BLE iKCP baseline application.
 *
 * The Runtime and callback user remain borrowed for the complete blocking
 * call. Client launchers must provide both advertising control callbacks;
 * Server launchers may provide advertise_server_service to share the existing
 * H2Loader advertising set. All launchers provide ready so image confirmation
 * happens only after the display and selected BLE role are operational. A null
 * should_stop callback keeps the persistent firmware behavior; Desktop apps
 * provide it so closing the window follows the normal cleanup path.
 */
int h2_bleikcp_speed_run(
    h2_runtime_t *runtime,
    const h2_bleikcp_speed_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
