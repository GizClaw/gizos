#include "h2/pal/hal/h2_pal_ble.h"

#include <assert.h>
#include <string.h>

typedef struct advertising_fake {
    unsigned calls;
    h2_pal_ble_adv_params_t params;
    unsigned set_data_calls;
    unsigned set_start_calls;
    unsigned set_stop_calls;
    unsigned set_destroy_calls;
} advertising_fake_t;

static h2_pal_result_t start_advertising(void *user, const h2_pal_ble_adv_params_t *params) {
    advertising_fake_t *fake = (advertising_fake_t *)user;
    assert(fake != NULL);
    assert(params != NULL);
    fake->calls += 1u;
    fake->params = *params;
    return H2_PAL_OK;
}

static h2_pal_result_t adv_set_create(
    void *user,
    const h2_pal_ble_adv_params_t *params,
    h2_pal_ble_adv_set_t **out_set) {
    advertising_fake_t *fake = user;
    assert(fake != NULL);
    assert(params != NULL);
    assert(out_set != NULL);
    fake->params = *params;
    *out_set = (h2_pal_ble_adv_set_t *)fake;
    return H2_PAL_OK;
}

static h2_pal_result_t adv_set_set_data(
    void *user,
    h2_pal_ble_adv_set_t *set,
    const h2_pal_ble_adv_data_t *data) {
    advertising_fake_t *fake = user;
    assert(set == (h2_pal_ble_adv_set_t *)fake);
    assert(data != NULL);
    ++fake->set_data_calls;
    return H2_PAL_OK;
}

static h2_pal_result_t adv_set_start(void *user, h2_pal_ble_adv_set_t *set) {
    advertising_fake_t *fake = user;
    assert(set == (h2_pal_ble_adv_set_t *)fake);
    ++fake->set_start_calls;
    return H2_PAL_OK;
}

static h2_pal_result_t adv_set_stop(void *user, h2_pal_ble_adv_set_t *set) {
    advertising_fake_t *fake = user;
    assert(set == (h2_pal_ble_adv_set_t *)fake);
    ++fake->set_stop_calls;
    return H2_PAL_OK;
}

static h2_pal_result_t adv_set_destroy(void *user, h2_pal_ble_adv_set_t *set) {
    advertising_fake_t *fake = user;
    assert(set == (h2_pal_ble_adv_set_t *)fake);
    ++fake->set_destroy_calls;
    return H2_PAL_OK;
}

int main(void) {
    advertising_fake_t fake;
    memset(&fake, 0, sizeof(fake));
    const h2_pal_ble_vtable_t vtable = {
        .start_advertising = start_advertising,
        .adv_set_create = adv_set_create,
        .adv_set_set_data = adv_set_set_data,
        .adv_set_start = adv_set_start,
        .adv_set_stop = adv_set_stop,
        .adv_set_destroy = adv_set_destroy,
    };
    const h2_pal_ble_host_api_t api = {
        .user = &fake,
        .vtable = &vtable,
    };

    h2_pal_ble_adv_params_t params = {
        .mode = H2_PAL_BLE_ADV_MODE_NON_CONNECTABLE,
        .interval_min_ms = 100u,
        .interval_max_ms = 150u,
    };
    assert(h2_pal_ble_start_advertising(&api, &params) == H2_PAL_OK);
    assert(fake.calls == 1u);
    assert(fake.params.type == H2_PAL_BLE_ADV_TYPE_LEGACY);
    assert(fake.params.duration_ms == 0u);
    assert(fake.params.max_adv_events == 0u);

    params.type = H2_PAL_BLE_ADV_TYPE_EXTENDED;
    params.primary_phy = H2_PAL_BLE_PHY_CODED;
    params.secondary_phy = H2_PAL_BLE_PHY_2M;
    params.sid = 7u;
    params.duration_ms = 250u;
    params.max_adv_events = 5u;
    assert(h2_pal_ble_start_advertising(&api, &params) == H2_PAL_OK);
    assert(fake.calls == 2u);
    assert(fake.params.sid == 7u);

    h2_pal_ble_adv_set_t *set = NULL;
    assert(h2_pal_ble_adv_set_create(&api, &params, &set) == H2_PAL_OK);
    assert(set == (h2_pal_ble_adv_set_t *)&fake);
    h2_pal_ble_adv_data_t data = { .local_name = "multiple-set" };
    assert(h2_pal_ble_adv_set_set_data(&api, set, &data) == H2_PAL_OK);
    assert(h2_pal_ble_adv_set_start(&api, set) == H2_PAL_OK);
    assert(h2_pal_ble_adv_set_stop(&api, set) == H2_PAL_OK);
    assert(h2_pal_ble_adv_set_destroy(&api, set) == H2_PAL_OK);
    assert(fake.set_data_calls == 1u);
    assert(fake.set_start_calls == 1u);
    assert(fake.set_stop_calls == 1u);
    assert(fake.set_destroy_calls == 1u);

    params.type = (h2_pal_ble_adv_type_t)9;
    assert(h2_pal_ble_start_advertising(&api, &params) == H2_PAL_ERR_INVALID_ARG);
    params.type = H2_PAL_BLE_ADV_TYPE_EXTENDED;
    params.mode = (h2_pal_ble_adv_mode_t)9;
    assert(h2_pal_ble_start_advertising(&api, &params) == H2_PAL_ERR_INVALID_ARG);
    params.mode = H2_PAL_BLE_ADV_MODE_NON_CONNECTABLE;
    params.interval_min_ms = 19u;
    assert(h2_pal_ble_start_advertising(&api, &params) == H2_PAL_ERR_INVALID_ARG);
    params.interval_min_ms = 151u;
    assert(h2_pal_ble_start_advertising(&api, &params) == H2_PAL_ERR_INVALID_ARG);
    params.interval_min_ms = 100u;
    params.primary_phy = H2_PAL_BLE_PHY_2M;
    assert(h2_pal_ble_start_advertising(&api, &params) == H2_PAL_ERR_INVALID_ARG);
    params.primary_phy = H2_PAL_BLE_PHY_1M;
    params.secondary_phy = (h2_pal_ble_phy_t)9;
    assert(h2_pal_ble_start_advertising(&api, &params) == H2_PAL_ERR_INVALID_ARG);
    params.secondary_phy = H2_PAL_BLE_PHY_1M;
    params.sid = 16u;
    assert(h2_pal_ble_start_advertising(&api, &params) == H2_PAL_ERR_INVALID_ARG);
    params.sid = 0u;
    params.interval_min_ms = H2_PAL_BLE_EXT_ADV_INTERVAL_MAX_MS;
    params.interval_max_ms = H2_PAL_BLE_EXT_ADV_INTERVAL_MAX_MS;
    params.duration_ms = H2_PAL_BLE_ADV_DURATION_MAX_MS;
    assert(h2_pal_ble_start_advertising(&api, &params) == H2_PAL_OK);
    assert(fake.calls == 3u);
    params.interval_max_ms = H2_PAL_BLE_EXT_ADV_INTERVAL_MAX_MS + 1u;
    assert(h2_pal_ble_start_advertising(&api, &params) == H2_PAL_ERR_INVALID_ARG);
    params.interval_min_ms = 100u;
    params.interval_max_ms = 150u;
    params.duration_ms = H2_PAL_BLE_ADV_DURATION_MAX_MS + 1u;
    assert(h2_pal_ble_start_advertising(&api, &params) == H2_PAL_ERR_INVALID_ARG);
    assert(fake.calls == 3u);

    params.type = H2_PAL_BLE_ADV_TYPE_LEGACY;
    params.duration_ms = 1u;
    assert(h2_pal_ble_start_advertising(&api, &params) == H2_PAL_ERR_INVALID_ARG);
    params.duration_ms = 0u;
    params.max_adv_events = 1u;
    assert(h2_pal_ble_start_advertising(&api, &params) == H2_PAL_ERR_INVALID_ARG);

    const h2_pal_ble_host_api_t unsupported = {0};
    params.max_adv_events = 0u;
    assert(h2_pal_ble_start_advertising(&unsupported, &params) == H2_PAL_ERR_UNSUPPORTED);
    set = (h2_pal_ble_adv_set_t *)&fake;
    assert(h2_pal_ble_adv_set_create(&unsupported, &params, &set) == H2_PAL_ERR_UNSUPPORTED);
    assert(set == NULL);
    assert(h2_pal_ble_adv_set_create(&api, NULL, &set) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_ble_adv_set_create(&api, &params, NULL) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_ble_adv_set_set_data(&api, NULL, &data) == H2_PAL_ERR_INVALID_ARG);
    return 0;
}
