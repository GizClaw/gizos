#ifndef H2_ESP_BLE_EXACT_ADAPTER_H
#define H2_ESP_BLE_EXACT_ADAPTER_H

#include "h2/pal/hal/h2_pal_ble.h"

typedef h2_pal_result_t (*h2_esp_ble_exact_submit_fn)(
    void *user,
    uint8_t instance,
    const uint8_t *data,
    size_t len);

h2_pal_result_t h2_esp_ble_exact_submit(
    uint8_t instance,
    const uint8_t *data,
    size_t len,
    size_t capacity,
    h2_esp_ble_exact_submit_fn submit,
    void *user);

h2_pal_result_t h2_esp_ble_prepare_legacy_structured_data(
    const h2_pal_ble_adv_data_t *data,
    h2_pal_ble_adv_data_t *out_primary,
    uint8_t *out_scan_response,
    size_t scan_response_capacity,
    size_t *out_scan_response_len);

void h2_esp_ble_resolve_scan_units(
    const h2_pal_ble_scan_params_t *params,
    uint16_t converted_interval,
    uint16_t converted_window,
    uint16_t *out_interval,
    uint16_t *out_window);

#endif
