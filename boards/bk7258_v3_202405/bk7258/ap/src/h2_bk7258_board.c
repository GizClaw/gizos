#include "h2_bk7258_board_private.h"

const h2_bk7258_audio_config_t h2_bk7258_audio_config = {
    .sample_rate = 16000u,
    .frame_samples_per_channel = 320u,
    .channels = 1u,
    .mic_channels = 2u,
    .speaker_channels = 1u,
    .bits_per_sample = 16u,
    .default_volume = 0x28u,
    .default_mic_gain = 0x2au,
    .frame_count = 4u,
};

const h2_bk7258_button_adc_range_t h2_bk7258_button_ranges[] = {
    { .id = 102u, .min_mv = 1u, .max_mv = 100u },
    { .id = 101u, .min_mv = 800u, .max_mv = 930u },
    { .id = 104u, .min_mv = 1600u, .max_mv = 1800u },
};

const size_t h2_bk7258_button_range_count =
    sizeof(h2_bk7258_button_ranges) / sizeof(h2_bk7258_button_ranges[0]);
