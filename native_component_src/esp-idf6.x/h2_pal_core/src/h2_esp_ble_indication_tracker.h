#ifndef H2_ESP_BLE_INDICATION_TRACKER_H
#define H2_ESP_BLE_INDICATION_TRACKER_H

#include "h2/pal/hal/h2_pal_ble.h"

typedef enum h2_esp_ble_indication_state {
    H2_ESP_BLE_INDICATION_IDLE = 0,
    H2_ESP_BLE_INDICATION_WAITING,
    H2_ESP_BLE_INDICATION_ABANDONED,
    H2_ESP_BLE_INDICATION_CONFIRMED,
} h2_esp_ble_indication_state_t;

typedef struct h2_esp_ble_indication_tracker {
    h2_esp_ble_indication_state_t state;
    uint16_t conn_handle;
    uint16_t attr_handle;
    h2_pal_result_t result;
} h2_esp_ble_indication_tracker_t;

static inline void h2_esp_ble_indication_tracker_clear(
    h2_esp_ble_indication_tracker_t *tracker) {
    tracker->state = H2_ESP_BLE_INDICATION_IDLE;
    tracker->conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
    tracker->attr_handle = H2_PAL_BLE_INVALID_ATTR_HANDLE;
    tracker->result = H2_PAL_OK;
}

static inline h2_pal_result_t h2_esp_ble_indication_tracker_begin(
    h2_esp_ble_indication_tracker_t *tracker,
    uint16_t conn_handle,
    uint16_t attr_handle) {
    if (tracker->state != H2_ESP_BLE_INDICATION_IDLE) {
        return H2_PAL_ERR_BUSY;
    }
    tracker->state = H2_ESP_BLE_INDICATION_WAITING;
    tracker->conn_handle = conn_handle;
    tracker->attr_handle = attr_handle;
    tracker->result = H2_PAL_ERR_WOULD_BLOCK;
    return H2_PAL_OK;
}

static inline bool h2_esp_ble_indication_tracker_finish(
    h2_esp_ble_indication_tracker_t *tracker,
    uint16_t conn_handle,
    uint16_t attr_handle,
    bool match_attr,
    h2_pal_result_t result) {
    if (tracker->state == H2_ESP_BLE_INDICATION_IDLE ||
        tracker->conn_handle != conn_handle ||
        (match_attr && tracker->attr_handle != attr_handle)) {
        return false;
    }
    if (tracker->state == H2_ESP_BLE_INDICATION_ABANDONED) {
        h2_esp_ble_indication_tracker_clear(tracker);
        return false;
    }
    tracker->result = result;
    tracker->state = H2_ESP_BLE_INDICATION_CONFIRMED;
    return true;
}

static inline h2_pal_result_t h2_esp_ble_indication_tracker_take(
    h2_esp_ble_indication_tracker_t *tracker) {
    if (tracker->state != H2_ESP_BLE_INDICATION_CONFIRMED) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    h2_pal_result_t result = tracker->result;
    h2_esp_ble_indication_tracker_clear(tracker);
    return result;
}

static inline h2_pal_result_t h2_esp_ble_indication_tracker_timeout(
    h2_esp_ble_indication_tracker_t *tracker) {
    if (tracker->state == H2_ESP_BLE_INDICATION_CONFIRMED) {
        return h2_esp_ble_indication_tracker_take(tracker);
    }
    if (tracker->state == H2_ESP_BLE_INDICATION_WAITING) {
        tracker->state = H2_ESP_BLE_INDICATION_ABANDONED;
        return H2_PAL_ERR_TIMEOUT;
    }
    return H2_PAL_ERR_INVALID_STATE;
}

static inline void h2_esp_ble_indication_tracker_cancel_submission(
    h2_esp_ble_indication_tracker_t *tracker,
    uint16_t conn_handle,
    uint16_t attr_handle) {
    if (tracker->conn_handle == conn_handle &&
        tracker->attr_handle == attr_handle &&
        tracker->state == H2_ESP_BLE_INDICATION_WAITING) {
        h2_esp_ble_indication_tracker_clear(tracker);
    }
}

#endif
