#include "h2_esp_es8311_es7210_gain.h"

#include <assert.h>
#include <stdint.h>

int main(void) {
    const uint8_t gains[H2_ESP_ES8311_ES7210_GAIN_INPUT_COUNT] = {
        30u,
        30u,
        12u,
        0u,
    };

    assert(h2_esp_es8311_es7210_input_gain_register(
               24u, 0u, gains, 0u) == 0x18u);
    assert(h2_esp_es8311_es7210_input_gain_register(
               24u, 0x07u, gains, 0u) == 0x1au);
    assert(h2_esp_es8311_es7210_input_gain_register(
               24u, 0x07u, gains, 1u) == 0x1au);
    assert(h2_esp_es8311_es7210_input_gain_register(
               24u, 0x07u, gains, 2u) == 0x14u);
    assert(h2_esp_es8311_es7210_zero_legacy_ref_gain(1, 0u, 2u));
    assert(!h2_esp_es8311_es7210_zero_legacy_ref_gain(1, 0x04u, 2u));
    assert(!h2_esp_es8311_es7210_zero_legacy_ref_gain(0, 0u, 2u));
    return 0;
}
