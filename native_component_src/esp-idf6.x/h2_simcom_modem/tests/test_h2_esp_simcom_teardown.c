#include "h2_esp_simcom_teardown.h"

#include <assert.h>
#include <stddef.h>

enum {
    TEST_POWER_OFF = 1,
    TEST_RESTORE_DEFAULT_NETIF = 2,
    TEST_UNREGISTER_EVENT_HANDLERS = 3,
    TEST_DESTROY_DCE = 4,
    TEST_DESTROY_NETIF = 5,
    TEST_DESTROY_EVENT_GROUP = 6,
};

typedef struct test_state {
    int calls[12];
    size_t call_count;
    int fail_call;
} test_state_t;

static h2_pal_result_t record_call(test_state_t *state, int call) {
    assert(state->call_count < sizeof(state->calls) / sizeof(state->calls[0]));
    state->calls[state->call_count++] = call;
    return state->fail_call == call ? H2_PAL_ERR_IO : H2_PAL_OK;
}

static h2_pal_result_t power_off(void *user) {
    return record_call((test_state_t *)user, TEST_POWER_OFF);
}

static h2_pal_result_t restore_default_netif(void *user) {
    return record_call((test_state_t *)user, TEST_RESTORE_DEFAULT_NETIF);
}

static h2_pal_result_t unregister_event_handlers(void *user) {
    return record_call((test_state_t *)user, TEST_UNREGISTER_EVENT_HANDLERS);
}

static h2_pal_result_t destroy_dce(void *user) {
    return record_call((test_state_t *)user, TEST_DESTROY_DCE);
}

static h2_pal_result_t destroy_netif(void *user) {
    return record_call((test_state_t *)user, TEST_DESTROY_NETIF);
}

static h2_pal_result_t destroy_event_group(void *user) {
    return record_call((test_state_t *)user, TEST_DESTROY_EVENT_GROUP);
}

static h2_esp_simcom_teardown_config_t test_config(test_state_t *state) {
    const h2_esp_simcom_teardown_config_t config = {
        .user = state,
        .power_off = power_off,
        .restore_default_netif = restore_default_netif,
        .unregister_event_handlers = unregister_event_handlers,
        .destroy_dce = destroy_dce,
        .destroy_netif = destroy_netif,
        .destroy_event_group = destroy_event_group,
    };
    return config;
}

static void test_power_off_precedes_local_teardown(void) {
    test_state_t state = {0};
    const h2_esp_simcom_teardown_config_t config = test_config(&state);

    assert(h2_esp_simcom_run_teardown(&config) == H2_PAL_OK);
    assert(state.call_count == 6u);
    for (size_t i = 0u; i < state.call_count; ++i) {
        assert(state.calls[i] == (int)i + 1);
    }
}

static void test_power_failure_preserves_all_local_resources(void) {
    test_state_t state = { .fail_call = TEST_POWER_OFF };
    const h2_esp_simcom_teardown_config_t config = test_config(&state);

    assert(h2_esp_simcom_run_teardown(&config) == H2_PAL_ERR_IO);
    assert(state.call_count == 1u);
    assert(state.calls[0] == TEST_POWER_OFF);
}

static void test_repeated_teardown_remains_idempotent(void) {
    test_state_t state = {0};
    const h2_esp_simcom_teardown_config_t config = test_config(&state);

    assert(h2_esp_simcom_run_teardown(&config) == H2_PAL_OK);
    assert(h2_esp_simcom_run_teardown(&config) == H2_PAL_OK);
    assert(state.call_count == 12u);
    for (size_t i = 0u; i < state.call_count; ++i) {
        assert(state.calls[i] == (int)(i % 6u) + 1);
    }
}

static void test_handler_failure_prevents_resource_destruction(void) {
    test_state_t state = { .fail_call = TEST_UNREGISTER_EVENT_HANDLERS };
    const h2_esp_simcom_teardown_config_t config = test_config(&state);

    assert(h2_esp_simcom_run_teardown(&config) == H2_PAL_ERR_IO);
    assert(state.call_count == 3u);
    assert(state.calls[2] == TEST_UNREGISTER_EVENT_HANDLERS);
}

static void test_each_cleanup_boundary_short_circuits(void) {
    for (int fail_call = TEST_RESTORE_DEFAULT_NETIF;
         fail_call <= TEST_DESTROY_EVENT_GROUP;
         ++fail_call) {
        test_state_t state = { .fail_call = fail_call };
        const h2_esp_simcom_teardown_config_t config = test_config(&state);
        assert(h2_esp_simcom_run_teardown(&config) == H2_PAL_ERR_IO);
        assert(state.call_count == (size_t)fail_call);
        assert(state.calls[state.call_count - 1u] == fail_call);
    }
}

static void test_invalid_config_is_rejected(void) {
    assert(h2_esp_simcom_run_teardown(NULL) == H2_PAL_ERR_INVALID_ARG);
    test_state_t state = {0};
    h2_esp_simcom_teardown_config_t config = test_config(&state);
    config.power_off = NULL;
    assert(h2_esp_simcom_run_teardown(&config) == H2_PAL_ERR_INVALID_ARG);
    assert(state.call_count == 0u);
}

int main(void) {
    test_power_off_precedes_local_teardown();
    test_power_failure_preserves_all_local_resources();
    test_repeated_teardown_remains_idempotent();
    test_handler_failure_prevents_resource_destruction();
    test_each_cleanup_boundary_short_circuits();
    test_invalid_config_is_rejected();
    return 0;
}
