#ifndef H2_BK_BLE_GATTS_TX_TRACKER_H
#define H2_BK_BLE_GATTS_TX_TRACKER_H

#include "h2/pal/hal/h2_pal_ble.h"

typedef enum h2_bk_ble_gatts_tx_kind {
    H2_BK_BLE_GATTS_TX_IDLE = 0,
    H2_BK_BLE_GATTS_TX_NOTIFY,
    H2_BK_BLE_GATTS_TX_INDICATE_WAITING,
    H2_BK_BLE_GATTS_TX_INDICATE_ABANDONED,
    H2_BK_BLE_GATTS_TX_INDICATE_CONFIRMED,
} h2_bk_ble_gatts_tx_kind_t;

typedef struct h2_bk_ble_gatts_tx_tracker {
    h2_bk_ble_gatts_tx_kind_t kind;
    uint16_t conn_handle;
    uint16_t attr_handle;
    h2_pal_result_t result;
} h2_bk_ble_gatts_tx_tracker_t;

static inline void h2_bk_ble_gatts_tx_tracker_clear(
    h2_bk_ble_gatts_tx_tracker_t *tracker) {
    tracker->kind = H2_BK_BLE_GATTS_TX_IDLE;
    tracker->conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
    tracker->attr_handle = H2_PAL_BLE_INVALID_ATTR_HANDLE;
    tracker->result = H2_PAL_OK;
}

static inline h2_pal_result_t h2_bk_ble_gatts_tx_tracker_begin(
    h2_bk_ble_gatts_tx_tracker_t *tracker,
    h2_bk_ble_gatts_tx_kind_t kind,
    uint16_t conn_handle,
    uint16_t attr_handle) {
    if (tracker->kind != H2_BK_BLE_GATTS_TX_IDLE) {
        return H2_PAL_ERR_BUSY;
    }
    tracker->kind = kind;
    tracker->conn_handle = conn_handle;
    tracker->attr_handle = attr_handle;
    tracker->result = H2_PAL_ERR_WOULD_BLOCK;
    return H2_PAL_OK;
}

static inline h2_bk_ble_gatts_tx_kind_t
h2_bk_ble_gatts_tx_tracker_finish(
    h2_bk_ble_gatts_tx_tracker_t *tracker,
    uint16_t conn_handle,
    h2_pal_result_t result) {
    if (tracker->kind == H2_BK_BLE_GATTS_TX_IDLE ||
        tracker->conn_handle != conn_handle) {
        return H2_BK_BLE_GATTS_TX_IDLE;
    }
    h2_bk_ble_gatts_tx_kind_t kind = tracker->kind;
    if (kind == H2_BK_BLE_GATTS_TX_NOTIFY) {
        h2_bk_ble_gatts_tx_tracker_clear(tracker);
    } else if (kind == H2_BK_BLE_GATTS_TX_INDICATE_ABANDONED) {
        h2_bk_ble_gatts_tx_tracker_clear(tracker);
        kind = H2_BK_BLE_GATTS_TX_IDLE;
    } else {
        tracker->result = result;
        tracker->kind = H2_BK_BLE_GATTS_TX_INDICATE_CONFIRMED;
    }
    return kind;
}

static inline h2_pal_result_t h2_bk_ble_gatts_tx_tracker_take(
    h2_bk_ble_gatts_tx_tracker_t *tracker) {
    if (tracker->kind != H2_BK_BLE_GATTS_TX_INDICATE_CONFIRMED) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    h2_pal_result_t result = tracker->result;
    h2_bk_ble_gatts_tx_tracker_clear(tracker);
    return result;
}

static inline h2_pal_result_t h2_bk_ble_gatts_tx_tracker_timeout(
    h2_bk_ble_gatts_tx_tracker_t *tracker) {
    if (tracker->kind == H2_BK_BLE_GATTS_TX_INDICATE_CONFIRMED) {
        return h2_bk_ble_gatts_tx_tracker_take(tracker);
    }
    if (tracker->kind == H2_BK_BLE_GATTS_TX_INDICATE_WAITING) {
        tracker->kind = H2_BK_BLE_GATTS_TX_INDICATE_ABANDONED;
        return H2_PAL_ERR_TIMEOUT;
    }
    return H2_PAL_ERR_INVALID_STATE;
}

static inline void h2_bk_ble_gatts_tx_tracker_cancel_submission(
    h2_bk_ble_gatts_tx_tracker_t *tracker,
    uint16_t conn_handle,
    uint16_t attr_handle) {
    if (tracker->conn_handle == conn_handle &&
        tracker->attr_handle == attr_handle &&
        (tracker->kind == H2_BK_BLE_GATTS_TX_NOTIFY ||
         tracker->kind == H2_BK_BLE_GATTS_TX_INDICATE_WAITING)) {
        h2_bk_ble_gatts_tx_tracker_clear(tracker);
    }
}

#endif
