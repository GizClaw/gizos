#include "h2_h2loader_host.h"

#include <assert.h>
#include <string.h>

typedef struct discovery_fixture {
    unsigned sleep_calls;
    uint32_t slept_ms;
    unsigned stop_calls;
    bool callback_requested_stop;
    bool service_only;
} discovery_fixture_t;

static h2_pal_result_t fake_mutex_create(
    void *user,
    const h2_pal_mutex_config_t *config,
    h2_pal_mutex_t **out_mutex) {
    (void)config;
    *out_mutex = (h2_pal_mutex_t *)user;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_mutex_destroy(
    void *user, h2_pal_mutex_t *mutex) {
    assert(mutex == (h2_pal_mutex_t *)user);
    return H2_PAL_OK;
}

static h2_pal_result_t fake_mutex_lock(
    void *user, h2_pal_mutex_t *mutex) {
    assert(mutex == (h2_pal_mutex_t *)user);
    return H2_PAL_OK;
}

static h2_pal_result_t fake_mutex_unlock(
    void *user, h2_pal_mutex_t *mutex) {
    assert(mutex == (h2_pal_mutex_t *)user);
    return H2_PAL_OK;
}

static h2_pal_result_t fake_sleep(void *user, uint32_t ms) {
    discovery_fixture_t *fixture = user;
    ++fixture->sleep_calls;
    fixture->slept_ms += ms;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_start_scan(
    void *user,
    const h2_pal_ble_scan_params_t *params,
    h2_pal_ble_scan_result_fn on_result,
    void *scan_user) {
    static const uint8_t service_uuid_bytes[16] = {
        0x1du, 0x72u, 0xa1u, 0x6bu, 0x3au, 0xafu, 0x0bu, 0xaau,
        0xe2u, 0x53u, 0xd8u, 0x3eu, 0x70u, 0xb5u, 0xa4u, 0x71u,
    };
    static const uint8_t identity[] = {
        'H', '2', 'L', 'D', 1u, 0u, 7u, 0u, 0u, 0u,
        6u, 'd', 'e', 'v', 'k', 'i', 't',
    };
    static const h2_pal_ble_uuid_t service_uuid = {
        .data = service_uuid_bytes,
        .len = sizeof(service_uuid_bytes),
    };
    discovery_fixture_t *fixture = user;
    const h2_pal_ble_scan_result_t result = {
        .addr = {
            .type = H2_PAL_BLE_ADDR_TYPE_RANDOM_IDENTITY,
            .value = {0x00u, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u},
        },
        .rssi = -40,
        .connectable = true,
        .manufacturer_data = {
            .data = fixture->service_only ? NULL : identity,
            .len = fixture->service_only ? 0u : sizeof(identity),
        },
        .service_uuids = &service_uuid,
        .service_uuid_count = 1u,
        .adv_type = H2_PAL_BLE_ADV_TYPE_LEGACY,
        .primary_phy = H2_PAL_BLE_PHY_1M,
        .data_status = H2_PAL_BLE_ADV_DATA_COMPLETE,
    };
    assert(params->timeout_ms == 1000u);
    fixture->callback_requested_stop = on_result(scan_user, &result);
    return H2_PAL_OK;
}

static h2_pal_result_t fake_stop_scan(void *user) {
    ++((discovery_fixture_t *)user)->stop_calls;
    return H2_PAL_OK;
}

static const h2_pal_sync_vtable_t sync_vtable = {
    .create_mutex = fake_mutex_create,
    .destroy_mutex = fake_mutex_destroy,
    .lock_mutex = fake_mutex_lock,
    .unlock_mutex = fake_mutex_unlock,
};

static const h2_pal_time_vtable_t time_vtable = {
    .sleep_ms = fake_sleep,
};

static const h2_pal_ble_vtable_t ble_vtable = {
    .start_scan = fake_start_scan,
    .stop_scan = fake_stop_scan,
};

static discovery_fixture_t run_scan(
    const char *endpoint,
    bool service_only,
    h2_h2loader_host_candidate_t *candidate) {
    discovery_fixture_t fixture = { .service_only = service_only };
    const h2_pal_sync_api_t sync = {
        .user = &fixture,
        .vtable = &sync_vtable,
    };
    const h2_pal_time_api_t time = {
        .user = &fixture,
        .vtable = &time_vtable,
    };
    const h2_pal_ble_host_api_t ble = {
        .user = &fixture,
        .vtable = &ble_vtable,
    };
    h2_h2loader_host_scan_result_t result;
    const h2_h2loader_host_scan_config_t config = {
        .ble = &ble,
        .sync = &sync,
        .time = &time,
        .ble_timeout_ms = 1000u,
        .ble_endpoint = endpoint,
        .candidates = candidate,
        .candidate_capacity = 1u,
    };

    assert(h2_h2loader_host_scan(&config, &result) == H2_PAL_OK);
    assert(result.count == 1u);
    assert(fixture.stop_calls == 1u);
    return fixture;
}

static void test_target_endpoint_stops_scan_without_waiting(void) {
    h2_h2loader_host_candidate_t candidate;
    discovery_fixture_t fixture = run_scan(
        "4:001122334455", false, &candidate);

    assert(strcmp(candidate.endpoint, "4:001122334455") == 0);
    assert(fixture.callback_requested_stop);
    assert(fixture.sleep_calls == 0u);
    assert(fixture.slept_ms == 0u);
}

static void test_general_scan_keeps_full_window(void) {
    h2_h2loader_host_candidate_t candidate;
    discovery_fixture_t fixture = run_scan(NULL, false, &candidate);

    assert(!fixture.callback_requested_stop);
    assert(fixture.sleep_calls == 1u);
    assert(fixture.slept_ms == 1000u);
}

static void test_unmatched_endpoint_keeps_full_window(void) {
    h2_h2loader_host_candidate_t candidate;
    discovery_fixture_t fixture = run_scan(
        "4:ffeeddccbbaa", false, &candidate);

    assert(!fixture.callback_requested_stop);
    assert(fixture.sleep_calls == 4u);
    assert(fixture.slept_ms == 1000u);
}

static void test_private_service_uuid_is_discoverable_without_scan_response(void) {
    h2_h2loader_host_candidate_t candidate;
    discovery_fixture_t fixture = run_scan(NULL, true, &candidate);

    assert(!fixture.callback_requested_stop);
    assert(candidate.advertised_board[0] == '\0');
    assert(strcmp(candidate.display_name, "h2l.unknown") == 0);
}

int main(void) {
    test_target_endpoint_stops_scan_without_waiting();
    test_general_scan_keeps_full_window();
    test_unmatched_endpoint_keeps_full_window();
    test_private_service_uuid_is_discoverable_without_scan_response();
    return 0;
}
