#include "h2_runtime.h"

#include <assert.h>
#include <string.h>

typedef struct test_runtime_buzzer_state {
    size_t get_info_calls;
    size_t start_calls;
    size_t stop_calls;
    h2_pal_buzzer_id_t last_id;
    uint32_t last_frequency_hz;
    uint8_t last_volume_percent;
} test_runtime_buzzer_state_t;

static test_runtime_buzzer_state_t buzzer_state;

static h2_pal_result_t test_buzzer_get_info(
    void *user,
    h2_pal_buzzer_id_t id,
    h2_pal_buzzer_info_t *out_info) {
    test_runtime_buzzer_state_t *state = user;
    state->get_info_calls++;
    state->last_id = id;
    *out_info = (h2_pal_buzzer_info_t){
        .id = id,
        .min_frequency_hz = 20u,
        .max_frequency_hz = 20000u,
        .supports_volume = 1u,
    };
    return H2_PAL_OK;
}

static h2_pal_result_t test_buzzer_start(
    void *user,
    h2_pal_buzzer_id_t id,
    uint32_t frequency_hz,
    uint8_t volume_percent) {
    test_runtime_buzzer_state_t *state = user;
    state->start_calls++;
    state->last_id = id;
    state->last_frequency_hz = frequency_hz;
    state->last_volume_percent = volume_percent;
    return H2_PAL_OK;
}

static h2_pal_result_t test_buzzer_stop(
    void *user,
    h2_pal_buzzer_id_t id) {
    test_runtime_buzzer_state_t *state = user;
    state->stop_calls++;
    state->last_id = id;
    return H2_PAL_OK;
}

static const h2_pal_buzzer_vtable_t buzzer_vtable = {
    .get_info = test_buzzer_get_info,
    .start = test_buzzer_start,
    .stop = test_buzzer_stop,
};

static const h2_pal_buzzer_api_t buzzer_api = {
    .user = &buzzer_state,
    .vtable = &buzzer_vtable,
};

const h2_pal_buzzer_api_t *h2_runtime_test_buzzer_api(void) {
    memset(&buzzer_state, 0, sizeof(buzzer_state));
    return &buzzer_api;
}

void h2_runtime_test_buzzer_binding(const h2_runtime_t *runtime) {
    assert(runtime != NULL);
    assert(runtime->buzzer != NULL);
    assert(runtime->buzzer != &buzzer_api);
    assert(runtime->buzzer->user == buzzer_api.user);
    assert(runtime->buzzer->vtable == buzzer_api.vtable);

    h2_pal_buzzer_info_t info;
    assert(h2_pal_buzzer_get_info(runtime->buzzer, 9u, &info) == H2_PAL_OK);
    assert(info.id == 9u);
    assert(info.min_frequency_hz == 20u);
    assert(info.max_frequency_hz == 20000u);
    assert(info.supports_volume == 1u);
    assert(buzzer_state.get_info_calls == 1u);

    assert(h2_pal_buzzer_start(runtime->buzzer, 9u, 440u, 75u) == H2_PAL_OK);
    assert(buzzer_state.start_calls == 1u);
    assert(buzzer_state.last_id == 9u);
    assert(buzzer_state.last_frequency_hz == 440u);
    assert(buzzer_state.last_volume_percent == 75u);

    assert(h2_pal_buzzer_stop(runtime->buzzer, 9u) == H2_PAL_OK);
    assert(buzzer_state.stop_calls == 1u);
    assert(buzzer_state.last_id == 9u);
}
