#include "h2_esp_es8311_es7210_gain.h"

static uint8_t gain_from_db(uint32_t db) {
    if (db >= 38u) {
        return 14u;
    }
    return (uint8_t)((db + 1u) / 3u);
}

uint8_t h2_esp_es8311_es7210_input_gain_register(
    uint32_t default_gain_db,
    uint8_t override_mask,
    const uint8_t override_gain_db[H2_ESP_ES8311_ES7210_GAIN_INPUT_COUNT],
    uint8_t input) {
    uint32_t gain_db = default_gain_db;
    if (input < H2_ESP_ES8311_ES7210_GAIN_INPUT_COUNT &&
        (override_mask & (1u << input)) != 0u) {
        gain_db = override_gain_db[input];
    }
    return (uint8_t)(0x10u | gain_from_db(gain_db));
}

int h2_esp_es8311_es7210_zero_legacy_ref_gain(
    int enable_aec,
    uint8_t override_mask,
    uint8_t ref_input) {
    return enable_aec &&
        ref_input < H2_ESP_ES8311_ES7210_GAIN_INPUT_COUNT &&
        (override_mask & (1u << ref_input)) == 0u;
}
