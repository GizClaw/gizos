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
    .mic_gain_min = 0x00u,
    .mic_gain_max = 0x3fu,
    .frame_count = 4u,
};

/* Native-count calibration: lower key levels about 49/1517, released rail
 * about 7822. The EVB schematic annotates equal 0/0.6/1.2/1.8V key steps;
 * extrapolated upper centers are 2985/4453. Cuts are adjacent midpoints,
 * including the cut toward release. Do not assume a 4096-count ADC range.
 * Physical validation observed KEY1..4 at 43..44 / 1514..1517 /
 * 2970..2976 / 4539..4547, with release at 7811..7833. */
const h2_bk7258_button_adc_range_t h2_bk7258_button_ranges[] = {
    {.id = 102u, .min_raw = 0u, .max_raw = 783u},
    {.id = 101u, .min_raw = 784u, .max_raw = 2251u},
    {.id = 103u, .min_raw = 2252u, .max_raw = 3719u},
    {.id = 104u, .min_raw = 3720u, .max_raw = 6137u},
};

const size_t h2_bk7258_button_range_count =
    sizeof(h2_bk7258_button_ranges) / sizeof(h2_bk7258_button_ranges[0]);
