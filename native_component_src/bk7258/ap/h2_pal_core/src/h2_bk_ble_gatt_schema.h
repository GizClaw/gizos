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
 * Attributes one EtherMind service declaration occupies: the service itself,
 * then one entry per characteristic plus one more for its CCCD when it
 * notifies or indicates. The SDK expands the declaration and the value from a
 * single entry, so two per characteristic is the worst case.
 */
#define H2_BK_BLE_ATTR_COUNT \
    (1u + 2u * H2_BK_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE)

/**
 * Attributes the legacy stack's fixed database occupies. It spells out the
 * declaration, the value and the CCCD separately, so it needs three per
 * characteristic whether or not that characteristic notifies.
 */
#define H2_BK_BLE_LEGACY_ATTR_COUNT \
    (1u + 3u * H2_BK_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE)

/** Index of the service declaration in the legacy database. */
#define H2_BK_BLE_LEGACY_SERVICE_INDEX 0u

/** Legacy attribute index of one characteristic's declaration. */
static inline size_t h2_bk_ble_legacy_declaration_index(
    size_t characteristic_index) {
    return 1u + 3u * characteristic_index;
}

/** Legacy attribute index of one characteristic's value. */
static inline size_t h2_bk_ble_legacy_value_slot(size_t characteristic_index) {
    return h2_bk_ble_legacy_declaration_index(characteristic_index) + 1u;
}

/** Legacy attribute index of one characteristic's CCCD. */
static inline size_t h2_bk_ble_legacy_cccd_slot(size_t characteristic_index) {
    return h2_bk_ble_legacy_declaration_index(characteristic_index) + 2u;
}

/**
 * Characteristic owning a legacy value attribute, or -1.
 *
 * @param characteristic_count Characteristics the service actually
 * registered, so attributes past the live layout are not claimed.
 */
static inline int h2_bk_ble_legacy_characteristic_from_value(
    size_t att_index,
    size_t characteristic_count) {
    for (size_t i = 0u; i < characteristic_count; ++i) {
        if (h2_bk_ble_legacy_value_slot(i) == att_index) {
            return (int)i;
        }
    }
    return -1;
}

/** Characteristic owning a legacy CCCD attribute, or -1. */
static inline int h2_bk_ble_legacy_characteristic_from_cccd(
    size_t att_index,
    size_t characteristic_count) {
    for (size_t i = 0u; i < characteristic_count; ++i) {
        if (h2_bk_ble_legacy_cccd_slot(i) == att_index) {
            return (int)i;
        }
    }
    return -1;
}

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
