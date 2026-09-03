#ifndef H2_ESP_BLE_PAIR_TRACKER_H
#define H2_ESP_BLE_PAIR_TRACKER_H

#include "h2/pal/hal/h2_pal_ble.h"

#include <stdbool.h>

typedef struct h2_esp_ble_pair_tracker {
    uint16_t conn_handle;
    h2_pal_result_t result;
    bool completed;
} h2_esp_ble_pair_tracker_t;

static inline void h2_esp_ble_pair_tracker_clear(
    h2_esp_ble_pair_tracker_t *tracker) {
    tracker->conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
    tracker->result = H2_PAL_ERR_IO;
    tracker->completed = false;
}

static inline void h2_esp_ble_pair_tracker_begin(
    h2_esp_ble_pair_tracker_t *tracker,
    uint16_t conn_handle) {
    tracker->conn_handle = conn_handle;
    tracker->result = H2_PAL_ERR_IO;
    tracker->completed = false;
}

static inline bool h2_esp_ble_pair_tracker_complete(
    h2_esp_ble_pair_tracker_t *tracker,
    uint16_t conn_handle,
    h2_pal_result_t result) {
    if (tracker->completed || tracker->conn_handle != conn_handle ||
        conn_handle == H2_PAL_BLE_INVALID_CONN_HANDLE) {
        return false;
    }
    tracker->result = result;
    tracker->completed = true;
    return true;
}

static inline h2_pal_result_t h2_esp_ble_pair_tracker_take(
    h2_esp_ble_pair_tracker_t *tracker) {
    h2_pal_result_t result = tracker->result;
    h2_esp_ble_pair_tracker_clear(tracker);
    return result;
}

#endif
