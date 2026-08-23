#include "h2_smoke_ble_observer.h"

#include <assert.h>
#include <string.h>

typedef enum test_scan_mode {
    TEST_SCAN_COMPLETE,
    TEST_SCAN_FRAGMENTED,
    TEST_SCAN_TRUNCATED,
    TEST_SCAN_MALFORMED,
    TEST_SCAN_TIMEOUT,
    TEST_SCAN_UNSUPPORTED,
} test_scan_mode_t;

typedef struct test_state {
    h2_pal_ble_scan_params_t params;
    unsigned host_start_calls;
    unsigned host_stop_calls;
    unsigned scan_calls;
    unsigned stop_scan_calls;
    unsigned pass_logs;
    unsigned fail_logs;
    test_scan_mode_t mode;
    h2_runtime_event_kind_t event;
} test_state_t;

static test_state_t s_test;

h2_pal_result_t h2_runtime_wait_event(
    h2_runtime_t *runtime,
    h2_runtime_event_t *out_event,
    uint32_t timeout_ms) {
    (void)runtime;
    (void)timeout_ms;
    out_event->kind = s_test.event;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_start(void *user) {
    (void)user;
    ++s_test.host_start_calls;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_stop(void *user) {
    (void)user;
    ++s_test.host_stop_calls;
    return H2_PAL_OK;
}

static void fill_payload(uint8_t payload[66]) {
    payload[0] = 65u;
    payload[1] = 0xffu;
    for (size_t i = 0u; i < 64u; ++i) {
        payload[i + 2u] = (uint8_t)i;
    }
}

static h2_pal_ble_scan_result_t scan_result(
    const uint8_t *data,
    size_t len,
    h2_pal_ble_adv_data_status_t status) {
    h2_pal_ble_scan_result_t result;
    memset(&result, 0, sizeof(result));
    result.addr.type = H2_PAL_BLE_ADDR_TYPE_PUBLIC;
    result.addr.value[0] = 0x32u;
    result.adv_type = H2_PAL_BLE_ADV_TYPE_EXTENDED;
    result.primary_phy = H2_PAL_BLE_PHY_CODED;
    result.secondary_phy = H2_PAL_BLE_PHY_2M;
    result.sid = 7u;
    result.data_status = status;
    result.raw_data.data = data;
    result.raw_data.len = len;
    return result;
}

static h2_pal_result_t fake_start_scan(
    void *user,
    const h2_pal_ble_scan_params_t *params,
    h2_pal_ble_scan_result_fn on_result,
    void *scan_user) {
    (void)user;
    ++s_test.scan_calls;
    s_test.params = *params;
    if (s_test.mode == TEST_SCAN_UNSUPPORTED) {
        return H2_PAL_ERR_UNSUPPORTED;
    }

    uint8_t payload[66];
    fill_payload(payload);
    if (s_test.mode == TEST_SCAN_COMPLETE) {
        h2_pal_ble_scan_result_t result =
            scan_result(payload, sizeof(payload), H2_PAL_BLE_ADV_DATA_COMPLETE);
        assert(!on_result(scan_user, &result));
    } else if (s_test.mode == TEST_SCAN_FRAGMENTED) {
        h2_pal_ble_scan_result_t first =
            scan_result(payload, 24u, H2_PAL_BLE_ADV_DATA_INCOMPLETE);
        assert(!on_result(scan_user, &first));
        h2_pal_ble_scan_result_t second =
            scan_result(payload + 24u, sizeof(payload) - 24u, H2_PAL_BLE_ADV_DATA_COMPLETE);
        assert(!on_result(scan_user, &second));
    } else if (s_test.mode == TEST_SCAN_TRUNCATED) {
        h2_pal_ble_scan_result_t result =
            scan_result(payload, 24u, H2_PAL_BLE_ADV_DATA_TRUNCATED);
        assert(!on_result(scan_user, &result));
    } else if (s_test.mode == TEST_SCAN_MALFORMED) {
        payload[0] = 80u;
        h2_pal_ble_scan_result_t result =
            scan_result(payload, sizeof(payload), H2_PAL_BLE_ADV_DATA_COMPLETE);
        assert(!on_result(scan_user, &result));
    }
    s_test.event = H2_RUNTIME_SYSTEM_EVENT_BLE_SCAN_STOPPED;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_stop_scan(void *user) {
    (void)user;
    ++s_test.stop_scan_calls;
    return H2_PAL_OK;
}

static int fake_log(
    void *user,
    h2_pal_log_level_t level,
    const char *scope,
    const char *message) {
    (void)user;
    assert(strcmp(scope, "ble-observer") == 0);
    if (strstr(message, "state=complete") != NULL) {
        ++s_test.pass_logs;
        assert(level == H2_PAL_LOG_INFO);
    } else if (strstr(message, "state=error") != NULL) {
        ++s_test.fail_logs;
        assert(level == H2_PAL_LOG_ERROR);
    }
    return H2_PAL_OK;
}

static const h2_pal_ble_vtable_t s_ble_vtable = {
    .start = fake_start,
    .stop = fake_stop,
    .start_scan = fake_start_scan,
    .stop_scan = fake_stop_scan,
};
static const h2_pal_ble_host_api_t s_ble = { .vtable = &s_ble_vtable };
static const h2_pal_log_vtable_t s_log_vtable = { .write = fake_log };
static const h2_pal_log_api_t s_log = { .vtable = &s_log_vtable };

static h2_runtime_t test_runtime(void) {
    h2_runtime_t runtime;
    memset(&runtime, 0, sizeof(runtime));
    runtime.ble_host = &s_ble;
    runtime.log = &s_log;
    return runtime;
}

static h2_pal_result_t run_mode(test_scan_mode_t mode) {
    memset(&s_test, 0, sizeof(s_test));
    s_test.mode = mode;
    h2_runtime_t runtime = test_runtime();
    return h2_smoke_ble_observer_run(&runtime);
}

static void assert_common_calls(void) {
    assert(s_test.host_start_calls == 1u);
    assert(s_test.host_stop_calls == 0u);
    assert(s_test.scan_calls == 1u);
    assert(s_test.params.type == H2_PAL_BLE_SCAN_TYPE_EXTENDED);
    assert(s_test.params.phy_mask == H2_PAL_BLE_SCAN_PHY_ALL);
}

int main(void) {
    assert(run_mode(TEST_SCAN_COMPLETE) == H2_PAL_OK);
    assert_common_calls();
    assert(s_test.pass_logs == 1u);
    assert(s_test.stop_scan_calls == 0u);

    assert(run_mode(TEST_SCAN_FRAGMENTED) == H2_PAL_OK);
    assert_common_calls();
    assert(s_test.pass_logs == 1u);

    assert(run_mode(TEST_SCAN_TRUNCATED) == H2_PAL_OK);
    assert_common_calls();
    assert(s_test.pass_logs == 1u);

    assert(run_mode(TEST_SCAN_MALFORMED) == H2_PAL_OK);
    assert_common_calls();
    assert(s_test.pass_logs == 1u);

    assert(run_mode(TEST_SCAN_TIMEOUT) == H2_PAL_OK);
    assert_common_calls();
    assert(s_test.pass_logs == 1u);

    assert(run_mode(TEST_SCAN_UNSUPPORTED) == H2_PAL_ERR_UNSUPPORTED);
    assert_common_calls();
    assert(s_test.stop_scan_calls == 0u);
    assert(s_test.fail_logs == 1u);
    return 0;
}
