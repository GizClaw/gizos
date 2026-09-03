#include "h2_esp_es8311_gain.h"

uint8_t h2_esp_es8311_mic_gain_register(uint32_t gain_db) {
    /* ES8311 SYSTEM14.PGAGAIN uses 3 dB steps from 0 through 30 dB. */
    if (gain_db >= 30u) {
        return 10u;
    }
    return (uint8_t)((gain_db + 1u) / 3u);
}
