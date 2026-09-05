#include "h2_esp_board.h"

#include <assert.h>

int main(void) {
    h2_esp_board_audio_config_t config = {
        .i2s_dma_desc_num = 0u,
        .i2s_dma_frame_num = 0u,
        .mic_gain_db = 18u,
        .mic_queue_frames = 4u,
        .aggressive_aec_nlp = 0,
    };
    assert(h2_esp_board_audio_config_is_valid(&config));
    assert(h2_esp_board_audio_config_is_valid(NULL) == 0);

    config.mic_gain_db = 30u;
    assert(h2_esp_board_audio_config_is_valid(&config));
    config.mic_gain_db = 31u;
    assert(h2_esp_board_audio_config_is_valid(&config) == 0);
    config.mic_gain_db = 18u;

    config.mic_queue_frames = 0u;
    assert(h2_esp_board_audio_config_is_valid(&config) == 0);
    config.mic_queue_frames = 4u;

    config.aggressive_aec_nlp = 1;
    assert(h2_esp_board_audio_config_is_valid(&config));
    config.aggressive_aec_nlp = 2;
    assert(h2_esp_board_audio_config_is_valid(&config) == 0);
    config.aggressive_aec_nlp = -1;
    assert(h2_esp_board_audio_config_is_valid(&config) == 0);

    assert(h2_esp_board_audio_config_may_apply(0));
    assert(h2_esp_board_audio_config_may_apply(1) == 0);
    return 0;
}
