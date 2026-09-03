#include "h2_esp_es8311_gain.h"

#include <assert.h>

int main(void) {
    assert(h2_esp_es8311_mic_gain_register(0u) == 0u);
    assert(h2_esp_es8311_mic_gain_register(1u) == 0u);
    assert(h2_esp_es8311_mic_gain_register(2u) == 1u);
    assert(h2_esp_es8311_mic_gain_register(3u) == 1u);
    assert(h2_esp_es8311_mic_gain_register(17u) == 6u);
    assert(h2_esp_es8311_mic_gain_register(18u) == 6u);
    assert(h2_esp_es8311_mic_gain_register(19u) == 6u);
    assert(h2_esp_es8311_mic_gain_register(30u) == 10u);
    assert(h2_esp_es8311_mic_gain_register(99u) == 10u);
    return 0;
}
