#include "h2_bk_ble_exact_adapter.h"

#include <string.h>

static int h2_bk_ble_legacy_put(
    uint8_t *out,
    size_t *out_len,
    size_t capacity,
    uint8_t type,
    const uint8_t *data,
    size_t data_len) {
    if (out == NULL || out_len == NULL || data_len > 254u ||
        (data == NULL && data_len != 0u) || capacity < data_len + 2u ||
        *out_len > capacity - data_len - 2u) {
        return 0;
    }
    out[(*out_len)++] = (uint8_t)(data_len + 1u);
    out[(*out_len)++] = type;
    if (data_len != 0u) {
        memcpy(&out[*out_len], data, data_len);
        *out_len += data_len;
    }
    return 1;
}

h2_pal_result_t h2_bk_ble_exact_submit(
    h2_bk_ble_exact_stack_t stack,
    uint8_t instance,
    const uint8_t *data,
    size_t len,
    size_t capacity,
    h2_bk_ble_exact_submit_fn submit,
    void *user) {
    if (data == NULL && len != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (len > capacity) {
        return H2_PAL_ERR_NO_SPACE;
    }
    if (stack != H2_BK_BLE_EXACT_STACK_ETHERMIND || submit == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return submit(user, instance, data, len);
}

h2_pal_result_t h2_bk_ble_legacy_place_structured_field(
    uint8_t *primary,
    size_t *primary_len,
    size_t primary_capacity,
    uint8_t *scan_response,
    size_t *scan_response_len,
    size_t scan_response_capacity,
    uint8_t type,
    const uint8_t *data,
    size_t data_len) {
    if (h2_bk_ble_legacy_put(
            primary,
            primary_len,
            primary_capacity,
            type,
            data,
            data_len)) {
        return H2_PAL_OK;
    }
    if (h2_bk_ble_legacy_put(
            scan_response,
            scan_response_len,
            scan_response_capacity,
            type,
            data,
            data_len)) {
        return H2_PAL_OK;
    }
    return H2_PAL_ERR_INVALID_ARG;
}

void h2_bk_ble_resolve_scan_units(
    const h2_pal_ble_scan_params_t *params,
    uint16_t converted_interval,
    uint16_t converted_window,
    uint16_t *out_interval,
    uint16_t *out_window) {
    if (params->interval_units_625us != 0u) {
        *out_interval = params->interval_units_625us;
        *out_window = params->window_units_625us;
        return;
    }
    *out_interval = converted_interval;
    *out_window = converted_window;
}
