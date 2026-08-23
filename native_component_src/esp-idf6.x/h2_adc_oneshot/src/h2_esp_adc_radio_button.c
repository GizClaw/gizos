#include "h2_esp_adc_stabilizer.h"

#include <string.h>

void h2_esp_adc_radio_button_init(h2_esp_adc_radio_button_state_t *state) {
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

bool h2_esp_adc_radio_button_update(
    h2_esp_adc_radio_button_state_t *state,
    h2_esp_adc_radio_button_sample_kind_t kind,
    uint32_t button_id,
    uint32_t *out_button_id) {
    if (state == NULL || out_button_id == NULL ||
        kind < H2_ESP_ADC_RADIO_BUTTON_SAMPLE_UNKNOWN ||
        kind > H2_ESP_ADC_RADIO_BUTTON_SAMPLE_PRESSED ||
        (kind == H2_ESP_ADC_RADIO_BUTTON_SAMPLE_PRESSED && button_id == 0u) ||
        (kind != H2_ESP_ADC_RADIO_BUTTON_SAMPLE_PRESSED && button_id != 0u)) {
        return false;
    }

    if (state->stable_button_id != 0u) {
        state->candidate_button_id = 0u;
        state->candidate_count = 0u;
        if (kind == H2_ESP_ADC_RADIO_BUTTON_SAMPLE_RELEASED) {
            if (state->release_count < H2_ESP_ADC_RADIO_BUTTON_CONFIRM_SAMPLES) {
                ++state->release_count;
            }
            if (state->release_count >= H2_ESP_ADC_RADIO_BUTTON_CONFIRM_SAMPLES) {
                state->stable_button_id = 0u;
                state->release_count = 0u;
            }
        } else {
            state->release_count = 0u;
        }
        *out_button_id = state->stable_button_id;
        return true;
    }

    state->release_count = 0u;
    if (kind != H2_ESP_ADC_RADIO_BUTTON_SAMPLE_PRESSED) {
        state->candidate_button_id = 0u;
        state->candidate_count = 0u;
        *out_button_id = 0u;
        return true;
    }

    if (state->candidate_button_id != button_id) {
        state->candidate_button_id = button_id;
        state->candidate_count = 1u;
    } else if (state->candidate_count < H2_ESP_ADC_RADIO_BUTTON_CONFIRM_SAMPLES) {
        ++state->candidate_count;
    }
    if (state->candidate_count >= H2_ESP_ADC_RADIO_BUTTON_CONFIRM_SAMPLES) {
        state->stable_button_id = state->candidate_button_id;
        state->candidate_button_id = 0u;
        state->candidate_count = 0u;
    }
    *out_button_id = state->stable_button_id;
    return true;
}
