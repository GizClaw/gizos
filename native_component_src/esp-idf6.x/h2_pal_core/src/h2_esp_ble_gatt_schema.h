#ifndef H2_ESP_BLE_GATT_SCHEMA_H
#define H2_ESP_BLE_GATT_SCHEMA_H

#include "h2/pal/hal/h2_pal_ble.h"

#include <stdbool.h>
#include <stddef.h>

/**
 * Static capacity of the NimBLE GATT server schema.
 *
 * Three characteristics per service, because libs/ble_wifi_config publishes a
 * command, a scan and a provisioning characteristic in one service. Every
 * table is sized off these, so a slot only costs static storage.
 */
#define H2_ESP_BLE_MAX_GATT_SERVICES 2u
#define H2_ESP_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE 3u
#define H2_ESP_BLE_MAX_GATT_CHARACTERISTICS \
    (H2_ESP_BLE_MAX_GATT_SERVICES * \
     H2_ESP_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE)

/** Whether a registration request fits the static schema. */
static inline bool h2_esp_ble_gatt_schema_accepts(
    size_t service_count,
    size_t characteristic_count) {
    return service_count <= H2_ESP_BLE_MAX_GATT_SERVICES &&
           characteristic_count <= H2_ESP_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE;
}

/**
 * Flat slot of one characteristic.
 *
 * The slot is the index NimBLE hands back to the access callback, so it must
 * be derived rather than taken from a literal table: a stale literal would
 * leave later slots reading as zero and dispatch one characteristic's reads
 * and writes to another.
 */
static inline size_t h2_esp_ble_gatt_schema_slot(
    size_t service_index,
    size_t characteristic_index) {
    return service_index * H2_ESP_BLE_MAX_GATT_CHARACTERISTICS_PER_SERVICE +
           characteristic_index;
}

/**
 * Bind every characteristic slot of one service to its own callback index.
 *
 * @p indices must hold H2_ESP_BLE_MAX_GATT_CHARACTERISTICS entries. NimBLE
 * hands each entry back to the access callback as its arg, so an entry that
 * kept a stale value would dispatch that characteristic's reads and writes to
 * whichever slot the value names. Binding covers the whole service, including
 * the later characteristics of the second registered service.
 */
static inline void h2_esp_ble_gatt_schema_bind_indices(
    uint8_t *indices,
    size_t service_index,
    size_t characteristic_count) {
    for (size_t i = 0u; i < characteristic_count; ++i) {
        size_t slot = h2_esp_ble_gatt_schema_slot(service_index, i);
        indices[slot] = (uint8_t)slot;
    }
}

#endif
