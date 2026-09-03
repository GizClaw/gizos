#include "h2_esp_board.h"

int h2_esp_board_audio_config_is_valid(
    const h2_esp_board_audio_config_t *config) {
    if (config == NULL || config->mic_queue_frames == 0u ||
        config->mic_gain_db > 30u ||
        (config->aggressive_aec_nlp != 0 &&
         config->aggressive_aec_nlp != 1)) {
        return 0;
    }
    return 1;
}

int h2_esp_board_audio_config_may_apply(int already_initialized) {
    return already_initialized == 0;
}
