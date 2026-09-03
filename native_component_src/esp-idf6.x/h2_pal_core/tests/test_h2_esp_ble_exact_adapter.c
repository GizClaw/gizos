#include "h2_esp_ble_exact_adapter.h"

#include <assert.h>
#include <string.h>

typedef struct submit_capture {
    unsigned calls;
    uint8_t instance;
    uint8_t data[32];
    size_t len;
} submit_capture_t;

static h2_pal_result_t capture_submit(
    void *user,
    uint8_t instance,
    const uint8_t *data,
    size_t len) {
    submit_capture_t *capture = user;
    ++capture->calls;
    capture->instance = instance;
    capture->len = len;
    if (len > 0u) {
        memcpy(capture->data, data, len);
    }
    return H2_PAL_OK;
}

int main(void) {
    const uint8_t repeated[] = {
        3u, 0xffu, 1u, 2u,
        3u, 0xffu, 3u, 4u,
        3u, 0xffu, 5u, 6u,
    };
    submit_capture_t capture = {0};
    assert(h2_esp_ble_exact_submit(
               2u,
               repeated,
               sizeof(repeated),
               sizeof(repeated),
               capture_submit,
               &capture) == H2_PAL_OK);
    assert(capture.calls == 1u);
    assert(capture.instance == 2u);
    assert(capture.len == sizeof(repeated));
    assert(memcmp(capture.data, repeated, sizeof(repeated)) == 0);
    assert(h2_esp_ble_exact_submit(
               2u, NULL, 0u, sizeof(repeated), capture_submit, &capture) ==
           H2_PAL_OK);
    assert(capture.calls == 2u);
    assert(capture.len == 0u);
    assert(h2_esp_ble_exact_submit(
               2u,
               repeated,
               sizeof(repeated),
               sizeof(repeated) - 1u,
               capture_submit,
               &capture) == H2_PAL_ERR_NO_SPACE);
    assert(capture.calls == 2u);

    const uint8_t manufacturer[] = { 0x34u, 0x12u };
    const h2_pal_ble_adv_data_t structured = {
        .local_name = "legacy",
        .manufacturer_data = {
            .data = manufacturer,
            .len = sizeof(manufacturer),
        },
        .service_data = {
            .data = repeated,
            .len = sizeof(repeated),
        },
    };
    h2_pal_ble_adv_data_t primary;
    uint8_t scan_response[H2_PAL_BLE_LEGACY_ADV_DATA_MAX_LEN];
    size_t scan_response_len = 0u;
    assert(h2_esp_ble_prepare_legacy_structured_data(
               &structured,
               &primary,
               scan_response,
               sizeof(scan_response),
               &scan_response_len) == H2_PAL_OK);
    static const uint8_t expected_scan_response[] = {
        3u, 0xffu, 0x34u, 0x12u,
        7u, 0x09u, 'l', 'e', 'g', 'a', 'c', 'y',
    };
    assert(primary.local_name == NULL);
    assert(primary.manufacturer_data.data == NULL);
    assert(primary.manufacturer_data.len == 0u);
    assert(primary.service_data.data == repeated);
    assert(scan_response_len == sizeof(expected_scan_response));
    assert(memcmp(
               scan_response,
               expected_scan_response,
               sizeof(expected_scan_response)) == 0);

    h2_pal_ble_scan_params_t params = {
        .interval_units_625us = 4u,
        .window_units_625us = 4u,
    };
    uint16_t interval = 0u;
    uint16_t window = 0u;
    h2_esp_ble_resolve_scan_units(
        &params, 160u, 80u, &interval, &window);
    assert(interval == 4u);
    assert(window == 4u);
    params.interval_units_625us = 0u;
    params.window_units_625us = 0u;
    h2_esp_ble_resolve_scan_units(
        &params, 160u, 80u, &interval, &window);
    assert(interval == 160u);
    assert(window == 80u);
    return 0;
}
