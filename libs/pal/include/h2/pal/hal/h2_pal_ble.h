#ifndef H2_PAL_BLE_H
#define H2_PAL_BLE_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/os/h2_pal_mem.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_PAL_BLE_ADDR_LEN 6u
#define H2_PAL_BLE_INVALID_CONN_HANDLE ((uint16_t)0xffffu)
#define H2_PAL_BLE_INVALID_ATTR_HANDLE ((uint16_t)0x0000u)
#define H2_PAL_BLE_ATT_HEADER_LEN 3u
/* ATT protocol maximum. Platform implementations should request this as their
 * local preferred MTU, then return the negotiated MTU from exchange_mtu(). */
#define H2_PAL_BLE_ATT_MAX_MTU 517u
#define H2_PAL_BLE_ATT_MAX_VALUE_LEN (H2_PAL_BLE_ATT_MAX_MTU - H2_PAL_BLE_ATT_HEADER_LEN)
/** Maximum encoded legacy advertising-data length in bytes. */
#define H2_PAL_BLE_LEGACY_ADV_DATA_MAX_LEN 31u
/** Maximum encoded Extended Advertising data length in bytes. */
#define H2_PAL_BLE_EXT_ADV_DATA_MAX_LEN 1650u
/** Maximum Extended Advertising SID. */
#define H2_PAL_BLE_EXT_ADV_SID_MAX 15u
/** Maximum Extended Advertising duration representable in 10 ms units. */
#define H2_PAL_BLE_ADV_DURATION_MAX_MS 655350u
/** Minimum primary advertising interval in milliseconds. */
#define H2_PAL_BLE_ADV_INTERVAL_MIN_MS 20u
/** Maximum legacy advertising interval in milliseconds. */
#define H2_PAL_BLE_LEGACY_ADV_INTERVAL_MAX_MS 10240u
/** Maximum Extended Advertising interval in whole milliseconds. */
#define H2_PAL_BLE_EXT_ADV_INTERVAL_MAX_MS 10485759u
/** Minimum scan interval and window expressible in whole milliseconds. */
#define H2_PAL_BLE_SCAN_INTERVAL_MIN_MS 3u
/** Maximum legacy scan interval in milliseconds. */
#define H2_PAL_BLE_LEGACY_SCAN_INTERVAL_MAX_MS 10240u
/** Maximum Extended Scan interval in whole milliseconds. */
#define H2_PAL_BLE_EXT_SCAN_INTERVAL_MAX_MS 40959u
/** Maximum scan duration representable in 10 ms units. */
#define H2_PAL_BLE_SCAN_DURATION_MAX_MS 655350u

typedef struct h2_pal_ble_api h2_pal_ble_api_t;
typedef struct h2_pal_ble_adv_set h2_pal_ble_adv_set_t;
typedef h2_pal_ble_api_t h2_pal_ble_t;
typedef h2_pal_ble_api_t h2_pal_ble_host_api_t;

typedef enum h2_pal_ble_addr_type {
    H2_PAL_BLE_ADDR_TYPE_UNKNOWN = 0,
    H2_PAL_BLE_ADDR_TYPE_PUBLIC = 1,
    H2_PAL_BLE_ADDR_TYPE_RANDOM = 2,
    H2_PAL_BLE_ADDR_TYPE_PUBLIC_IDENTITY = 3,
    H2_PAL_BLE_ADDR_TYPE_RANDOM_IDENTITY = 4,
} h2_pal_ble_addr_type_t;

typedef struct h2_pal_ble_addr {
    uint8_t value[H2_PAL_BLE_ADDR_LEN];
    h2_pal_ble_addr_type_t type;
} h2_pal_ble_addr_t;

typedef enum h2_pal_ble_role {
    H2_PAL_BLE_ROLE_UNKNOWN = 0,
    H2_PAL_BLE_ROLE_PERIPHERAL = 1,
    H2_PAL_BLE_ROLE_CENTRAL = 2,
} h2_pal_ble_role_t;

typedef enum h2_pal_ble_adv_mode {
    H2_PAL_BLE_ADV_MODE_CONNECTABLE = 0,
    H2_PAL_BLE_ADV_MODE_NON_CONNECTABLE = 1,
} h2_pal_ble_adv_mode_t;

/** Advertising PDU family selected for a PAL advertising set. */
typedef enum h2_pal_ble_adv_type {
    /** Bluetooth LE legacy advertising. This zero value preserves old initializers. */
    H2_PAL_BLE_ADV_TYPE_LEGACY = 0,
    /** Bluetooth LE Extended Advertising. */
    H2_PAL_BLE_ADV_TYPE_EXTENDED = 1,
} h2_pal_ble_adv_type_t;

typedef enum h2_pal_ble_scan_mode {
    H2_PAL_BLE_SCAN_MODE_PASSIVE = 0,
    H2_PAL_BLE_SCAN_MODE_ACTIVE = 1,
} h2_pal_ble_scan_mode_t;

/** Controller scan procedure selected by start_scan(). */
typedef enum h2_pal_ble_scan_type {
    /** Legacy scanning. This zero value preserves old initializers. */
    H2_PAL_BLE_SCAN_TYPE_LEGACY = 0,
    /** Bluetooth LE Extended Scanning. */
    H2_PAL_BLE_SCAN_TYPE_EXTENDED = 1,
} h2_pal_ble_scan_type_t;

typedef uint8_t h2_pal_ble_scan_phy_mask_t;

/** Scan advertisements whose primary advertising PHY is LE 1M. */
#define H2_PAL_BLE_SCAN_PHY_1M ((h2_pal_ble_scan_phy_mask_t)(1u << 0))
/** Scan advertisements whose primary advertising PHY is LE Coded. */
#define H2_PAL_BLE_SCAN_PHY_CODED ((h2_pal_ble_scan_phy_mask_t)(1u << 1))
#define H2_PAL_BLE_SCAN_PHY_ALL \
    ((h2_pal_ble_scan_phy_mask_t)(H2_PAL_BLE_SCAN_PHY_1M | H2_PAL_BLE_SCAN_PHY_CODED))

typedef enum h2_pal_ble_gatt_property {
    H2_PAL_BLE_GATT_PROPERTY_READ = 1u << 0,
    H2_PAL_BLE_GATT_PROPERTY_WRITE = 1u << 1,
    H2_PAL_BLE_GATT_PROPERTY_WRITE_NO_RSP = 1u << 2,
    H2_PAL_BLE_GATT_PROPERTY_NOTIFY = 1u << 3,
    H2_PAL_BLE_GATT_PROPERTY_INDICATE = 1u << 4,
} h2_pal_ble_gatt_property_t;

typedef enum h2_pal_ble_gatt_permission {
    H2_PAL_BLE_GATT_PERMISSION_READ = 1u << 0,
    H2_PAL_BLE_GATT_PERMISSION_WRITE = 1u << 1,
    H2_PAL_BLE_GATT_PERMISSION_READ_ENCRYPTED = 1u << 2,
    H2_PAL_BLE_GATT_PERMISSION_WRITE_ENCRYPTED = 1u << 3,
    H2_PAL_BLE_GATT_PERMISSION_READ_AUTHENTICATED = 1u << 4,
    H2_PAL_BLE_GATT_PERMISSION_WRITE_AUTHENTICATED = 1u << 5,
} h2_pal_ble_gatt_permission_t;

typedef enum h2_pal_ble_pairing_io {
    H2_PAL_BLE_PAIRING_IO_DISPLAY_ONLY = 0,
    H2_PAL_BLE_PAIRING_IO_KEYBOARD_ONLY = 1,
} h2_pal_ble_pairing_io_t;

/**
 * Ephemeral LE Secure Connections configuration.
 *
 * A disabled configuration removes the previously installed passkey. Enabling
 * pairing requires a six-digit passkey and MITM-protected Secure Connections;
 * PAL implementations must not silently downgrade to Just Works or legacy
 * pairing.
 */
typedef struct h2_pal_ble_pairing_config {
    bool enabled;
    uint32_t passkey;
    h2_pal_ble_pairing_io_t io;
} h2_pal_ble_pairing_config_t;

typedef enum h2_pal_ble_gatt_discovery_kind {
    H2_PAL_BLE_GATT_DISCOVERY_SERVICE = 0,
    H2_PAL_BLE_GATT_DISCOVERY_CHARACTERISTIC = 1,
    H2_PAL_BLE_GATT_DISCOVERY_DESCRIPTOR = 2,
} h2_pal_ble_gatt_discovery_kind_t;

typedef enum h2_pal_ble_subscribe_mode {
    H2_PAL_BLE_SUBSCRIBE_MODE_NOTIFY = 0,
    H2_PAL_BLE_SUBSCRIBE_MODE_INDICATE = 1,
} h2_pal_ble_subscribe_mode_t;

typedef enum h2_pal_ble_phy {
    H2_PAL_BLE_PHY_UNKNOWN = 0,
    H2_PAL_BLE_PHY_1M = 1,
    H2_PAL_BLE_PHY_2M = 2,
    H2_PAL_BLE_PHY_CODED = 3,
} h2_pal_ble_phy_t;

/** Completeness of the raw advertising data in one scan report. */
typedef enum h2_pal_ble_adv_data_status {
    H2_PAL_BLE_ADV_DATA_COMPLETE = 0,
    H2_PAL_BLE_ADV_DATA_INCOMPLETE = 1,
    H2_PAL_BLE_ADV_DATA_TRUNCATED = 2,
} h2_pal_ble_adv_data_status_t;

typedef struct h2_pal_ble_bytes {
    const uint8_t *data;
    size_t len;
} h2_pal_ble_bytes_t;

typedef struct h2_pal_ble_uuid {
    const uint8_t *data;
    size_t len;
} h2_pal_ble_uuid_t;

typedef struct h2_pal_ble_adv_data {
    const char *local_name;
    const h2_pal_ble_uuid_t *service_uuids;
    size_t service_uuid_count;
    h2_pal_ble_bytes_t manufacturer_data;
    /**
     * Optional UUID selecting the Service Data AD type. When omitted,
     * service_data retains the legacy raw 16-bit Service Data encoding and
     * must include its own 16-bit UUID prefix.
     */
    h2_pal_ble_uuid_t service_data_uuid;
    /** Service Data payload, excluding service_data_uuid when it is present. */
    h2_pal_ble_bytes_t service_data;
} h2_pal_ble_adv_data_t;

typedef struct h2_pal_ble_adv_params {
    h2_pal_ble_adv_mode_t mode;
    /** Minimum primary advertising interval in milliseconds. */
    uint32_t interval_min_ms;
    /** Maximum primary advertising interval in milliseconds. */
    uint32_t interval_max_ms;
    /** Legacy or Extended Advertising. The zero value selects legacy. */
    h2_pal_ble_adv_type_t type;
    /** Primary PHY. UNKNOWN selects LE 1M. */
    h2_pal_ble_phy_t primary_phy;
    /** Secondary PHY. UNKNOWN selects LE 1M. */
    h2_pal_ble_phy_t secondary_phy;
    /** Extended Advertising SID in the inclusive range 0 through 15. */
    uint8_t sid;
    /** Stop after this many Extended Advertising events; zero means unlimited. */
    uint8_t max_adv_events;
    /** Stop Extended Advertising after this many milliseconds; zero means unlimited. */
    uint32_t duration_ms;
} h2_pal_ble_adv_params_t;

/** Originating handle-scoped advertising set for lifecycle system events. */
typedef struct h2_pal_ble_adv_set_event {
    /** Borrowed set identity. Handle-scoped events always provide a set. */
    h2_pal_ble_adv_set_t *set;
    /** Completion result reported by the backend. */
    h2_pal_result_t status;
} h2_pal_ble_adv_set_event_t;

typedef struct h2_pal_ble_scan_params {
    h2_pal_ble_scan_mode_t mode;
    uint32_t interval_ms;
    uint32_t window_ms;
    uint32_t timeout_ms;
    /** Legacy or Extended Scanning. The zero value selects legacy. */
    h2_pal_ble_scan_type_t type;
    /** Primary PHY mask. Zero selects LE 1M. */
    h2_pal_ble_scan_phy_mask_t phy_mask;
} h2_pal_ble_scan_params_t;

typedef struct h2_pal_ble_scan_result {
    h2_pal_ble_addr_t addr;
    int rssi;
    bool connectable;
    bool scan_response;
    const char *local_name;
    size_t local_name_len;
    h2_pal_ble_bytes_t manufacturer_data;
    h2_pal_ble_bytes_t service_data;
    const h2_pal_ble_uuid_t *service_uuids;
    size_t service_uuid_count;
    /** Legacy or Extended Advertising report. */
    h2_pal_ble_adv_type_t adv_type;
    h2_pal_ble_phy_t primary_phy;
    h2_pal_ble_phy_t secondary_phy;
    uint8_t sid;
    h2_pal_ble_adv_data_status_t data_status;
    /** Advertiser transmit power in dBm, or 127 when unavailable. */
    int8_t tx_power;
    /** Borrowed raw advertising data valid only during the callback. */
    h2_pal_ble_bytes_t raw_data;
} h2_pal_ble_scan_result_t;

/* Return true to request scan stop; return false to keep scanning. */
typedef bool (*h2_pal_ble_scan_result_fn)(
    void *user,
    const h2_pal_ble_scan_result_t *result);

typedef struct h2_pal_ble_connection {
    uint16_t conn_handle;
    h2_pal_ble_role_t role;
    h2_pal_ble_addr_t peer_addr;
    uint16_t mtu;
} h2_pal_ble_connection_t;

typedef struct h2_pal_ble_connect_params {
    uint32_t timeout_ms;
    uint16_t interval_min_ms;
    uint16_t interval_max_ms;
    uint16_t latency;
    uint16_t supervision_timeout_ms;
} h2_pal_ble_connect_params_t;

typedef struct h2_pal_ble_connection_params {
    uint16_t interval_min_ms;
    uint16_t interval_max_ms;
    uint16_t latency;
    uint16_t supervision_timeout_ms;
} h2_pal_ble_connection_params_t;

typedef struct h2_pal_ble_gatt_access {
    uint16_t conn_handle;
    uint16_t attr_handle;
    uint16_t offset;
} h2_pal_ble_gatt_access_t;

/**
 * Serve a GATT read request and return its ATT result synchronously.
 *
 * The access descriptor and output buffer are borrowed only for the callback.
 * They must not be retained. A successful return publishes exactly
 * @p out_len bytes from @p out, where @p out_len must not exceed
 * @p out_size. Execution context and request buffering are provider-specific.
 */
typedef h2_pal_result_t (*h2_pal_ble_gatt_read_fn)(
    void *user,
    const h2_pal_ble_gatt_access_t *access,
    uint8_t *out,
    size_t out_size,
    size_t *out_len);

/**
 * Serve a GATT write request and return its ATT result synchronously.
 *
 * The access descriptor and data are borrowed only for the callback and must
 * not be retained. Execution context and request buffering are
 * provider-specific.
 */
typedef h2_pal_result_t (*h2_pal_ble_gatt_write_fn)(
    void *user,
    const h2_pal_ble_gatt_access_t *access,
    const uint8_t *data,
    size_t len);

typedef struct h2_pal_ble_gatt_characteristic {
    h2_pal_ble_uuid_t uuid;
    uint32_t properties;
    uint32_t permissions;
    const uint8_t *initial_value;
    size_t initial_value_len;
    size_t max_value_len;
    h2_pal_ble_gatt_read_fn read;
    h2_pal_ble_gatt_write_fn write;
    void *user;
    uint16_t *out_value_handle;
    uint16_t *out_cccd_handle;
} h2_pal_ble_gatt_characteristic_t;

typedef struct h2_pal_ble_gatt_service {
    h2_pal_ble_uuid_t uuid;
    bool primary;
    const h2_pal_ble_gatt_characteristic_t *characteristics;
    size_t characteristic_count;
    uint16_t *out_service_handle;
} h2_pal_ble_gatt_service_t;

typedef struct h2_pal_ble_gatt_discovery_request {
    h2_pal_ble_gatt_discovery_kind_t kind;
    h2_pal_ble_uuid_t uuid_filter;
    uint16_t start_handle;
    uint16_t end_handle;
} h2_pal_ble_gatt_discovery_request_t;

typedef struct h2_pal_ble_gatt_discovery_entry {
    h2_pal_ble_gatt_discovery_kind_t kind;
    h2_pal_ble_uuid_t uuid;
    uint16_t start_handle;
    uint16_t end_handle;
    uint16_t value_handle;
    uint32_t properties;
} h2_pal_ble_gatt_discovery_entry_t;

typedef struct h2_pal_ble_gatt_subscribe {
    uint16_t value_handle;
    uint16_t cccd_handle;
    h2_pal_ble_subscribe_mode_t mode;
    bool enable;
} h2_pal_ble_gatt_subscribe_t;

typedef struct h2_pal_ble_subscription_state {
    uint16_t conn_handle;
    uint16_t value_handle;
    h2_pal_ble_subscribe_mode_t mode;
    bool enabled;
} h2_pal_ble_subscription_state_t;

typedef struct h2_pal_ble_gatt_client_value {
    uint16_t conn_handle;
    uint16_t attr_handle;
    size_t value_len;
    uint8_t value[H2_PAL_BLE_ATT_MAX_VALUE_LEN];
} h2_pal_ble_gatt_client_value_t;

typedef struct h2_pal_ble_disconnected_info {
    uint16_t conn_handle;
    h2_pal_ble_addr_t peer_addr;
    int reason;
} h2_pal_ble_disconnected_info_t;

typedef struct h2_pal_ble_mtu_info {
    uint16_t conn_handle;
    uint16_t mtu;
} h2_pal_ble_mtu_info_t;

typedef struct h2_pal_ble_phy_info {
    uint16_t conn_handle;
    h2_pal_ble_phy_t tx_phy;
    h2_pal_ble_phy_t rx_phy;
} h2_pal_ble_phy_info_t;

typedef struct h2_pal_ble_vtable {
    h2_pal_result_t (*start)(void *user);
    h2_pal_result_t (*stop)(void *user);
    h2_pal_result_t (*set_adv_data)(void *user, const h2_pal_ble_adv_data_t *data);
    h2_pal_result_t (*start_advertising)(void *user, const h2_pal_ble_adv_params_t *params);
    h2_pal_result_t (*stop_advertising)(void *user);
    h2_pal_result_t (*adv_set_create)(
        void *user,
        const h2_pal_ble_adv_params_t *params,
        h2_pal_ble_adv_set_t **out_set);
    h2_pal_result_t (*adv_set_set_data)(
        void *user,
        h2_pal_ble_adv_set_t *set,
        const h2_pal_ble_adv_data_t *data);
    h2_pal_result_t (*adv_set_start)(void *user, h2_pal_ble_adv_set_t *set);
    h2_pal_result_t (*adv_set_stop)(void *user, h2_pal_ble_adv_set_t *set);
    h2_pal_result_t (*adv_set_destroy)(void *user, h2_pal_ble_adv_set_t *set);
    h2_pal_result_t (*start_scan)(
        void *user,
        const h2_pal_ble_scan_params_t *params,
        h2_pal_ble_scan_result_fn on_result,
        void *scan_user);
    h2_pal_result_t (*stop_scan)(void *user);
    h2_pal_result_t (*register_gatt_services)(void *user, const h2_pal_ble_gatt_service_t *services, size_t count);
    h2_pal_result_t (*unregister_gatt_services)(void *user);
    h2_pal_result_t (*notify)(void *user, uint16_t conn_handle, uint16_t attr_handle, const uint8_t *data, size_t len);
    h2_pal_result_t (*indicate)(
        void *user,
        uint16_t conn_handle,
        uint16_t attr_handle,
        const uint8_t *data,
        size_t len,
        uint32_t timeout_ms);
    h2_pal_result_t (*connect)(
        void *user,
        const h2_pal_ble_addr_t *addr,
        const h2_pal_ble_connect_params_t *params,
        uint16_t *out_conn_handle);
    h2_pal_result_t (*configure_pairing)(
        void *user,
        const h2_pal_ble_pairing_config_t *config);
    h2_pal_result_t (*pair)(
        void *user,
        uint16_t conn_handle,
        uint32_t timeout_ms);
    h2_pal_result_t (*disconnect)(void *user, uint16_t conn_handle);
    h2_pal_result_t (*update_connection)(void *user, uint16_t conn_handle, const h2_pal_ble_connection_params_t *params);
    h2_pal_result_t (*exchange_mtu)(void *user, uint16_t conn_handle, uint16_t *out_mtu, uint32_t timeout_ms);
    h2_pal_result_t (*set_preferred_phy)(
        void *user,
        uint16_t conn_handle,
        h2_pal_ble_phy_t tx_phy,
        h2_pal_ble_phy_t rx_phy,
        uint32_t timeout_ms);
    h2_pal_result_t (*read_phy)(void *user, uint16_t conn_handle, h2_pal_ble_phy_info_t *out_phy, uint32_t timeout_ms);
    h2_pal_result_t (*gatt_discover)(
        void *user,
        uint16_t conn_handle,
        const h2_pal_ble_gatt_discovery_request_t *request,
        h2_pal_ble_gatt_discovery_entry_t *entries,
        size_t max_entries,
        size_t *out_count,
        uint32_t timeout_ms);
    h2_pal_result_t (*gatt_read)(
        void *user,
        uint16_t conn_handle,
        uint16_t attr_handle,
        uint16_t offset,
        uint8_t *out,
        size_t out_size,
        size_t *out_len,
        uint32_t timeout_ms);
    h2_pal_result_t (*gatt_write)(
        void *user,
        uint16_t conn_handle,
        uint16_t attr_handle,
        const uint8_t *data,
        size_t len,
        bool with_response,
        uint32_t timeout_ms);
    h2_pal_result_t (*gatt_subscribe)(
        void *user,
        uint16_t conn_handle,
        const h2_pal_ble_gatt_subscribe_t *subscribe,
        uint32_t timeout_ms);
} h2_pal_ble_vtable_t;

struct h2_pal_ble_api {
    void *user;
    const h2_pal_ble_vtable_t *vtable;
    const h2_pal_mem_api_t *allocator;
};

static inline bool h2_pal_ble_adv_params_valid(
    const h2_pal_ble_adv_params_t *params) {
    if (params == NULL ||
        (params->mode != H2_PAL_BLE_ADV_MODE_CONNECTABLE &&
         params->mode != H2_PAL_BLE_ADV_MODE_NON_CONNECTABLE) ||
        (params->type != H2_PAL_BLE_ADV_TYPE_LEGACY &&
         params->type != H2_PAL_BLE_ADV_TYPE_EXTENDED) ||
        params->interval_min_ms < H2_PAL_BLE_ADV_INTERVAL_MIN_MS ||
        params->interval_min_ms > params->interval_max_ms) {
        return false;
    }
    if (params->type == H2_PAL_BLE_ADV_TYPE_LEGACY) {
        return params->interval_max_ms <= H2_PAL_BLE_LEGACY_ADV_INTERVAL_MAX_MS &&
               (params->primary_phy == H2_PAL_BLE_PHY_UNKNOWN ||
                params->primary_phy == H2_PAL_BLE_PHY_1M) &&
               (params->secondary_phy == H2_PAL_BLE_PHY_UNKNOWN ||
                params->secondary_phy == H2_PAL_BLE_PHY_1M) &&
               params->sid == 0u && params->max_adv_events == 0u &&
               params->duration_ms == 0u;
    }
    return params->interval_max_ms <= H2_PAL_BLE_EXT_ADV_INTERVAL_MAX_MS &&
           (params->primary_phy == H2_PAL_BLE_PHY_UNKNOWN ||
            params->primary_phy == H2_PAL_BLE_PHY_1M ||
            params->primary_phy == H2_PAL_BLE_PHY_CODED) &&
           (params->secondary_phy == H2_PAL_BLE_PHY_UNKNOWN ||
            params->secondary_phy == H2_PAL_BLE_PHY_1M ||
            params->secondary_phy == H2_PAL_BLE_PHY_2M ||
            params->secondary_phy == H2_PAL_BLE_PHY_CODED) &&
           params->sid <= H2_PAL_BLE_EXT_ADV_SID_MAX &&
           params->duration_ms <= H2_PAL_BLE_ADV_DURATION_MAX_MS;
}

static inline const h2_pal_mem_api_t *h2_pal_ble_allocator(const h2_pal_ble_host_api_t *ble) {
    if (ble == NULL) {
        return NULL;
    }
    return ble->allocator;
}

static inline h2_pal_result_t h2_pal_ble_start(const h2_pal_ble_host_api_t *ble) {
    if (ble == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (ble->vtable == NULL || ble->vtable->start == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return ble->vtable->start(ble->user);
}

static inline h2_pal_result_t h2_pal_ble_stop(const h2_pal_ble_host_api_t *ble) {
    if (ble == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (ble->vtable == NULL || ble->vtable->stop == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return ble->vtable->stop(ble->user);
}

static inline h2_pal_result_t h2_pal_ble_set_adv_data(
    const h2_pal_ble_host_api_t *ble,
    const h2_pal_ble_adv_data_t *data) {
    if (ble == NULL || data == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (ble->vtable == NULL || ble->vtable->set_adv_data == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return ble->vtable->set_adv_data(ble->user, data);
}

static inline h2_pal_result_t h2_pal_ble_start_advertising(
    const h2_pal_ble_host_api_t *ble,
    const h2_pal_ble_adv_params_t *params) {
    if (ble == NULL || params == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!h2_pal_ble_adv_params_valid(params)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (ble->vtable == NULL || ble->vtable->start_advertising == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return ble->vtable->start_advertising(ble->user, params);
}

/**
 * @brief Create an independent advertising set owned by the BLE Host.
 *
 * The backend copies @p params before returning. The returned handle remains
 * valid until h2_pal_ble_adv_set_destroy() succeeds or the Host is stopped.
 */
static inline h2_pal_result_t h2_pal_ble_adv_set_create(
    const h2_pal_ble_host_api_t *ble,
    const h2_pal_ble_adv_params_t *params,
    h2_pal_ble_adv_set_t **out_set) {
    if (out_set != NULL) {
        *out_set = NULL;
    }
    if (ble == NULL || params == NULL || out_set == NULL ||
        !h2_pal_ble_adv_params_valid(params)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (ble->vtable == NULL || ble->vtable->adv_set_create == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return ble->vtable->adv_set_create(ble->user, params, out_set);
}

/** Copy advertising data into one advertising set. */
static inline h2_pal_result_t h2_pal_ble_adv_set_set_data(
    const h2_pal_ble_host_api_t *ble,
    h2_pal_ble_adv_set_t *set,
    const h2_pal_ble_adv_data_t *data) {
    if (ble == NULL || set == NULL || data == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (ble->vtable == NULL || ble->vtable->adv_set_set_data == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return ble->vtable->adv_set_set_data(ble->user, set, data);
}

/** Start one advertising set without changing any other set. */
static inline h2_pal_result_t h2_pal_ble_adv_set_start(
    const h2_pal_ble_host_api_t *ble,
    h2_pal_ble_adv_set_t *set) {
    if (ble == NULL || set == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (ble->vtable == NULL || ble->vtable->adv_set_start == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return ble->vtable->adv_set_start(ble->user, set);
}

/** Stop one advertising set. Repeated stop is successful. */
static inline h2_pal_result_t h2_pal_ble_adv_set_stop(
    const h2_pal_ble_host_api_t *ble,
    h2_pal_ble_adv_set_t *set) {
    if (ble == NULL || set == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (ble->vtable == NULL || ble->vtable->adv_set_stop == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return ble->vtable->adv_set_stop(ble->user, set);
}

/**
 * Request destruction of one set.
 *
 * A successful return invalidates the caller's handle immediately. The
 * provider may finish target stop/delete work asynchronously, but retains no
 * caller-owned storage while doing so.
 */
static inline h2_pal_result_t h2_pal_ble_adv_set_destroy(
    const h2_pal_ble_host_api_t *ble,
    h2_pal_ble_adv_set_t *set) {
    if (ble == NULL || set == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (ble->vtable == NULL || ble->vtable->adv_set_destroy == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return ble->vtable->adv_set_destroy(ble->user, set);
}

static inline h2_pal_result_t h2_pal_ble_stop_advertising(const h2_pal_ble_host_api_t *ble) {
    if (ble == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (ble->vtable == NULL || ble->vtable->stop_advertising == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return ble->vtable->stop_advertising(ble->user);
}

static inline h2_pal_result_t h2_pal_ble_start_scan(
    const h2_pal_ble_host_api_t *ble,
    const h2_pal_ble_scan_params_t *params,
    h2_pal_ble_scan_result_fn on_result,
    void *user) {
    if (ble == NULL || params == NULL || on_result == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if ((params->mode != H2_PAL_BLE_SCAN_MODE_PASSIVE &&
         params->mode != H2_PAL_BLE_SCAN_MODE_ACTIVE) ||
        (params->type != H2_PAL_BLE_SCAN_TYPE_LEGACY &&
         params->type != H2_PAL_BLE_SCAN_TYPE_EXTENDED) ||
        params->interval_ms < H2_PAL_BLE_SCAN_INTERVAL_MIN_MS ||
        params->window_ms < H2_PAL_BLE_SCAN_INTERVAL_MIN_MS ||
        params->window_ms > params->interval_ms ||
        params->timeout_ms > H2_PAL_BLE_SCAN_DURATION_MAX_MS) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_ble_scan_phy_mask_t phy_mask = params->phy_mask == 0u
                                              ? H2_PAL_BLE_SCAN_PHY_1M
                                              : params->phy_mask;
    if ((phy_mask & (h2_pal_ble_scan_phy_mask_t)~H2_PAL_BLE_SCAN_PHY_ALL) != 0u ||
        (params->type == H2_PAL_BLE_SCAN_TYPE_LEGACY &&
         (phy_mask != H2_PAL_BLE_SCAN_PHY_1M ||
          params->interval_ms > H2_PAL_BLE_LEGACY_SCAN_INTERVAL_MAX_MS)) ||
        (params->type == H2_PAL_BLE_SCAN_TYPE_EXTENDED &&
         params->interval_ms > H2_PAL_BLE_EXT_SCAN_INTERVAL_MAX_MS)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (ble->vtable == NULL || ble->vtable->start_scan == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return ble->vtable->start_scan(ble->user, params, on_result, user);
}

static inline h2_pal_result_t h2_pal_ble_stop_scan(const h2_pal_ble_host_api_t *ble) {
    if (ble == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (ble->vtable == NULL || ble->vtable->stop_scan == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return ble->vtable->stop_scan(ble->user);
}

/**
 * Register a GATT schema whose declarations and callbacks remain borrowed.
 *
 * The caller keeps the service, characteristic, UUID, initial-value, callback
 * context, and output-handle storage valid until unregister_gatt_services()
 * or Host stop completes. Providers may impose implementation-specific schema
 * and resource limits. Resource exhaustion returns H2_PAL_ERR_NO_SPACE without
 * exposing a partial schema.
 */
static inline h2_pal_result_t h2_pal_ble_register_gatt_services(
    const h2_pal_ble_host_api_t *ble,
    const h2_pal_ble_gatt_service_t *services,
    size_t count) {
    if (ble == NULL || (count > 0u && services == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (ble->vtable == NULL || ble->vtable->register_gatt_services == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return ble->vtable->register_gatt_services(ble->user, services, count);
}

static inline h2_pal_result_t h2_pal_ble_unregister_gatt_services(
    const h2_pal_ble_host_api_t *ble) {
    if (ble == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (ble->vtable == NULL || ble->vtable->unregister_gatt_services == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return ble->vtable->unregister_gatt_services(ble->user);
}

static inline h2_pal_result_t h2_pal_ble_notify(
    const h2_pal_ble_host_api_t *ble,
    uint16_t conn_handle,
    uint16_t attr_handle,
    const uint8_t *data,
    size_t len) {
    if (ble == NULL || conn_handle == H2_PAL_BLE_INVALID_CONN_HANDLE ||
        attr_handle == H2_PAL_BLE_INVALID_ATTR_HANDLE || (len > 0u && data == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (ble->vtable == NULL || ble->vtable->notify == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return ble->vtable->notify(ble->user, conn_handle, attr_handle, data, len);
}

/**
 * Send a server indication and wait for its terminal result.
 *
 * Data remains borrowed only until this call returns. H2_PAL_OK means the peer
 * confirmed the ATT indication. Submission failure, protocol rejection,
 * disconnect, Host stop, and timeout return directly. A zero timeout may
 * succeed only if completion is already observable; otherwise the provider
 * returns H2_PAL_ERR_WOULD_BLOCK without submitting untracked work.
 */
static inline h2_pal_result_t h2_pal_ble_indicate(
    const h2_pal_ble_t *ble,
    uint16_t conn_handle,
    uint16_t attr_handle,
    const uint8_t *data,
    size_t len,
    uint32_t timeout_ms) {
    if (ble == NULL || conn_handle == H2_PAL_BLE_INVALID_CONN_HANDLE ||
        attr_handle == H2_PAL_BLE_INVALID_ATTR_HANDLE ||
        (len != 0u && data == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (ble->vtable == NULL || ble->vtable->indicate == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return ble->vtable->indicate(
        ble->user, conn_handle, attr_handle, data, len, timeout_ms);
}

static inline h2_pal_result_t h2_pal_ble_connect(
    const h2_pal_ble_host_api_t *ble,
    const h2_pal_ble_addr_t *addr,
    const h2_pal_ble_connect_params_t *params,
    uint16_t *out_conn_handle) {
    if (ble == NULL || addr == NULL || params == NULL || out_conn_handle == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (ble->vtable == NULL || ble->vtable->connect == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    *out_conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
    return ble->vtable->connect(ble->user, addr, params, out_conn_handle);
}

static inline h2_pal_result_t h2_pal_ble_configure_pairing(
    const h2_pal_ble_host_api_t *ble,
    const h2_pal_ble_pairing_config_t *config) {
    if (ble == NULL || config == NULL ||
        (config->enabled &&
         (config->passkey < 100000u || config->passkey > 999999u ||
          (config->io != H2_PAL_BLE_PAIRING_IO_DISPLAY_ONLY &&
           config->io != H2_PAL_BLE_PAIRING_IO_KEYBOARD_ONLY)))) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (ble->vtable == NULL || ble->vtable->configure_pairing == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return ble->vtable->configure_pairing(ble->user, config);
}

static inline h2_pal_result_t h2_pal_ble_pair(
    const h2_pal_ble_host_api_t *ble,
    uint16_t conn_handle,
    uint32_t timeout_ms) {
    if (ble == NULL || conn_handle == H2_PAL_BLE_INVALID_CONN_HANDLE ||
        timeout_ms == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (ble->vtable == NULL || ble->vtable->pair == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return ble->vtable->pair(ble->user, conn_handle, timeout_ms);
}

static inline h2_pal_result_t h2_pal_ble_disconnect(
    const h2_pal_ble_host_api_t *ble,
    uint16_t conn_handle) {
    if (ble == NULL || conn_handle == H2_PAL_BLE_INVALID_CONN_HANDLE) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (ble->vtable == NULL || ble->vtable->disconnect == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return ble->vtable->disconnect(ble->user, conn_handle);
}

static inline h2_pal_result_t h2_pal_ble_update_connection(
    const h2_pal_ble_host_api_t *ble,
    uint16_t conn_handle,
    const h2_pal_ble_connection_params_t *params) {
    if (ble == NULL || conn_handle == H2_PAL_BLE_INVALID_CONN_HANDLE || params == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (ble->vtable == NULL || ble->vtable->update_connection == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return ble->vtable->update_connection(ble->user, conn_handle, params);
}

static inline h2_pal_result_t h2_pal_ble_exchange_mtu(
    const h2_pal_ble_host_api_t *ble,
    uint16_t conn_handle,
    uint16_t *out_mtu,
    uint32_t timeout_ms) {
    if (ble == NULL || conn_handle == H2_PAL_BLE_INVALID_CONN_HANDLE || out_mtu == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (ble->vtable == NULL || ble->vtable->exchange_mtu == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    *out_mtu = 0u;
    return ble->vtable->exchange_mtu(ble->user, conn_handle, out_mtu, timeout_ms);
}

static inline h2_pal_result_t h2_pal_ble_set_preferred_phy(
    const h2_pal_ble_host_api_t *ble,
    uint16_t conn_handle,
    h2_pal_ble_phy_t tx_phy,
    h2_pal_ble_phy_t rx_phy,
    uint32_t timeout_ms) {
    if (ble == NULL || conn_handle == H2_PAL_BLE_INVALID_CONN_HANDLE ||
        tx_phy == H2_PAL_BLE_PHY_UNKNOWN || rx_phy == H2_PAL_BLE_PHY_UNKNOWN) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (ble->vtable == NULL || ble->vtable->set_preferred_phy == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return ble->vtable->set_preferred_phy(ble->user, conn_handle, tx_phy, rx_phy, timeout_ms);
}

static inline h2_pal_result_t h2_pal_ble_read_phy(
    const h2_pal_ble_host_api_t *ble,
    uint16_t conn_handle,
    h2_pal_ble_phy_info_t *out_phy,
    uint32_t timeout_ms) {
    if (ble == NULL || conn_handle == H2_PAL_BLE_INVALID_CONN_HANDLE || out_phy == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (ble->vtable == NULL || ble->vtable->read_phy == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    out_phy->conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
    out_phy->tx_phy = H2_PAL_BLE_PHY_UNKNOWN;
    out_phy->rx_phy = H2_PAL_BLE_PHY_UNKNOWN;
    return ble->vtable->read_phy(ble->user, conn_handle, out_phy, timeout_ms);
}

static inline h2_pal_result_t h2_pal_ble_gatt_discover(
    const h2_pal_ble_host_api_t *ble,
    uint16_t conn_handle,
    const h2_pal_ble_gatt_discovery_request_t *request,
    h2_pal_ble_gatt_discovery_entry_t *entries,
    size_t max_entries,
    size_t *out_count,
    uint32_t timeout_ms) {
    if (ble == NULL || conn_handle == H2_PAL_BLE_INVALID_CONN_HANDLE ||
        request == NULL || out_count == NULL || (max_entries > 0u && entries == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (ble->vtable == NULL || ble->vtable->gatt_discover == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    *out_count = 0u;
    return ble->vtable->gatt_discover(ble->user, conn_handle, request, entries, max_entries, out_count, timeout_ms);
}

static inline h2_pal_result_t h2_pal_ble_gatt_read(
    const h2_pal_ble_host_api_t *ble,
    uint16_t conn_handle,
    uint16_t attr_handle,
    uint16_t offset,
    uint8_t *out,
    size_t out_size,
    size_t *out_len,
    uint32_t timeout_ms) {
    if (ble == NULL || conn_handle == H2_PAL_BLE_INVALID_CONN_HANDLE ||
        attr_handle == H2_PAL_BLE_INVALID_ATTR_HANDLE ||
        out_len == NULL || (out_size > 0u && out == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (ble->vtable == NULL || ble->vtable->gatt_read == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    *out_len = 0u;
    return ble->vtable->gatt_read(ble->user, conn_handle, attr_handle, offset, out, out_size, out_len, timeout_ms);
}

static inline h2_pal_result_t h2_pal_ble_gatt_write(
    const h2_pal_ble_host_api_t *ble,
    uint16_t conn_handle,
    uint16_t attr_handle,
    const uint8_t *data,
    size_t len,
    bool with_response,
    uint32_t timeout_ms) {
    if (ble == NULL || conn_handle == H2_PAL_BLE_INVALID_CONN_HANDLE ||
        attr_handle == H2_PAL_BLE_INVALID_ATTR_HANDLE || (len > 0u && data == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (ble->vtable == NULL || ble->vtable->gatt_write == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return ble->vtable->gatt_write(ble->user, conn_handle, attr_handle, data, len, with_response, timeout_ms);
}

static inline h2_pal_result_t h2_pal_ble_gatt_subscribe(
    const h2_pal_ble_host_api_t *ble,
    uint16_t conn_handle,
    const h2_pal_ble_gatt_subscribe_t *subscribe,
    uint32_t timeout_ms) {
    if (ble == NULL || conn_handle == H2_PAL_BLE_INVALID_CONN_HANDLE || subscribe == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (ble->vtable == NULL || ble->vtable->gatt_subscribe == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return ble->vtable->gatt_subscribe(ble->user, conn_handle, subscribe, timeout_ms);
}

#ifdef __cplusplus
}
#endif

#endif
