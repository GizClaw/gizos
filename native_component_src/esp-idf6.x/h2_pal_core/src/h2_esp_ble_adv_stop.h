#ifndef H2_ESP_BLE_ADV_STOP_H
#define H2_ESP_BLE_ADV_STOP_H

/**
 * Outcome of asking NimBLE to stop advertising.
 *
 * NimBLE reports BLE_GAP_EVENT_ADV_COMPLETE only when advertising ends on its
 * own or on a connection. An explicit stop that succeeds ends it
 * synchronously and emits nothing, so the caller must publish the stopped
 * state itself; waiting for the event makes every explicit stop burn the full
 * GAP timeout and then fail.
 */
typedef enum h2_esp_ble_adv_stop_outcome {
    /** Advertising is stopped now and no event will follow. */
    H2_ESP_BLE_ADV_STOP_COMPLETED = 0,
    /** The stop request itself failed; the caller maps the SDK result. */
    H2_ESP_BLE_ADV_STOP_FAILED,
} h2_esp_ble_adv_stop_outcome_t;

/**
 * Classify a NimBLE advertising-stop result.
 *
 * @param stop_rc Result of ble_gap_adv_stop() or ble_gap_ext_adv_stop().
 * @param already_rc The SDK's "was not advertising" code, BLE_HS_EALREADY.
 */
static inline h2_esp_ble_adv_stop_outcome_t h2_esp_ble_adv_stop_classify(
    int stop_rc,
    int already_rc) {
    return (stop_rc == 0 || stop_rc == already_rc)
               ? H2_ESP_BLE_ADV_STOP_COMPLETED
               : H2_ESP_BLE_ADV_STOP_FAILED;
}

#endif
