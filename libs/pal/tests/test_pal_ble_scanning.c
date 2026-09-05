#include "h2/pal/hal/h2_pal_ble.h"

#include <assert.h>
#include <string.h>

typedef struct scanning_fake {
    unsigned calls;
    h2_pal_ble_scan_params_t params;
} scanning_fake_t;

static bool on_result(void *user, const h2_pal_ble_scan_result_t *result) {
    (void)user;
    (void)result;
    return false;
}

static h2_pal_result_t start_scan(
    void *user,
    const h2_pal_ble_scan_params_t *params,
    h2_pal_ble_scan_result_fn callback,
    void *scan_user) {
    scanning_fake_t *fake = user;
    assert(fake != NULL);
    assert(params != NULL);
    assert(callback == on_result);
    assert(scan_user == fake);
    ++fake->calls;
    fake->params = *params;
    return H2_PAL_OK;
}

int main(void) {
    scanning_fake_t fake;
    memset(&fake, 0, sizeof(fake));
    const h2_pal_ble_vtable_t vtable = { .start_scan = start_scan };
    const h2_pal_ble_host_api_t api = { .user = &fake, .vtable = &vtable };
    h2_pal_ble_scan_params_t params = {
        .mode = H2_PAL_BLE_SCAN_MODE_PASSIVE,
        .interval_ms = 100u,
        .window_ms = 50u,
    };

    assert(h2_pal_ble_start_scan(&api, &params, on_result, &fake) == H2_PAL_OK);
    assert(fake.calls == 1u);
    assert(fake.params.type == H2_PAL_BLE_SCAN_TYPE_LEGACY);
    assert(fake.params.phy_mask == 0u);

    params.type = H2_PAL_BLE_SCAN_TYPE_EXTENDED;
    params.phy_mask = H2_PAL_BLE_SCAN_PHY_ALL;
    params.timeout_ms = 1230u;
    assert(h2_pal_ble_start_scan(&api, &params, on_result, &fake) == H2_PAL_OK);
    assert(fake.calls == 2u);
    assert(fake.params.phy_mask == H2_PAL_BLE_SCAN_PHY_ALL);

    params.interval_ms = 0u;
    params.window_ms = 0u;
    params.interval_units_625us = H2_PAL_BLE_SCAN_UNITS_625US_MIN;
    params.window_units_625us = H2_PAL_BLE_SCAN_UNITS_625US_MIN;
    assert(h2_pal_ble_start_scan(&api, &params, on_result, &fake) == H2_PAL_OK);
    assert(fake.calls == 3u);
    assert(fake.params.interval_units_625us == 4u);
    assert(fake.params.window_units_625us == 4u);

    params.interval_ms = 1u;
    assert(h2_pal_ble_start_scan(&api, &params, on_result, &fake) ==
           H2_PAL_ERR_INVALID_ARG);
    params.interval_ms = 0u;
    params.window_units_625us = 0u;
    assert(h2_pal_ble_start_scan(&api, &params, on_result, &fake) ==
           H2_PAL_ERR_INVALID_ARG);
    params.window_units_625us = 5u;
    assert(h2_pal_ble_start_scan(&api, &params, on_result, &fake) ==
           H2_PAL_ERR_INVALID_ARG);
    params.interval_units_625us = H2_PAL_BLE_SCAN_UNITS_625US_MIN;
    params.window_units_625us = H2_PAL_BLE_SCAN_UNITS_625US_MIN;
    params.type = H2_PAL_BLE_SCAN_TYPE_LEGACY;
    params.interval_units_625us =
        (uint16_t)(H2_PAL_BLE_LEGACY_SCAN_UNITS_625US_MAX + 1u);
    assert(h2_pal_ble_start_scan(&api, &params, on_result, &fake) ==
           H2_PAL_ERR_INVALID_ARG);
    params.type = H2_PAL_BLE_SCAN_TYPE_EXTENDED;
    params.interval_units_625us = H2_PAL_BLE_EXT_SCAN_UNITS_625US_MAX;
    params.window_units_625us = H2_PAL_BLE_EXT_SCAN_UNITS_625US_MAX;
    assert(h2_pal_ble_start_scan(&api, &params, on_result, &fake) == H2_PAL_OK);
    assert(fake.calls == 4u);

    params.interval_units_625us = 0u;
    params.window_units_625us = 0u;
    params.interval_ms = 100u;
    params.window_ms = 50u;

    params.mode = (h2_pal_ble_scan_mode_t)9;
    assert(h2_pal_ble_start_scan(&api, &params, on_result, &fake) == H2_PAL_ERR_INVALID_ARG);
    params.mode = H2_PAL_BLE_SCAN_MODE_ACTIVE;
    params.type = (h2_pal_ble_scan_type_t)9;
    assert(h2_pal_ble_start_scan(&api, &params, on_result, &fake) == H2_PAL_ERR_INVALID_ARG);
    params.type = H2_PAL_BLE_SCAN_TYPE_EXTENDED;
    params.phy_mask = (h2_pal_ble_scan_phy_mask_t)(H2_PAL_BLE_SCAN_PHY_ALL | 0x80u);
    assert(h2_pal_ble_start_scan(&api, &params, on_result, &fake) == H2_PAL_ERR_INVALID_ARG);
    params.phy_mask = H2_PAL_BLE_SCAN_PHY_1M;
    params.interval_ms = 2u;
    assert(h2_pal_ble_start_scan(&api, &params, on_result, &fake) == H2_PAL_ERR_INVALID_ARG);
    params.interval_ms = 100u;
    params.window_ms = 101u;
    assert(h2_pal_ble_start_scan(&api, &params, on_result, &fake) == H2_PAL_ERR_INVALID_ARG);
    params.window_ms = 50u;
    params.timeout_ms = H2_PAL_BLE_SCAN_DURATION_MAX_MS + 1u;
    assert(h2_pal_ble_start_scan(&api, &params, on_result, &fake) == H2_PAL_ERR_INVALID_ARG);
    params.timeout_ms = 0u;
    params.type = H2_PAL_BLE_SCAN_TYPE_LEGACY;
    params.phy_mask = H2_PAL_BLE_SCAN_PHY_CODED;
    assert(h2_pal_ble_start_scan(&api, &params, on_result, &fake) == H2_PAL_ERR_INVALID_ARG);
    params.phy_mask = H2_PAL_BLE_SCAN_PHY_1M;
    params.interval_ms = H2_PAL_BLE_LEGACY_SCAN_INTERVAL_MAX_MS + 1u;
    assert(h2_pal_ble_start_scan(&api, &params, on_result, &fake) == H2_PAL_ERR_INVALID_ARG);
    params.interval_ms = 100u;
    params.timeout_ms = H2_PAL_BLE_SCAN_DURATION_MAX_MS + 1u;
    assert(h2_pal_ble_start_scan(&api, &params, on_result, &fake) == H2_PAL_ERR_INVALID_ARG);

    const h2_pal_ble_host_api_t unsupported = {0};
    params.timeout_ms = 0u;
    assert(h2_pal_ble_start_scan(&unsupported, &params, on_result, &fake) == H2_PAL_ERR_UNSUPPORTED);
    assert(fake.calls == 4u);
    return 0;
}
