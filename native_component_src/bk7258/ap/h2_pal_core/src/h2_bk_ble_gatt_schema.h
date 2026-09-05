#ifndef H2_BK_BLE_GATT_SCHEMA_H
#define H2_BK_BLE_GATT_SCHEMA_H

#include <stdbool.h>
#include <stddef.h>

/**
 * Static capacity of the BK GATT server schema.
 *
 * Three characteristics per service, because libs/ble_wifi_config publishes a
 * command, a scan and a provisioning characteristic in one service. Every
 * table is sized off these, so a slot only costs static storage.
 */
#define H2_BK_BLE_MAX_GATT_SERVICES 4u
#define H2_BK_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE 3u
#define H2_BK_BLE_MAX_GATT_CHARACTERISTICS \
    (H2_BK_BLE_MAX_GATT_SERVICES * H2_BK_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE)

/**
 * Attributes one service declaration occupies: the service itself, then a
 * declaration and a value attribute for every characteristic.
 */
#define H2_BK_BLE_ATTR_COUNT \
    (1u + 2u * H2_BK_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE)

/** Whether a registration request fits the static schema. */
static inline bool h2_bk_ble_gatt_schema_accepts(
    size_t service_count,
    size_t characteristic_count) {
    return service_count <= H2_BK_BLE_MAX_GATT_SERVICES &&
           characteristic_count <= H2_BK_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE;
}

/** First flat characteristic slot owned by one service. */
static inline size_t h2_bk_ble_gatt_schema_first_slot(size_t service_index) {
    return service_index * H2_BK_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE;
}

#endif
