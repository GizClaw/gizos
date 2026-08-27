#include "h2_esp_wifi_teardown.h"

#include "h2/pal/core/h2_pal_errors.h"

#include <assert.h>
#include <stddef.h>

enum {
    TEST_SDK_OK = 0,
    TEST_DHCP_ALREADY_STOPPED = 1,
    TEST_WIFI_NOT_INITIALIZED = 2,
    TEST_WIFI_NOT_STARTED = 3,
    TEST_DHCP_FAILURE = 4,
    TEST_WIFI_STOP_FAILURE = 5,
    TEST_WIFI_DEINIT_FAILURE = 6,
};

enum {
    TEST_CALL_DHCP_STOP = 1,
    TEST_CALL_WIFI_STOP = 2,
    TEST_CALL_WIFI_DEINIT = 3,
    TEST_CALL_MAP_ERROR = 4,
};

typedef struct test_state {
    int dhcp_result;
    int wifi_stop_result;
    int wifi_deinit_result;
    int calls[4];
    size_t call_count;
} test_state_t;

static void record_call(test_state_t *state, int call) {
    assert(state->call_count < sizeof(state->calls) / sizeof(state->calls[0]));
    state->calls[state->call_count++] = call;
}

static int test_dhcp_stop(void *user) {
    test_state_t *state = (test_state_t *)user;
    record_call(state, TEST_CALL_DHCP_STOP);
    return state->dhcp_result;
}

static int test_wifi_stop(void *user) {
    test_state_t *state = (test_state_t *)user;
    record_call(state, TEST_CALL_WIFI_STOP);
    return state->wifi_stop_result;
}

static int test_wifi_deinit(void *user) {
    test_state_t *state = (test_state_t *)user;
    record_call(state, TEST_CALL_WIFI_DEINIT);
    return state->wifi_deinit_result;
}

static int test_map_error(void *user, int error) {
    test_state_t *state = (test_state_t *)user;
    record_call(state, TEST_CALL_MAP_ERROR);
    return -error;
}

static h2_esp_wifi_teardown_config_t test_config(test_state_t *state) {
    const h2_esp_wifi_teardown_config_t config = {
        .user = state,
        .dhcp_stop = test_dhcp_stop,
        .wifi_stop = test_wifi_stop,
        .wifi_deinit = test_wifi_deinit,
        .map_error = test_map_error,
        .success = TEST_SDK_OK,
        .dhcp_already_stopped = TEST_DHCP_ALREADY_STOPPED,
        .wifi_not_initialized = TEST_WIFI_NOT_INITIALIZED,
        .wifi_not_started = TEST_WIFI_NOT_STARTED,
    };
    return config;
}

static void assert_complete_order(const test_state_t *state) {
    assert(state->call_count == 3u);
    assert(state->calls[0] == TEST_CALL_DHCP_STOP);
    assert(state->calls[1] == TEST_CALL_WIFI_STOP);
    assert(state->calls[2] == TEST_CALL_WIFI_DEINIT);
}

static void test_successful_teardown_order(void) {
    test_state_t state = {0};
    const h2_esp_wifi_teardown_config_t config = test_config(&state);
    assert(h2_esp_wifi_run_driver_teardown(&config) == H2_PAL_OK);
    assert_complete_order(&state);
}

static void test_already_stopped_dhcp_continues_teardown(void) {
    test_state_t state = {
        .dhcp_result = TEST_DHCP_ALREADY_STOPPED,
    };
    const h2_esp_wifi_teardown_config_t config = test_config(&state);
    assert(h2_esp_wifi_run_driver_teardown(&config) == H2_PAL_OK);
    assert_complete_order(&state);
}

static void test_dhcp_failure_prevents_driver_destruction(void) {
    test_state_t state = {
        .dhcp_result = TEST_DHCP_FAILURE,
    };
    const h2_esp_wifi_teardown_config_t config = test_config(&state);
    assert(h2_esp_wifi_run_driver_teardown(&config) == -TEST_DHCP_FAILURE);
    assert(state.call_count == 2u);
    assert(state.calls[0] == TEST_CALL_DHCP_STOP);
    assert(state.calls[1] == TEST_CALL_MAP_ERROR);
}

static void test_wifi_stop_failure_prevents_deinit(void) {
    test_state_t state = {
        .wifi_stop_result = TEST_WIFI_STOP_FAILURE,
    };
    const h2_esp_wifi_teardown_config_t config = test_config(&state);
    assert(h2_esp_wifi_run_driver_teardown(&config) == -TEST_WIFI_STOP_FAILURE);
    assert(state.call_count == 3u);
    assert(state.calls[0] == TEST_CALL_DHCP_STOP);
    assert(state.calls[1] == TEST_CALL_WIFI_STOP);
    assert(state.calls[2] == TEST_CALL_MAP_ERROR);
}

static void test_idempotent_driver_states_continue(void) {
    test_state_t not_started = {
        .wifi_stop_result = TEST_WIFI_NOT_STARTED,
        .wifi_deinit_result = TEST_WIFI_NOT_INITIALIZED,
    };
    const h2_esp_wifi_teardown_config_t not_started_config = test_config(&not_started);
    assert(h2_esp_wifi_run_driver_teardown(&not_started_config) == H2_PAL_OK);
    assert_complete_order(&not_started);

    test_state_t not_initialized = {
        .wifi_stop_result = TEST_WIFI_NOT_INITIALIZED,
        .wifi_deinit_result = TEST_WIFI_NOT_INITIALIZED,
    };
    const h2_esp_wifi_teardown_config_t not_initialized_config = test_config(&not_initialized);
    assert(h2_esp_wifi_run_driver_teardown(&not_initialized_config) == H2_PAL_OK);
    assert_complete_order(&not_initialized);
}

static void test_wifi_deinit_failure_is_mapped(void) {
    test_state_t state = {
        .wifi_deinit_result = TEST_WIFI_DEINIT_FAILURE,
    };
    const h2_esp_wifi_teardown_config_t config = test_config(&state);
    assert(h2_esp_wifi_run_driver_teardown(&config) == -TEST_WIFI_DEINIT_FAILURE);
    assert(state.call_count == 4u);
    assert(state.calls[3] == TEST_CALL_MAP_ERROR);
}

static void test_invalid_config_is_rejected(void) {
    assert(h2_esp_wifi_run_driver_teardown(NULL) == H2_PAL_ERR_INVALID_ARG);
    test_state_t state = {0};
    h2_esp_wifi_teardown_config_t config = test_config(&state);
    config.dhcp_stop = NULL;
    assert(h2_esp_wifi_run_driver_teardown(&config) == H2_PAL_ERR_INVALID_ARG);
    assert(state.call_count == 0u);
}

int main(void) {
    test_successful_teardown_order();
    test_already_stopped_dhcp_continues_teardown();
    test_dhcp_failure_prevents_driver_destruction();
    test_wifi_stop_failure_prevents_deinit();
    test_idempotent_driver_states_continue();
    test_wifi_deinit_failure_is_mapped();
    test_invalid_config_is_rejected();
    return 0;
}
