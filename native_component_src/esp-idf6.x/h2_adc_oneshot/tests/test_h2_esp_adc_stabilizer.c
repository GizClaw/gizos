#include "h2_esp_adc_stabilizer.h"
#include "h2_esp_adc_stabilizer_internal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define RIGHT_BUTTON_ID 104u
#define LEFT_BUTTON_ID 103u
#define STREAM_NO_FAILURE SIZE_MAX

typedef struct numeric_stream_fixture {
    const int32_t *samples;
    size_t sample_count;
    size_t next_sample;
    size_t fail_at;
    size_t wait_count;
    uint32_t last_interval_us;
} numeric_stream_fixture_t;

static bool read_numeric_stream_sample(void *user, int32_t *out_sample) {
    numeric_stream_fixture_t *fixture = (numeric_stream_fixture_t *)user;
    if (fixture->next_sample == fixture->fail_at ||
        fixture->next_sample >= fixture->sample_count) {
        return false;
    }
    *out_sample = fixture->samples[fixture->next_sample++];
    return true;
}

static void wait_numeric_stream_sample(void *user, uint32_t interval_us) {
    numeric_stream_fixture_t *fixture = (numeric_stream_fixture_t *)user;
    ++fixture->wait_count;
    fixture->last_interval_us = interval_us;
}

static h2_esp_adc_value_stabilizer_config_t value_stabilizer_config(void) {
    return (h2_esp_adc_value_stabilizer_config_t){
        .jump_threshold_raw = 200,
        .discard_samples = 5u,
        .sample_interval_us = 5000u,
    };
}

static void configure_value_stabilizer(
    h2_esp_adc_value_stabilizer_t *state) {
    const h2_esp_adc_value_stabilizer_config_t config =
        value_stabilizer_config();
    assert(h2_esp_adc_value_stabilizer_configure_internal(state, &config));
}

static bool read_stabilized_value(
    h2_esp_adc_value_stabilizer_t *state,
    numeric_stream_fixture_t *fixture,
    h2_esp_adc_value_reading_t *out_reading) {
    return h2_esp_adc_value_stabilizer_read_internal(
        state,
        read_numeric_stream_sample,
        wait_numeric_stream_sample,
        fixture,
        out_reading);
}

static uint32_t update_button(
    h2_esp_adc_radio_button_state_t *state,
    h2_esp_adc_radio_button_sample_kind_t kind,
    uint32_t button_id) {
    uint32_t stable_button_id = UINT32_MAX;
    assert(h2_esp_adc_radio_button_update(
        state, kind, button_id, &stable_button_id));
    return stable_button_id;
}

static bool update_binary(
    h2_esp_adc_binary_stabilizer_t *state,
    bool sample) {
    bool stable_value = !sample;
    assert(h2_esp_adc_binary_stabilizer_update(
        state, sample, &stable_value));
    return stable_value;
}

static void test_binary_stabilizer(void) {
    h2_esp_adc_binary_stabilizer_t state;
    h2_esp_adc_binary_stabilizer_init(&state);

    assert(!update_binary(&state, false));
    assert(!update_binary(&state, true));
    assert(!update_binary(&state, false));
    assert(!update_binary(&state, true));
    assert(update_binary(&state, true));
    assert(update_binary(&state, false));
    assert(!update_binary(&state, false));

    h2_esp_adc_binary_stabilizer_init(&state);
    assert(update_binary(&state, true));
    assert(update_binary(&state, false));
    assert(update_binary(&state, true));
    assert(update_binary(&state, false));
    assert(!update_binary(&state, false));
    assert(!update_binary(&state, true));
    assert(update_binary(&state, true));
}

static void test_radio_button_debounce(void) {
    h2_esp_adc_radio_button_state_t state;
    h2_esp_adc_radio_button_init(&state);

    assert(update_button(
               &state, H2_ESP_ADC_RADIO_BUTTON_SAMPLE_PRESSED, RIGHT_BUTTON_ID) == 0u);
    assert(update_button(
               &state, H2_ESP_ADC_RADIO_BUTTON_SAMPLE_RELEASED, 0u) == 0u);

    assert(update_button(
               &state, H2_ESP_ADC_RADIO_BUTTON_SAMPLE_PRESSED, RIGHT_BUTTON_ID) == 0u);
    assert(update_button(
               &state, H2_ESP_ADC_RADIO_BUTTON_SAMPLE_PRESSED, RIGHT_BUTTON_ID) ==
           RIGHT_BUTTON_ID);

    assert(update_button(
               &state, H2_ESP_ADC_RADIO_BUTTON_SAMPLE_PRESSED, LEFT_BUTTON_ID) ==
           RIGHT_BUTTON_ID);
    assert(update_button(
               &state, H2_ESP_ADC_RADIO_BUTTON_SAMPLE_UNKNOWN, 0u) == RIGHT_BUTTON_ID);
    assert(update_button(
               &state, H2_ESP_ADC_RADIO_BUTTON_SAMPLE_RELEASED, 0u) == RIGHT_BUTTON_ID);
    assert(update_button(
               &state, H2_ESP_ADC_RADIO_BUTTON_SAMPLE_PRESSED, LEFT_BUTTON_ID) ==
           RIGHT_BUTTON_ID);

    assert(update_button(
               &state, H2_ESP_ADC_RADIO_BUTTON_SAMPLE_RELEASED, 0u) == RIGHT_BUTTON_ID);
    assert(update_button(
               &state, H2_ESP_ADC_RADIO_BUTTON_SAMPLE_RELEASED, 0u) == 0u);
    assert(update_button(
               &state, H2_ESP_ADC_RADIO_BUTTON_SAMPLE_PRESSED, LEFT_BUTTON_ID) == 0u);
    assert(update_button(
               &state, H2_ESP_ADC_RADIO_BUTTON_SAMPLE_PRESSED, LEFT_BUTTON_ID) ==
           LEFT_BUTTON_ID);
}

static void test_overlapping_buttons_require_full_release(void) {
    h2_esp_adc_radio_button_state_t state;
    h2_esp_adc_radio_button_init(&state);
    (void)update_button(
        &state, H2_ESP_ADC_RADIO_BUTTON_SAMPLE_PRESSED, RIGHT_BUTTON_ID);
    assert(update_button(
               &state, H2_ESP_ADC_RADIO_BUTTON_SAMPLE_PRESSED, RIGHT_BUTTON_ID) ==
           RIGHT_BUTTON_ID);

    for (size_t i = 0u; i < 4u; ++i) {
        assert(update_button(
                   &state, H2_ESP_ADC_RADIO_BUTTON_SAMPLE_PRESSED, LEFT_BUTTON_ID) ==
               RIGHT_BUTTON_ID);
    }
    assert(update_button(
               &state, H2_ESP_ADC_RADIO_BUTTON_SAMPLE_RELEASED, 0u) == RIGHT_BUTTON_ID);
    assert(update_button(
               &state, H2_ESP_ADC_RADIO_BUTTON_SAMPLE_PRESSED, LEFT_BUTTON_ID) ==
           RIGHT_BUTTON_ID);
}

static void test_numeric_outlier_rejection_and_slow_ema(void) {
    h2_esp_adc_numeric_stabilizer_t state;
    h2_esp_adc_numeric_stabilizer_init_internal(&state);
    int32_t stable = 0;
    for (size_t i = 0u; i < 5u; ++i) {
        assert(h2_esp_adc_numeric_stabilizer_update_internal(
            &state, 3800, &stable));
        assert(stable == 3800);
    }
    assert(h2_esp_adc_numeric_stabilizer_update_internal(
        &state, 4300, &stable));
    assert(stable == 3800);

    for (size_t i = 0u; i < 5u; ++i) {
        assert(h2_esp_adc_numeric_stabilizer_update_internal(
            &state, 3860, &stable));
    }
    assert(stable > 3800);
    assert(stable < 3820);
}

static void test_numeric_negative_startup_value(void) {
    h2_esp_adc_numeric_stabilizer_t state;
    h2_esp_adc_numeric_stabilizer_init_internal(&state);
    int32_t stable = 0;
    assert(h2_esp_adc_numeric_stabilizer_update_internal(
        &state, -100, &stable));
    assert(stable == -100);
}

static void test_numeric_initial_window_precedes_ema(void) {
    h2_esp_adc_numeric_stabilizer_t state;
    h2_esp_adc_numeric_stabilizer_init_internal(&state);
    const int32_t samples[] = {0, 100, 200, 300, 400};
    const int32_t expected[] = {0, 100, 100, 200, 200};
    int32_t stable = INT32_MIN;
    for (size_t i = 0u; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        assert(h2_esp_adc_numeric_stabilizer_update_internal(
            &state, samples[i], &stable));
        assert(stable == expected[i]);
    }

    assert(h2_esp_adc_numeric_stabilizer_update_internal(
        &state, 1000, &stable));
    assert(stable == 202);
}

static void test_numeric_stream_startup_steady_and_jump(void) {
    h2_esp_adc_value_stabilizer_t state;
    configure_value_stabilizer(&state);

    const int32_t startup_samples[] = {
        1000, 1001, 1002, 1003, 1004,
        1090, 1050, 1100, 1070, 1080,
    };
    numeric_stream_fixture_t fixture = {
        .samples = startup_samples,
        .sample_count = sizeof(startup_samples) / sizeof(startup_samples[0]),
        .fail_at = STREAM_NO_FAILURE,
    };
    h2_esp_adc_value_reading_t result;
    memset(&result, 0xa5, sizeof(result));
    assert(read_stabilized_value(&state, &fixture, &result));
    assert(result.reason == H2_ESP_ADC_VALUE_READ_STARTUP);
    assert(result.immediate_raw == 1080);
    assert(result.stable_raw == 1080);
    assert(fixture.next_sample == 10u);
    assert(fixture.wait_count == 9u);
    assert(fixture.last_interval_us == 5000u);
    assert(state.stable_raw == 1080);

    const int32_t steady_samples[] = {1279};
    fixture = (numeric_stream_fixture_t){
        .samples = steady_samples,
        .sample_count = 1u,
        .fail_at = STREAM_NO_FAILURE,
    };
    assert(read_stabilized_value(&state, &fixture, &result));
    assert(result.reason == H2_ESP_ADC_VALUE_READ_STEADY);
    assert(result.immediate_raw == 1279);
    assert(result.stable_raw == 1080);
    assert(fixture.next_sample == 1u);
    assert(fixture.wait_count == 0u);
    assert(state.stable_raw == 1080);

    const int32_t jump_samples[] = {
        1479, 1480, 1481, 1482, 1483,
        1500, 1490, 1510, 1480, 1520,
    };
    fixture = (numeric_stream_fixture_t){
        .samples = jump_samples,
        .sample_count = sizeof(jump_samples) / sizeof(jump_samples[0]),
        .fail_at = STREAM_NO_FAILURE,
    };
    assert(read_stabilized_value(&state, &fixture, &result));
    assert(result.reason == H2_ESP_ADC_VALUE_READ_JUMP);
    assert(result.immediate_raw == 1520);
    assert(result.stable_raw == 1500);
    assert(state.stable_raw == 1500);

    const int32_t reverse_jump_samples[] = {
        1200, 1201, 1202, 1203, 1204,
        1190, 1210, 1180, 1220, 1200,
    };
    fixture = (numeric_stream_fixture_t){
        .samples = reverse_jump_samples,
        .sample_count =
            sizeof(reverse_jump_samples) / sizeof(reverse_jump_samples[0]),
        .fail_at = STREAM_NO_FAILURE,
    };
    assert(read_stabilized_value(&state, &fixture, &result));
    assert(result.reason == H2_ESP_ADC_VALUE_READ_JUMP);
    assert(result.stable_raw == 1200);
    assert(state.stable_raw == 1200);
}

static void test_numeric_stream_cumulative_raw_jump(void) {
    h2_esp_adc_value_stabilizer_t state;
    configure_value_stabilizer(&state);

    const int32_t startup_samples[] = {
        1000, 1001, 1002, 1003, 1004,
        1005, 1006, 1007, 1008, 1009,
    };
    numeric_stream_fixture_t fixture = {
        .samples = startup_samples,
        .sample_count = sizeof(startup_samples) / sizeof(startup_samples[0]),
        .fail_at = STREAM_NO_FAILURE,
    };
    h2_esp_adc_value_reading_t reading;
    assert(read_stabilized_value(&state, &fixture, &reading));
    assert(reading.reason == H2_ESP_ADC_VALUE_READ_STARTUP);
    assert(reading.stable_raw == 1007);

    const int32_t below_threshold_samples[] = {1100, 1190};
    for (size_t i = 0u;
         i < sizeof(below_threshold_samples) /
                 sizeof(below_threshold_samples[0]);
         ++i) {
        fixture = (numeric_stream_fixture_t){
            .samples = &below_threshold_samples[i],
            .sample_count = 1u,
            .fail_at = STREAM_NO_FAILURE,
        };
        assert(read_stabilized_value(&state, &fixture, &reading));
        assert(reading.reason == H2_ESP_ADC_VALUE_READ_STEADY);
        assert(reading.stable_raw == 1007);
    }

    const int32_t exact_threshold_samples[] = {
        1207, 1208, 1209, 1210, 1211,
        1212, 1213, 1214, 1215, 1216,
    };
    fixture = (numeric_stream_fixture_t){
        .samples = exact_threshold_samples,
        .sample_count =
            sizeof(exact_threshold_samples) /
            sizeof(exact_threshold_samples[0]),
        .fail_at = STREAM_NO_FAILURE,
    };
    assert(read_stabilized_value(&state, &fixture, &reading));
    assert(reading.reason == H2_ESP_ADC_VALUE_READ_JUMP);
    assert(reading.immediate_raw == 1216);
    assert(reading.stable_raw == 1214);
    assert(state.stable_raw == 1214);
}

static void test_numeric_stream_widened_reverse_delta(void) {
    const h2_esp_adc_value_stabilizer_config_t config = {
        .jump_threshold_raw = INT32_MAX,
        .discard_samples = 0u,
    };
    h2_esp_adc_value_stabilizer_t state;
    assert(h2_esp_adc_value_stabilizer_configure_internal(&state, &config));

    const int32_t startup_samples[] = {
        INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX,
    };
    numeric_stream_fixture_t fixture = {
        .samples = startup_samples,
        .sample_count = sizeof(startup_samples) / sizeof(startup_samples[0]),
        .fail_at = STREAM_NO_FAILURE,
    };
    h2_esp_adc_value_reading_t reading;
    assert(read_stabilized_value(&state, &fixture, &reading));
    assert(reading.stable_raw == INT32_MAX);

    const int32_t jump_samples[] = {
        INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN,
    };
    fixture = (numeric_stream_fixture_t){
        .samples = jump_samples,
        .sample_count = sizeof(jump_samples) / sizeof(jump_samples[0]),
        .fail_at = STREAM_NO_FAILURE,
    };
    assert(read_stabilized_value(&state, &fixture, &reading));
    assert(reading.reason == H2_ESP_ADC_VALUE_READ_JUMP);
    assert(reading.stable_raw == INT32_MIN);
}

static void test_numeric_stream_failure_is_atomic(void) {
    const int32_t baseline_samples[] = {
        1000, 1001, 1002, 1003, 1004,
        1005, 1006, 1007, 1008, 1009,
    };
    h2_esp_adc_value_stabilizer_t baseline_state;
    configure_value_stabilizer(&baseline_state);
    numeric_stream_fixture_t fixture = {
        .samples = baseline_samples,
        .sample_count = sizeof(baseline_samples) / sizeof(baseline_samples[0]),
        .fail_at = STREAM_NO_FAILURE,
    };
    h2_esp_adc_value_reading_t baseline_result;
    assert(read_stabilized_value(
        &baseline_state, &fixture, &baseline_result));

    const int32_t jump_samples[] = {
        1300, 1301, 1302, 1303, 1304,
        1305, 1306, 1307, 1308, 1309,
    };
    for (size_t fail_at = 0u; fail_at < 10u; ++fail_at) {
        h2_esp_adc_value_stabilizer_t state = baseline_state;
        h2_esp_adc_value_reading_t result;
        memset(&result, 0x5a, sizeof(result));
        const h2_esp_adc_value_reading_t expected_result = result;
        fixture = (numeric_stream_fixture_t){
            .samples = jump_samples,
            .sample_count = sizeof(jump_samples) / sizeof(jump_samples[0]),
            .fail_at = fail_at,
        };
        assert(!read_stabilized_value(&state, &fixture, &result));
        assert(memcmp(&state, &baseline_state, sizeof(state)) == 0);
        assert(memcmp(&result, &expected_result, sizeof(result)) == 0);
    }
}

static void test_numeric_stream_invalid_config_is_atomic(void) {
    h2_esp_adc_value_stabilizer_reset_internal(NULL);
    h2_esp_adc_value_stabilizer_t state = {0};
    const h2_esp_adc_value_stabilizer_t expected_state = state;
    const int32_t samples[] = {1000};
    numeric_stream_fixture_t fixture = {
        .samples = samples,
        .sample_count = 1u,
        .fail_at = STREAM_NO_FAILURE,
    };
    h2_esp_adc_value_stabilizer_config_t config =
        value_stabilizer_config();
    h2_esp_adc_value_reading_t result;
    memset(&result, 0x5a, sizeof(result));
    const h2_esp_adc_value_reading_t expected_result = result;

    config.jump_threshold_raw = -1;
    assert(!h2_esp_adc_value_stabilizer_configure_internal(&state, &config));
    assert(memcmp(&state, &expected_state, sizeof(state)) == 0);
    assert(memcmp(&result, &expected_result, sizeof(result)) == 0);

    config = value_stabilizer_config();
    config.jump_threshold_raw = 0;
    assert(!h2_esp_adc_value_stabilizer_configure_internal(&state, &config));
    assert(memcmp(&state, &expected_state, sizeof(state)) == 0);

    configure_value_stabilizer(&state);
    const h2_esp_adc_value_stabilizer_t configured_state = state;
    assert(!h2_esp_adc_value_stabilizer_read_internal(
        &state, read_numeric_stream_sample, NULL, &fixture, &result));
    assert(memcmp(&state, &configured_state, sizeof(state)) == 0);
    assert(memcmp(&result, &expected_result, sizeof(result)) == 0);

    assert(!h2_esp_adc_value_stabilizer_read_internal(
        &state, NULL, wait_numeric_stream_sample, &fixture, &result));
    assert(memcmp(&state, &configured_state, sizeof(state)) == 0);
    assert(memcmp(&result, &expected_result, sizeof(result)) == 0);
}

static void test_value_stabilizer_zero_threshold_updates_automatically(void) {
    const h2_esp_adc_value_stabilizer_config_t config = {
        .jump_threshold_raw = 0,
    };
    h2_esp_adc_value_stabilizer_t state;
    assert(h2_esp_adc_value_stabilizer_configure_internal(&state, &config));

    const int32_t samples[] = {100, 200, 300, 400, 500, 1000};
    const int32_t expected[] = {100, 200, 200, 300, 300, 302};
    for (size_t i = 0u; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        numeric_stream_fixture_t fixture = {
            .samples = &samples[i],
            .sample_count = 1u,
            .fail_at = STREAM_NO_FAILURE,
        };
        h2_esp_adc_value_reading_t reading;
        assert(read_stabilized_value(&state, &fixture, &reading));
        assert(reading.reason == (i == 0u
                                      ? H2_ESP_ADC_VALUE_READ_STARTUP
                                      : H2_ESP_ADC_VALUE_READ_STEADY));
        assert(reading.immediate_raw == samples[i]);
        assert(reading.stable_raw == expected[i]);
        assert(fixture.next_sample == 1u);
        assert(fixture.wait_count == 0u);
    }
}

static void test_value_stabilizer_raw_config_and_reset(void) {
    const h2_esp_adc_value_stabilizer_config_t config = {
        .jump_threshold_raw = 200,
        .discard_samples = 0u,
        .sample_interval_us = 0u,
    };
    h2_esp_adc_value_stabilizer_t state;
    assert(h2_esp_adc_value_stabilizer_configure_internal(&state, &config));

    const int32_t raw_samples[] = {1, 2, 3, 4, 5};
    numeric_stream_fixture_t fixture = {
        .samples = raw_samples,
        .sample_count = sizeof(raw_samples) / sizeof(raw_samples[0]),
        .fail_at = STREAM_NO_FAILURE,
    };
    h2_esp_adc_value_reading_t reading;
    assert(h2_esp_adc_value_stabilizer_read_internal(
        &state,
        read_numeric_stream_sample,
        NULL,
        &fixture,
        &reading));
    assert(reading.reason == H2_ESP_ADC_VALUE_READ_STARTUP);
    assert(reading.stable_raw == 3);
    assert(reading.immediate_raw == 5);

    assert(h2_esp_adc_value_stabilizer_configure_internal(&state, &config));
    assert(state.configured);
    assert(!state.initialized);
    h2_esp_adc_value_stabilizer_reset_internal(&state);
    assert(!state.configured);
    assert(!state.initialized);
}

static void test_percent_hysteresis_and_direction(void) {
    h2_esp_adc_percent_stabilizer_t state;
    h2_esp_adc_percent_stabilizer_init(&state);
    uint16_t published = 0u;

    assert(h2_esp_adc_percent_stabilizer_update(&state, 5050u, false, &published));
    assert(published == 5050u);
    assert(h2_esp_adc_percent_stabilizer_update(&state, 4999u, false, &published));
    assert(published == 5050u);
    assert(h2_esp_adc_percent_stabilizer_update(&state, 4950u, false, &published));
    assert(published == 4950u);
    assert(h2_esp_adc_percent_stabilizer_update(&state, 5100u, false, &published));
    assert(published == 4950u);

    assert(h2_esp_adc_percent_stabilizer_update(&state, 5100u, true, &published));
    assert(published == 5100u);
    assert(h2_esp_adc_percent_stabilizer_update(&state, 5050u, true, &published));
    assert(published == 5100u);
    assert(h2_esp_adc_percent_stabilizer_update(&state, 5200u, true, &published));
    assert(published == 5200u);
}

static void test_invalid_samples_do_not_change_state(void) {
    h2_esp_adc_binary_stabilizer_init(NULL);
    h2_esp_adc_binary_stabilizer_t binary_state;
    h2_esp_adc_binary_stabilizer_init(&binary_state);
    bool stable_binary = false;
    assert(h2_esp_adc_binary_stabilizer_update(
        &binary_state, true, &stable_binary));
    const h2_esp_adc_binary_stabilizer_t expected_binary_state = binary_state;
    stable_binary = false;
    assert(!h2_esp_adc_binary_stabilizer_update(
        NULL, false, &stable_binary));
    assert(!stable_binary);
    assert(!h2_esp_adc_binary_stabilizer_update(
        &binary_state, false, NULL));
    assert(memcmp(
               &binary_state,
               &expected_binary_state,
               sizeof(binary_state)) == 0);

    h2_esp_adc_radio_button_state_t button_state;
    h2_esp_adc_radio_button_init(&button_state);
    uint32_t stable_button = 0u;
    assert(!h2_esp_adc_radio_button_update(
        &button_state,
        H2_ESP_ADC_RADIO_BUTTON_SAMPLE_PRESSED,
        0u,
        &stable_button));
    assert(!h2_esp_adc_radio_button_update(
        &button_state,
        (h2_esp_adc_radio_button_sample_kind_t)-1,
        0u,
        &stable_button));
    h2_esp_adc_radio_button_state_t expected_button_state;
    memset(&expected_button_state, 0, sizeof(expected_button_state));
    assert(memcmp(
               &button_state,
               &expected_button_state,
               sizeof(button_state)) == 0);

    h2_esp_adc_numeric_stabilizer_t numeric_state;
    h2_esp_adc_numeric_stabilizer_init_internal(&numeric_state);
    int32_t stable_numeric = 0;
    assert(h2_esp_adc_numeric_stabilizer_update_internal(
        &numeric_state, 3800, &stable_numeric));
    const h2_esp_adc_numeric_stabilizer_t expected_numeric_state = numeric_state;
    assert(!h2_esp_adc_numeric_stabilizer_update_internal(
        &numeric_state, 0, NULL));
    assert(memcmp(
               &numeric_state,
               &expected_numeric_state,
               sizeof(numeric_state)) == 0);

    h2_esp_adc_percent_stabilizer_t percent_state;
    h2_esp_adc_percent_stabilizer_init(&percent_state);
    uint16_t stable_percent = 0u;
    assert(h2_esp_adc_percent_stabilizer_update(
        &percent_state, 5000u, false, &stable_percent));
    const h2_esp_adc_percent_stabilizer_t expected_percent_state = percent_state;
    assert(!h2_esp_adc_percent_stabilizer_update(
        &percent_state, 10001u, false, &stable_percent));
    assert(memcmp(
               &percent_state,
               &expected_percent_state,
               sizeof(percent_state)) == 0);
}

int main(void) {
    test_binary_stabilizer();
    test_radio_button_debounce();
    test_overlapping_buttons_require_full_release();
    test_numeric_outlier_rejection_and_slow_ema();
    test_numeric_negative_startup_value();
    test_numeric_initial_window_precedes_ema();
    test_numeric_stream_startup_steady_and_jump();
    test_numeric_stream_cumulative_raw_jump();
    test_numeric_stream_widened_reverse_delta();
    test_numeric_stream_failure_is_atomic();
    test_numeric_stream_invalid_config_is_atomic();
    test_value_stabilizer_zero_threshold_updates_automatically();
    test_value_stabilizer_raw_config_and_reset();
    test_percent_hysteresis_and_direction();
    test_invalid_samples_do_not_change_state();
    puts("h2_esp_adc_stabilizer tests passed");
    return 0;
}
