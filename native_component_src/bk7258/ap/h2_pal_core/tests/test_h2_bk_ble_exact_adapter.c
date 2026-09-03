#include "h2_bk_ble_exact_adapter.h"

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
    submit_capture_t *state = user;
    ++state->calls;
    state->instance = instance;
    state->len = len;
    if (len > 0u) memcpy(state->data, data, len);
    return H2_PAL_OK;
}

int main(void) {
    const uint8_t repeated[] = {
        3u, 0xffu, 1u, 2u,
        3u, 0xffu, 3u, 4u,
        3u, 0xffu, 5u, 6u,
    };
    submit_capture_t state = {0};
#if H2_BK_TEST_ETHERMIND
    const h2_bk_ble_exact_stack_t stack =
        H2_BK_BLE_EXACT_STACK_ETHERMIND;
#else
    const h2_bk_ble_exact_stack_t stack = H2_BK_BLE_EXACT_STACK_LEGACY;
#endif
#if H2_BK_TEST_ETHERMIND
    assert(h2_bk_ble_exact_submit(
               stack,
               3u,
               repeated,
               sizeof(repeated),
               sizeof(repeated),
               capture_submit,
               &state) == H2_PAL_OK);
    assert(state.calls == 1u);
    assert(state.instance == 3u);
    assert(state.len == sizeof(repeated));
    assert(memcmp(state.data, repeated, sizeof(repeated)) == 0);
    assert(h2_bk_ble_exact_submit(
               stack,
               3u,
               NULL,
               0u,
               sizeof(repeated),
               capture_submit,
               &state) == H2_PAL_OK);
    assert(state.calls == 2u && state.len == 0u);
    assert(h2_bk_ble_exact_submit(
               stack,
               3u,
               repeated,
               sizeof(repeated),
               sizeof(repeated) - 1u,
               capture_submit,
               &state) == H2_PAL_ERR_NO_SPACE);
    assert(state.calls == 2u);
#else
    assert(h2_bk_ble_exact_submit(
               stack,
               3u,
               repeated,
               sizeof(repeated),
               sizeof(repeated),
               capture_submit,
               &state) == H2_PAL_ERR_UNSUPPORTED);
    assert(state.calls == 0u);
#endif

    uint8_t primary[H2_PAL_BLE_LEGACY_ADV_DATA_MAX_LEN] = {0};
    uint8_t scan_response[H2_PAL_BLE_LEGACY_ADV_DATA_MAX_LEN] = {0};
    size_t primary_len = sizeof(primary) - 2u;
    size_t scan_response_len = 0u;
    const uint8_t manufacturer[] = { 0x34u, 0x12u };
    assert(h2_bk_ble_legacy_place_structured_field(
               primary,
               &primary_len,
               sizeof(primary),
               scan_response,
               &scan_response_len,
               sizeof(scan_response),
               0xffu,
               manufacturer,
               sizeof(manufacturer)) == H2_PAL_OK);
    static const uint8_t expected_scan_response[] = {
        3u, 0xffu, 0x34u, 0x12u,
    };
    assert(primary_len == sizeof(primary) - 2u);
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
    h2_bk_ble_resolve_scan_units(
        &params, 160u, 80u, &interval, &window);
    assert(interval == 4u);
    assert(window == 4u);
    params.interval_units_625us = 0u;
    params.window_units_625us = 0u;
    h2_bk_ble_resolve_scan_units(
        &params, 160u, 80u, &interval, &window);
    assert(interval == 160u);
    assert(window == 80u);
    return 0;
}
