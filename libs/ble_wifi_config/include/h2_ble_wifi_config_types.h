#ifndef H2_BLE_WIFI_CONFIG_TYPES_H
#define H2_BLE_WIFI_CONFIG_TYPES_H

#include "h2_ble_wifi_config_protocol.h"

#include "h2/pal/hal/h2_pal_ble.h"
#include "h2/pal/hal/h2_pal_wifi.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/os/h2_pal_sync.h"
#include "h2/pal/os/h2_pal_system_event.h"
#include "h2/pal/os/h2_pal_task.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Default scan timeout passed to the Wi-Fi PAL, in milliseconds. */
#define H2_BLE_WIFI_CONFIG_DEFAULT_SCAN_TIMEOUT_MS 10000u
/** Default connect timeout passed to the Wi-Fi PAL, in milliseconds. */
#define H2_BLE_WIFI_CONFIG_DEFAULT_CONNECT_TIMEOUT_MS 15000u
/** Number of characteristics in the provisioning service. */
#define H2_BLE_WIFI_CONFIG_CHARACTERISTIC_COUNT 3u

/** Provisioning service instance. */
typedef struct h2_ble_wifi_config h2_ble_wifi_config_t;

/** Service lifecycle notifications. */
typedef enum h2_ble_wifi_config_event {
    /** A peripheral connection this service can serve was established. */
    H2_BLE_WIFI_CONFIG_EVENT_CONNECTED = 0,
    /** The served connection went away. */
    H2_BLE_WIFI_CONFIG_EVENT_DISCONNECTED,
    /**
     * The negotiated ATT MTU cannot carry a full credential frame. The
     * provisioning window stays open, but credentials cannot arrive.
     */
    H2_BLE_WIFI_CONFIG_EVENT_MTU_TOO_SMALL,
    /** A scan started on the worker task. */
    H2_BLE_WIFI_CONFIG_EVENT_SCAN_STARTED,
    /** A scan finished; status carries the Wi-Fi PAL result. */
    H2_BLE_WIFI_CONFIG_EVENT_SCAN_FINISHED,
    /** A well-formed credential frame was accepted for a connect attempt. */
    H2_BLE_WIFI_CONFIG_EVENT_CREDENTIALS_RECEIVED,
    /** The station reached an address with the received credentials. */
    H2_BLE_WIFI_CONFIG_EVENT_PROVISION_SUCCEEDED,
    /** The connect attempt failed; status carries the reported reason byte. */
    H2_BLE_WIFI_CONFIG_EVENT_PROVISION_FAILED,
    /**
     * A malformed or unexpected frame was rejected; status carries why. A
     * malformed credential frame also fails its ATT write and still produces
     * a failure result frame, so the application never waits for a reply that
     * will not come.
     */
    H2_BLE_WIFI_CONFIG_EVENT_PROTOCOL_ERROR,
} h2_ble_wifi_config_event_t;

/**
 * Service notification callback.
 *
 * Callbacks run on the service worker task, without any service lock held,
 * and must return promptly. The service instance is borrowed and must not be
 * closed from the callback.
 */
typedef void (*h2_ble_wifi_config_event_fn)(
    void *user,
    h2_ble_wifi_config_t *service,
    h2_ble_wifi_config_event_t event,
    uint16_t conn_handle,
    int status);

/**
 * Optional replacement for the built-in connect step.
 *
 * The callback runs on the service worker task, one call at a time, with no
 * service lock held. It is synchronous: the attempt is finished when it
 * returns, and the service encodes its result into the provisioning result
 * frame right after. There is no cancellation and no service-side timeout, so
 * the callback owns its own bound and must return within it;
 * h2_ble_wifi_config_close() waits for an attempt already in flight. The
 * callback owns the whole attempt, including the access-point check,
 * connecting, and persisting the credentials when it succeeds, and neither
 * h2_ble_wifi_config_config_t::connect_timeout_ms nor
 * h2_ble_wifi_config_config_t::skip_ap_verification_before_connect applies
 * to it.
 *
 * @param user h2_ble_wifi_config_config_t::user.
 * @param credentials Borrowed credentials, valid only for the call and never
 * retained by the service after the attempt.
 * @param out_reason Preset to H2_BLE_WIFI_CONFIG_REASON_NONE. On failure the
 * callback writes the reason byte to report; leaving it unchanged reports
 * H2_BLE_WIFI_CONFIG_REASON_UNKNOWN. It is ignored on success.
 * @return H2_PAL_OK when the station reached an address, or any other result
 * to report a failure. The returned value is not sent to the peer; only
 * @p out_reason is.
 */
typedef int (*h2_ble_wifi_config_connect_fn)(
    void *user,
    const h2_ble_wifi_config_credentials_t *credentials,
    h2_ble_wifi_config_reason_t *out_reason);

/**
 * Optional replacement for the built-in disconnect-reason mapping.
 *
 * @param user h2_ble_wifi_config_config_t::user.
 * @param connect_result Result returned by h2_pal_wifi_sta_connect().
 * @param status Borrowed station status, or NULL when it could not be read.
 * @return The reason byte to report to the application.
 */
typedef h2_ble_wifi_config_reason_t (*h2_ble_wifi_config_reason_fn)(
    void *user,
    int connect_result,
    const h2_pal_wifi_sta_status_t *status);

/** Platform capabilities borrowed by the service for its whole lifetime. */
typedef struct h2_ble_wifi_config_api {
    const h2_pal_ble_host_api_t *ble;
    const h2_pal_wifi_sta_api_t *wifi_sta;
    const h2_pal_task_api_t *task;
    const h2_pal_sync_api_t *sync;
    const h2_pal_system_event_api_t *system_event;
    const h2_pal_mem_api_t *allocator;
} h2_ble_wifi_config_api_t;

/** Service configuration. A NULL config selects every default. */
typedef struct h2_ble_wifi_config_config {
    /** Zero-length UUIDs select the documented provisioning UUIDs. */
    h2_pal_ble_uuid_t service_uuid;
    h2_pal_ble_uuid_t command_char_uuid;
    h2_pal_ble_uuid_t scan_char_uuid;
    h2_pal_ble_uuid_t provision_char_uuid;
    /**
     * Smallest ATT MTU that may carry credentials. Zero selects
     * H2_BLE_WIFI_CONFIG_MIN_ATT_MTU and smaller values are rejected.
     */
    uint16_t min_att_mtu;
    /** Zero selects H2_BLE_WIFI_CONFIG_DEFAULT_SCAN_TIMEOUT_MS. */
    uint32_t scan_timeout_ms;
    /** Zero selects H2_BLE_WIFI_CONFIG_DEFAULT_CONNECT_TIMEOUT_MS. */
    uint32_t connect_timeout_ms;
    /**
     * Connect without first scanning for the requested SSID. The default
     * scans, which costs one extra scan per attempt but reports a missing
     * access point as such instead of guessing it from a platform-specific
     * disconnect reason.
     */
    bool skip_ap_verification_before_connect;
    /**
     * Keep this service's advertising running across a scan or connect
     * attempt. The default stops and restarts it, because Wi-Fi and BLE
     * share one radio and advertising during Wi-Fi work slows both down.
     */
    bool keep_advertising_during_wifi;
    /**
     * The caller registers the GATT schema itself, for example to publish
     * the provisioning service next to other services on the same Host.
     * h2_ble_wifi_config_gatt_service() then returns the borrowed
     * declaration to include in that registration.
     */
    bool gatt_service_registered_by_caller;
    h2_pal_task_options_t worker_task_options;
    /** NULL selects the built-in Wi-Fi PAL connect step. */
    h2_ble_wifi_config_connect_fn connect;
    /** NULL selects h2_ble_wifi_config_default_reason(). */
    h2_ble_wifi_config_reason_fn map_reason;
    h2_ble_wifi_config_event_fn on_event;
    void *user;
} h2_ble_wifi_config_config_t;

/** Counters for diagnostics and tests. */
typedef struct h2_ble_wifi_config_stats {
    uint16_t att_mtu;
    uint32_t scans_started;
    uint32_t aps_reported;
    uint32_t aps_dropped;
    uint32_t notify_failures;
    /**
     * Sends whose peer was replaced while the BLE Host call was running.
     * The Wi-Fi provisioning frames are addressed by connection handle only,
     * so such a frame may have reached the replacement peer; the service
     * cannot prevent it and stops the operation instead.
     */
    uint32_t sends_during_peer_change;
    uint32_t provision_attempts;
    uint32_t provision_failures;
    uint32_t protocol_errors;
} h2_ble_wifi_config_stats_t;

#ifdef __cplusplus
}
#endif

#endif
