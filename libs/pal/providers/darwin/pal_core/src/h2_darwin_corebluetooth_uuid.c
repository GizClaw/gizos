#include "h2_darwin_corebluetooth_internal.h"

static int h2_darwin_corebluetooth_uuid_len_valid(size_t len) {
    return len == 2u || len == 4u || len == 16u;
}

bool h2_darwin_corebluetooth_adv_params_supported(
    const h2_pal_ble_adv_params_t *params) {
    return params != NULL && params->type == H2_PAL_BLE_ADV_TYPE_LEGACY;
}

bool h2_darwin_corebluetooth_scan_params_supported(
    const h2_pal_ble_scan_params_t *params) {
    return params != NULL && params->type == H2_PAL_BLE_SCAN_TYPE_LEGACY;
}

int h2_darwin_corebluetooth_uuid_to_platform(
    const h2_pal_ble_uuid_t *uuid,
    uint8_t *out,
    size_t out_size) {
    if (uuid == NULL || uuid->data == NULL || out == NULL ||
        !h2_darwin_corebluetooth_uuid_len_valid(uuid->len) ||
        out_size < uuid->len) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    for (size_t index = 0u; index < uuid->len; ++index) {
        out[index] = uuid->data[uuid->len - index - 1u];
    }
    return H2_PAL_OK;
}

int h2_darwin_corebluetooth_uuid_from_platform(
    const uint8_t *bytes,
    size_t len,
    uint8_t *out,
    size_t out_size) {
    if (bytes == NULL || out == NULL ||
        !h2_darwin_corebluetooth_uuid_len_valid(len) || out_size < len) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    for (size_t index = 0u; index < len; ++index) {
        out[index] = bytes[len - index - 1u];
    }
    return H2_PAL_OK;
}
