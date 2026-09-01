#ifndef H2_BK_BLE_EXACT_ADAPTER_H
#define H2_BK_BLE_EXACT_ADAPTER_H

#include "h2/pal/hal/h2_pal_ble.h"

typedef enum h2_bk_ble_exact_stack {
    H2_BK_BLE_EXACT_STACK_ETHERMIND = 0,
    H2_BK_BLE_EXACT_STACK_LEGACY = 1,
} h2_bk_ble_exact_stack_t;

typedef h2_pal_result_t (*h2_bk_ble_exact_submit_fn)(
    void *user,
    uint8_t instance,
    const uint8_t *data,
    size_t len);

h2_pal_result_t h2_bk_ble_exact_submit(
    h2_bk_ble_exact_stack_t stack,
    uint8_t instance,
    const uint8_t *data,
    size_t len,
    size_t capacity,
    h2_bk_ble_exact_submit_fn submit,
    void *user);

h2_pal_result_t h2_bk_ble_legacy_place_structured_field(
    uint8_t *primary,
    size_t *primary_len,
    size_t primary_capacity,
    uint8_t *scan_response,
    size_t *scan_response_len,
    size_t scan_response_capacity,
    uint8_t type,
    const uint8_t *data,
    size_t data_len);

void h2_bk_ble_resolve_scan_units(
    const h2_pal_ble_scan_params_t *params,
    uint16_t converted_interval,
    uint16_t converted_window,
    uint16_t *out_interval,
    uint16_t *out_window);

#endif
