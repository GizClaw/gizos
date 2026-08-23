#include "h2_smoke_ble_advertising.h"

#include <assert.h>
#include <string.h>

#define TEST_EVENT_CAPACITY 10u
#define TEST_ADV_CALL_CAPACITY 5u

typedef struct test_state {
    h2_runtime_event_kind_t events[TEST_EVENT_CAPACITY];
    size_t event_read;
    size_t event_write;
    h2_pal_ble_adv_params_t adv_params[TEST_ADV_CALL_CAPACITY];
    size_t manufacturer_len[TEST_ADV_CALL_CAPACITY];
    size_t set_data_calls;
    size_t start_adv_calls;
    size_t stop_adv_calls;
    size_t adv_set_create_calls;
    size_t adv_set_data_calls;
    size_t adv_set_start_calls;
    size_t adv_set_stop_calls;
    size_t adv_set_destroy_calls;
    size_t host_start_calls;
    size_t host_stop_calls;
    size_t pass_logs;
    size_t error_logs;
    int reject_extended;
    size_t reject_start_call;
    size_t reject_adv_set_start_call;
    int adv_set_allocated;
} test_state_t;

static test_state_t s_test;
static uint8_t s_adv_set_handle;

static void push_event(h2_runtime_event_kind_t kind) {
    assert(s_test.event_write < TEST_EVENT_CAPACITY);
    s_test.events[s_test.event_write++] = kind;
}

h2_pal_result_t h2_runtime_wait_event(
    h2_runtime_t *runtime,
    h2_runtime_event_t *out_event,
    uint32_t timeout_ms) {
    (void)runtime;
    (void)timeout_ms;
    if (out_event == NULL || s_test.event_read == s_test.event_write) {
        return H2_PAL_ERR_TIMEOUT;
    }
    out_event->kind = s_test.events[s_test.event_read++];
    out_event->payload_size = 0u;
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

static h2_pal_result_t fake_set_adv_data(void *user, const h2_pal_ble_adv_data_t *data) {
    (void)user;
    assert(data != NULL);
    assert(s_test.set_data_calls < TEST_ADV_CALL_CAPACITY);
    s_test.manufacturer_len[s_test.set_data_calls++] = data->manufacturer_data.len;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_start_advertising(void *user, const h2_pal_ble_adv_params_t *params) {
    (void)user;
    assert(params != NULL);
    assert(s_test.start_adv_calls < TEST_ADV_CALL_CAPACITY);
    s_test.adv_params[s_test.start_adv_calls++] = *params;
    if (s_test.reject_start_call == s_test.start_adv_calls) {
        return H2_PAL_ERR_IO;
    }
    if (s_test.reject_extended && params->type == H2_PAL_BLE_ADV_TYPE_EXTENDED) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    push_event(H2_RUNTIME_SYSTEM_EVENT_BLE_ADVERTISING_STARTED);
    if (params->duration_ms > 0u || params->max_adv_events > 0u) {
        push_event(H2_RUNTIME_SYSTEM_EVENT_BLE_ADVERTISING_STOPPED);
    }
    return H2_PAL_OK;
}

static h2_pal_result_t fake_stop_advertising(void *user) {
    (void)user;
    ++s_test.stop_adv_calls;
    push_event(H2_RUNTIME_SYSTEM_EVENT_BLE_ADVERTISING_STOPPED);
    return H2_PAL_OK;
}

static h2_pal_result_t fake_adv_set_create(
    void *user,
    const h2_pal_ble_adv_params_t *params,
    h2_pal_ble_adv_set_t **out_set) {
    (void)user;
    assert(params != NULL);
    assert(out_set != NULL);
    assert(!s_test.adv_set_allocated);
    ++s_test.adv_set_create_calls;
    s_test.adv_set_allocated = 1;
    *out_set = (h2_pal_ble_adv_set_t *)&s_adv_set_handle;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_adv_set_set_data(
    void *user,
    h2_pal_ble_adv_set_t *set,
    const h2_pal_ble_adv_data_t *data) {
    (void)user;
    assert(set == (h2_pal_ble_adv_set_t *)&s_adv_set_handle);
    assert(data != NULL);
    assert(s_test.adv_set_allocated);
    ++s_test.adv_set_data_calls;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_adv_set_start(
    void *user,
    h2_pal_ble_adv_set_t *set) {
    (void)user;
    assert(set == (h2_pal_ble_adv_set_t *)&s_adv_set_handle);
    assert(s_test.adv_set_allocated);
    ++s_test.adv_set_start_calls;
    return s_test.reject_adv_set_start_call == s_test.adv_set_start_calls
               ? H2_PAL_ERR_IO
               : H2_PAL_OK;
}

static h2_pal_result_t fake_adv_set_stop(
    void *user,
    h2_pal_ble_adv_set_t *set) {
    (void)user;
    assert(set == (h2_pal_ble_adv_set_t *)&s_adv_set_handle);
    assert(s_test.adv_set_allocated);
    ++s_test.adv_set_stop_calls;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_adv_set_destroy(
    void *user,
    h2_pal_ble_adv_set_t *set) {
    (void)user;
    assert(set == (h2_pal_ble_adv_set_t *)&s_adv_set_handle);
    assert(s_test.adv_set_allocated);
    ++s_test.adv_set_destroy_calls;
    s_test.adv_set_allocated = 0;
    return H2_PAL_OK;
}

static int fake_log_write(
    void *user,
    h2_pal_log_level_t level,
    const char *scope,
    const char *message) {
    (void)user;
    assert(strcmp(scope, "ble-broadcaster") == 0);
    if (strstr(message, "state=complete") != NULL) {
        ++s_test.pass_logs;
        assert(level == H2_PAL_LOG_INFO);
    } else if (strstr(message, "state=error") != NULL) {
        ++s_test.error_logs;
        assert(level == H2_PAL_LOG_ERROR);
    }
    return H2_PAL_OK;
}

static const h2_pal_ble_vtable_t s_ble_vtable = {
    .start = fake_start,
    .stop = fake_stop,
    .set_adv_data = fake_set_adv_data,
    .start_advertising = fake_start_advertising,
    .stop_advertising = fake_stop_advertising,
    .adv_set_create = fake_adv_set_create,
    .adv_set_set_data = fake_adv_set_set_data,
    .adv_set_start = fake_adv_set_start,
    .adv_set_stop = fake_adv_set_stop,
    .adv_set_destroy = fake_adv_set_destroy,
};
static const h2_pal_ble_host_api_t s_ble = {
    .user = NULL,
    .vtable = &s_ble_vtable,
};
static const h2_pal_log_vtable_t s_log_vtable = {
    .write = fake_log_write,
};
static const h2_pal_log_api_t s_log = {
    .user = NULL,
    .vtable = &s_log_vtable,
};

static h2_runtime_t test_runtime(void) {
    h2_runtime_t runtime;
    memset(&runtime, 0, sizeof(runtime));
    runtime.ble_host = &s_ble;
    runtime.log = &s_log;
    return runtime;
}

static void test_success_sequence(void) {
    memset(&s_test, 0, sizeof(s_test));
    h2_runtime_t runtime = test_runtime();
    assert(h2_smoke_ble_advertising_run(&runtime) == H2_PAL_OK);
    assert(s_test.host_start_calls == 1u);
    assert(s_test.host_stop_calls == 0u);
    assert(s_test.set_data_calls == 4u);
    assert(s_test.start_adv_calls == 4u);
    assert(s_test.stop_adv_calls == 2u);
    assert(s_test.adv_set_create_calls == 2u);
    assert(s_test.adv_set_data_calls == 3u);
    assert(s_test.adv_set_start_calls == 3u);
    assert(s_test.adv_set_stop_calls == 2u);
    assert(s_test.adv_set_destroy_calls == 1u);
    assert(s_test.adv_set_allocated);
    assert(s_test.pass_logs == 6u);
    assert(s_test.error_logs == 0u);
    assert(s_test.adv_params[0].type == H2_PAL_BLE_ADV_TYPE_LEGACY);
    assert(s_test.adv_params[0].mode == H2_PAL_BLE_ADV_MODE_CONNECTABLE);
    assert(s_test.adv_params[0].duration_ms == 0u);
    assert(s_test.adv_params[0].max_adv_events == 0u);
    assert(s_test.manufacturer_len[0] == 4u);
    assert(s_test.adv_params[1].type == H2_PAL_BLE_ADV_TYPE_EXTENDED);
    assert(s_test.adv_params[1].primary_phy == H2_PAL_BLE_PHY_CODED);
    assert(s_test.adv_params[1].secondary_phy == H2_PAL_BLE_PHY_2M);
    assert(s_test.adv_params[1].sid == 7u);
    assert(s_test.adv_params[1].duration_ms == 0u);
    assert(s_test.manufacturer_len[1] == 64u);
    assert(s_test.adv_params[2].duration_ms == 500u);
    assert(s_test.adv_params[2].max_adv_events == 0u);
    assert(s_test.adv_params[3].duration_ms == 0u);
    assert(s_test.adv_params[3].max_adv_events == 3u);
    assert(s_test.event_read == s_test.event_write);
}

static void test_unsupported_propagates(void) {
    memset(&s_test, 0, sizeof(s_test));
    s_test.reject_extended = 1;
    h2_runtime_t runtime = test_runtime();
    assert(h2_smoke_ble_advertising_run(&runtime) == H2_PAL_ERR_UNSUPPORTED);
    assert(s_test.host_start_calls == 1u);
    assert(s_test.host_stop_calls == 1u);
    assert(s_test.start_adv_calls == 2u);
    assert(s_test.stop_adv_calls == 1u);
    assert(s_test.pass_logs == 1u);
    assert(s_test.error_logs == 2u);
}

static void test_final_start_failure_stops_host(void) {
    memset(&s_test, 0, sizeof(s_test));
    s_test.reject_adv_set_start_call = 3u;
    h2_runtime_t runtime = test_runtime();
    assert(h2_smoke_ble_advertising_run(&runtime) == H2_PAL_ERR_IO);
    assert(s_test.host_start_calls == 1u);
    assert(s_test.host_stop_calls == 1u);
    assert(s_test.start_adv_calls == 4u);
    assert(s_test.stop_adv_calls == 2u);
    assert(s_test.adv_set_create_calls == 2u);
    assert(s_test.adv_set_data_calls == 3u);
    assert(s_test.adv_set_start_calls == 3u);
    assert(s_test.adv_set_stop_calls == 2u);
    assert(s_test.adv_set_destroy_calls == 2u);
    assert(!s_test.adv_set_allocated);
    assert(s_test.pass_logs == 4u);
    assert(s_test.error_logs == 2u);
}

int main(void) {
    test_success_sequence();
    test_unsupported_propagates();
    test_final_start_failure_stops_host();
    return 0;
}
