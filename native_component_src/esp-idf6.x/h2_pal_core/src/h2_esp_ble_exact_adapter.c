#include "h2_esp_ble_exact_adapter.h"

#include <string.h>

#define H2_ESP_BLE_EXACT_AD_TYPE_NAME_COMPLETE 0x09u
#define H2_ESP_BLE_EXACT_AD_TYPE_MANUFACTURER 0xffu

static int h2_esp_ble_legacy_put(
    uint8_t *out,
    size_t *out_len,
    size_t capacity,
    uint8_t type,
    const uint8_t *data,
    size_t data_len) {
    if (data_len > 254u || (data == NULL && data_len != 0u) ||
        capacity < data_len + 2u || *out_len > capacity - data_len - 2u) {
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

h2_pal_result_t h2_esp_ble_exact_submit(
    uint8_t instance,
    const uint8_t *data,
    size_t len,
    size_t capacity,
    h2_esp_ble_exact_submit_fn submit,
    void *user) {
    if ((data == NULL && len != 0u) || submit == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (len > capacity) {
        return H2_PAL_ERR_NO_SPACE;
    }
    return submit(user, instance, data, len);
}

h2_pal_result_t h2_esp_ble_prepare_legacy_structured_data(
    const h2_pal_ble_adv_data_t *data,
    h2_pal_ble_adv_data_t *out_primary,
    uint8_t *out_scan_response,
    size_t scan_response_capacity,
    size_t *out_scan_response_len) {
    if (data == NULL || out_primary == NULL || out_scan_response == NULL ||
        out_scan_response_len == NULL ||
        (data->manufacturer_data.data == NULL &&
         data->manufacturer_data.len != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    size_t scan_response_len = 0u;
    if (data->manufacturer_data.len != 0u &&
        !h2_esp_ble_legacy_put(
            out_scan_response,
            &scan_response_len,
            scan_response_capacity,
            H2_ESP_BLE_EXACT_AD_TYPE_MANUFACTURER,
            data->manufacturer_data.data,
            data->manufacturer_data.len)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (data->local_name != NULL &&
        !h2_esp_ble_legacy_put(
            out_scan_response,
            &scan_response_len,
            scan_response_capacity,
            H2_ESP_BLE_EXACT_AD_TYPE_NAME_COMPLETE,
            (const uint8_t *)data->local_name,
            strlen(data->local_name))) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_primary = *data;
    out_primary->local_name = NULL;
    out_primary->manufacturer_data = (h2_pal_ble_bytes_t){ 0 };
    *out_scan_response_len = scan_response_len;
    return H2_PAL_OK;
}

void h2_esp_ble_resolve_scan_units(
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
