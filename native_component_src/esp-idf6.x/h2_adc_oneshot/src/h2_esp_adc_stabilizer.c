#include "h2_esp_adc_stabilizer.h"
#include "h2_esp_adc_stabilizer_internal.h"

#include <string.h>

static int32_t median(const int32_t *samples, size_t count) {
    int32_t sorted[H2_ESP_ADC_NUMERIC_MEDIAN_WINDOW];
    memcpy(sorted, samples, count * sizeof(sorted[0]));
    for (size_t i = 1u; i < count; ++i) {
        const int32_t value = sorted[i];
        size_t position = i;
        while (position > 0u && sorted[position - 1u] > value) {
            sorted[position] = sorted[position - 1u];
            --position;
        }
        sorted[position] = value;
    }
    return sorted[count / 2u];
}

static int32_t q16_to_i32(int64_t value) {
    if (value >= 0) {
        return (int32_t)((value + INT64_C(32768)) >> 16);
    }
    return (int32_t)(-(((-value) + INT64_C(32768)) >> 16));
}

void h2_esp_adc_binary_stabilizer_init(
    h2_esp_adc_binary_stabilizer_t *state) {
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

bool h2_esp_adc_binary_stabilizer_update(
    h2_esp_adc_binary_stabilizer_t *state,
    bool sample,
    bool *out_stable_value) {
    if (state == NULL || out_stable_value == NULL) {
        return false;
    }

    if (!state->initialized) {
        state->stable_value = sample;
        state->candidate_value = sample;
        state->candidate_count = 0u;
        state->initialized = true;
    } else if (sample == state->stable_value) {
        state->candidate_value = sample;
        state->candidate_count = 0u;
    } else {
        if (sample != state->candidate_value) {
            state->candidate_value = sample;
            state->candidate_count = 0u;
        }
        if (++state->candidate_count >= H2_ESP_ADC_BINARY_CONFIRM_SAMPLES) {
            state->stable_value = sample;
            state->candidate_count = 0u;
        }
    }

    *out_stable_value = state->stable_value;
    return true;
}

void h2_esp_adc_numeric_stabilizer_init_internal(
    h2_esp_adc_numeric_stabilizer_t *state) {
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

bool h2_esp_adc_numeric_stabilizer_update_internal(
    h2_esp_adc_numeric_stabilizer_t *state,
    int32_t sample,
    int32_t *out_stable_value) {
    if (state == NULL || out_stable_value == NULL) {
        return false;
    }

    const bool collecting_initial_window =
        state->sample_count < H2_ESP_ADC_NUMERIC_MEDIAN_WINDOW;
    state->samples[state->next_sample] = sample;
    state->next_sample =
        (state->next_sample + 1u) % H2_ESP_ADC_NUMERIC_MEDIAN_WINDOW;
    if (state->sample_count < H2_ESP_ADC_NUMERIC_MEDIAN_WINDOW) {
        ++state->sample_count;
    }
    const int32_t median_value = median(state->samples, state->sample_count);
    const int64_t median_q16 = (int64_t)median_value * INT64_C(65536);
    if (!state->initialized || collecting_initial_window) {
        state->ema_q16 = median_q16;
        state->initialized = true;
    } else {
        state->ema_q16 +=
            (median_q16 - state->ema_q16) / H2_ESP_ADC_NUMERIC_EMA_DIVISOR;
    }
    *out_stable_value = q16_to_i32(state->ema_q16);
    return true;
}

bool h2_esp_adc_value_stabilizer_init_internal(
    h2_esp_adc_value_stabilizer_t *state) {
    if (state == NULL) {
        return false;
    }

    h2_esp_adc_value_stabilizer_t next_state = {0};
    next_state.configured = true;
    *state = next_state;
    return true;
}

void h2_esp_adc_value_stabilizer_reset_internal(
    h2_esp_adc_value_stabilizer_t *state) {
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

static bool read_raw_sample(
    h2_esp_adc_value_sample_fn sample,
    void *user,
    int32_t *out_raw) {
    return sample(user, out_raw);
}

bool h2_esp_adc_value_stabilizer_read_internal(
    h2_esp_adc_value_stabilizer_t *state,
    h2_esp_adc_value_sample_fn sample,
    h2_esp_adc_value_wait_fn wait,
    void *user,
    h2_esp_adc_value_reading_t *out_reading) {
    if (state == NULL || !state->configured || sample == NULL ||
        out_reading == NULL || wait == NULL) {
        return false;
    }

    h2_esp_adc_value_stabilizer_t next_state = *state;
    h2_esp_adc_value_reading_t reading = {0};
    int32_t trigger_raw = 0;
    if (!read_raw_sample(sample, user, &trigger_raw)) {
        return false;
    }

    bool reseed = false;
    reading.reason = H2_ESP_ADC_VALUE_READ_STEADY;
    if (!next_state.initialized) {
        reading.reason = H2_ESP_ADC_VALUE_READ_STARTUP;
        reseed = true;
    } else {
        int64_t delta = (int64_t)trigger_raw -
                        (int64_t)next_state.stable_raw;
        if (delta < 0) {
            delta = -delta;
        }
        if (delta > H2_ESP_ADC_VALUE_SMOOTH_DELTA_RAW) {
            reading.reason = H2_ESP_ADC_VALUE_READ_JUMP;
            reseed = true;
        }
    }

    reading.immediate_raw = trigger_raw;
    if (!reseed) {
        if (!h2_esp_adc_numeric_stabilizer_update_internal(
                &next_state.filter,
                trigger_raw,
                &reading.stable_raw)) {
            return false;
        }
    } else {
        int32_t seed_samples[H2_ESP_ADC_NUMERIC_MEDIAN_WINDOW] = {0};
        size_t seed_count = 0u;
        const size_t total_samples =
            H2_ESP_ADC_VALUE_RESEED_DISCARD_SAMPLES +
            H2_ESP_ADC_NUMERIC_MEDIAN_WINDOW;
        for (size_t i = 1u; i < total_samples; ++i) {
            wait(user, H2_ESP_ADC_VALUE_RESEED_INTERVAL_US);
            int32_t raw = 0;
            if (!read_raw_sample(sample, user, &raw)) {
                return false;
            }
            reading.immediate_raw = raw;
            if (i >= H2_ESP_ADC_VALUE_RESEED_DISCARD_SAMPLES) {
                seed_samples[seed_count++] = raw;
            }
        }

        h2_esp_adc_numeric_stabilizer_init_internal(&next_state.filter);
        for (size_t i = 0u; i < H2_ESP_ADC_NUMERIC_MEDIAN_WINDOW; ++i) {
            if (!h2_esp_adc_numeric_stabilizer_update_internal(
                    &next_state.filter,
                    seed_samples[i],
                    &reading.stable_raw)) {
                return false;
            }
        }
    }

    next_state.stable_raw = reading.stable_raw;
    next_state.initialized = true;
    *state = next_state;
    *out_reading = reading;
    return true;
}

void h2_esp_adc_percent_stabilizer_init(
    h2_esp_adc_percent_stabilizer_t *state) {
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

bool h2_esp_adc_percent_stabilizer_update(
    h2_esp_adc_percent_stabilizer_t *state,
    uint16_t candidate_percent_x100,
    bool charging,
    uint16_t *out_percent_x100) {
    if (state == NULL || out_percent_x100 == NULL ||
        candidate_percent_x100 > 10000u) {
        return false;
    }

    if (!state->initialized || state->charging != charging) {
        state->published_percent_x100 = candidate_percent_x100;
        state->charging = charging;
        state->initialized = true;
    } else if (charging) {
        if (candidate_percent_x100 >= state->published_percent_x100 + 100u) {
            state->published_percent_x100 = candidate_percent_x100;
        }
    } else if ((uint32_t)candidate_percent_x100 + 100u <=
               state->published_percent_x100) {
        state->published_percent_x100 = candidate_percent_x100;
    }

    *out_percent_x100 = state->published_percent_x100;
    return true;
}
