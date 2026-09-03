#ifndef H2_ESP_BLE_PAIR_TRACKER_H
#define H2_ESP_BLE_PAIR_TRACKER_H

#include "h2/pal/hal/h2_pal_ble.h"

#include <stdbool.h>

typedef enum h2_esp_ble_pair_state {
    H2_ESP_BLE_PAIR_IDLE = 0,
    H2_ESP_BLE_PAIR_WAITING,
    H2_ESP_BLE_PAIR_DRAINING,
    H2_ESP_BLE_PAIR_COMPLETED,
} h2_esp_ble_pair_state_t;

typedef struct h2_esp_ble_pair_tracker {
    h2_esp_ble_pair_state_t state;
    uint16_t conn_handle;
    h2_pal_result_t result;
} h2_esp_ble_pair_tracker_t;

static inline void h2_esp_ble_pair_tracker_clear(
    h2_esp_ble_pair_tracker_t *tracker) {
    tracker->state = H2_ESP_BLE_PAIR_IDLE;
    tracker->conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
    tracker->result = H2_PAL_ERR_IO;
}

static inline h2_pal_result_t h2_esp_ble_pair_tracker_begin(
    h2_esp_ble_pair_tracker_t *tracker,
    uint16_t conn_handle) {
    if (tracker->state != H2_ESP_BLE_PAIR_IDLE) {
        return H2_PAL_ERR_BUSY;
    }
    tracker->state = H2_ESP_BLE_PAIR_WAITING;
    tracker->conn_handle = conn_handle;
    tracker->result = H2_PAL_ERR_IO;
    return H2_PAL_OK;
}

static inline bool h2_esp_ble_pair_tracker_complete(
    h2_esp_ble_pair_tracker_t *tracker,
    uint16_t conn_handle,
    h2_pal_result_t result) {
    if (tracker->state == H2_ESP_BLE_PAIR_IDLE ||
        tracker->state == H2_ESP_BLE_PAIR_COMPLETED ||
        tracker->conn_handle != conn_handle ||
        conn_handle == H2_PAL_BLE_INVALID_CONN_HANDLE) {
        return false;
    }
    if (tracker->state == H2_ESP_BLE_PAIR_DRAINING) {
        h2_esp_ble_pair_tracker_clear(tracker);
        return false;
    }
    tracker->result = result;
    tracker->state = H2_ESP_BLE_PAIR_COMPLETED;
    return true;
}

static inline h2_pal_result_t h2_esp_ble_pair_tracker_take(
    h2_esp_ble_pair_tracker_t *tracker) {
    if (tracker->state != H2_ESP_BLE_PAIR_COMPLETED) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    h2_pal_result_t result = tracker->result;
    h2_esp_ble_pair_tracker_clear(tracker);
    return result;
}

static inline h2_pal_result_t h2_esp_ble_pair_tracker_timeout(
    h2_esp_ble_pair_tracker_t *tracker) {
    if (tracker->state == H2_ESP_BLE_PAIR_COMPLETED) {
        return h2_esp_ble_pair_tracker_take(tracker);
    }
    if (tracker->state == H2_ESP_BLE_PAIR_WAITING) {
        tracker->state = H2_ESP_BLE_PAIR_DRAINING;
        return H2_PAL_ERR_TIMEOUT;
    }
    return H2_PAL_ERR_INVALID_STATE;
}

static inline void h2_esp_ble_pair_tracker_cancel_submission(
    h2_esp_ble_pair_tracker_t *tracker,
    uint16_t conn_handle) {
    if (tracker->state == H2_ESP_BLE_PAIR_WAITING &&
        tracker->conn_handle == conn_handle) {
        h2_esp_ble_pair_tracker_clear(tracker);
    }
}

#endif
