#include "h2_h2loader_cli_internal.h"

#include <assert.h>

typedef struct fake_state {
    h2_pal_result_t sleep_result;
    h2_pal_result_t stop_scan_result;
    h2_pal_result_t host_stop_result;
    unsigned scan_starts;
    unsigned scan_stops;
    unsigned host_stops;
} fake_state_t;

static h2_pal_result_t fake_sleep(void *user, uint32_t ms) {
    fake_state_t *state = user;
    assert(ms > 0u);
    return state->sleep_result;
}

static h2_pal_result_t fake_start_scan(
    void *user,
    const h2_pal_ble_scan_params_t *params,
    h2_pal_ble_scan_result_fn on_result,
    void *scan_user) {
    fake_state_t *state = user;
    assert(params != NULL);
    assert(on_result != NULL);
    assert(scan_user != NULL);
    ++state->scan_starts;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_stop_scan(void *user) {
    fake_state_t *state = user;
    ++state->scan_stops;
    return state->stop_scan_result;
}

static h2_pal_result_t fake_host_stop(void *user) {
    fake_state_t *state = user;
    ++state->host_stops;
    return state->host_stop_result;
}

static h2_pal_result_t run_find(fake_state_t *state) {
    static const h2_pal_time_vtable_t time_vtable = {.sleep_ms = fake_sleep};
    static const h2_pal_ble_vtable_t ble_vtable = {
        .stop = fake_host_stop,
        .start_scan = fake_start_scan,
        .stop_scan = fake_stop_scan,
    };
    h2_pal_time_api_t time = {.user = state, .vtable = &time_vtable};
    h2_pal_ble_host_api_t ble = {.user = state, .vtable = &ble_vtable};
    h2_runtime_t runtime = {
        .time = &time,
        .ble_host = &ble,
    };
    h2_h2loader_cli_config_t config = {0};
    h2_h2loader_cli_context_t context = {.runtime = &runtime, .config = &config};
    h2_pal_ble_addr_t address;
    int rssi;
    return h2_h2loader_cli_find_ble_peer(&context, 50u, &address, &rssi);
}

static void test_sleep_failure_still_quiesces_scan(void) {
    fake_state_t state = {
        .sleep_result = H2_PAL_ERR_IO,
        .stop_scan_result = H2_PAL_OK,
        .host_stop_result = H2_PAL_OK,
    };
    assert(run_find(&state) == H2_PAL_ERR_IO);
    assert(state.scan_starts == 1u);
    assert(state.scan_stops == 1u);
    assert(state.host_stops == 0u);
}

static void test_stop_failure_uses_host_quiesce_fallback(void) {
    fake_state_t state = {
        .sleep_result = H2_PAL_OK,
        .stop_scan_result = H2_PAL_ERR_IO,
        .host_stop_result = H2_PAL_OK,
    };
    assert(run_find(&state) == H2_PAL_ERR_IO);
    assert(state.scan_starts == 1u);
    assert(state.scan_stops == 3u);
    assert(state.host_stops == 1u);
}

static void test_double_stop_failure_retains_process_owner(void) {
    fake_state_t state = {
        .sleep_result = H2_PAL_ERR_IO,
        .stop_scan_result = H2_PAL_ERR_IO,
        .host_stop_result = H2_PAL_ERR_IO,
    };
    assert(run_find(&state) == H2_PAL_ERR_IO);
    assert(state.scan_starts == 1u);
    assert(state.scan_stops == 3u);
    assert(state.host_stops == 3u);

    fake_state_t retry = {0};
    assert(run_find(&retry) == H2_PAL_ERR_BUSY);
    assert(retry.scan_starts == 0u);
}

int main(void) {
    test_sleep_failure_still_quiesces_scan();
    test_stop_failure_uses_host_quiesce_fallback();
    test_double_stop_failure_retains_process_owner();
    return 0;
}
