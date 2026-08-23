#ifndef H2_ESP_ES8311_ES7210_GAIN_H
#define H2_ESP_ES8311_ES7210_GAIN_H

#include <stdint.h>

#define H2_ESP_ES8311_ES7210_GAIN_INPUT_COUNT 4u

uint8_t h2_esp_es8311_es7210_input_gain_register(
    uint32_t default_gain_db,
    uint8_t override_mask,
    const uint8_t override_gain_db[H2_ESP_ES8311_ES7210_GAIN_INPUT_COUNT],
    uint8_t input);
int h2_esp_es8311_es7210_zero_legacy_ref_gain(
    int enable_aec,
    uint8_t override_mask,
    uint8_t ref_input);

#endif
