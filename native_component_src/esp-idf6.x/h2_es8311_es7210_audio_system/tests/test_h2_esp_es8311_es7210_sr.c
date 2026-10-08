#include "h2_esp_es8311_es7210_audio_system.h"

#include "esp_heap_caps.h"
#include "fake_esp_aec.h"

#include <stdint.h>

static h2_esp_es8311_es7210_aec_stats_t observed;
static unsigned observation_count;
static void observe(void *user, const h2_esp_es8311_es7210_aec_stats_t *stats) {
    ++*(unsigned *)user;
    observed = *stats;
}

#define CHECK(condition)        \
    do {                        \
        if (!(condition)) {     \
            return __LINE__;    \
        }                       \
    } while (0)

int main(void) {
    fake_esp_aec_reset(3);
    const h2_esp_es8311_es7210_audio_system_config_t config = {
        .sample_rate_hz = 16000u,
        .frame_samples_per_channel = 3u,
        .raw_channels = 4u,
        .processed_channels = 1u,
        .mic_channel_count = 2u,
        .mic_channel_indices = {1u, 3u},
        .ref_channel_index = 0u,
        .aec_reference_gain_milli = 2000u,
        .enable_aec = 1,
        .aec_observe = observe,
        .aec_observer_user = &observation_count,
    };
    h2_esp_es8311_es7210_sr_state_t state;
    CHECK(h2_esp_es8311_es7210_sr_init(&state, &config) == H2_AUDIO_OK);

    const aec_config_t *aec_config = fake_esp_aec_config();
    CHECK(aec_config->mic_num == 2);
    CHECK(aec_config->ref_num == 1);
    CHECK(aec_config->out_num == 1);
    CHECK(aec_config->filter_length == 4);
    CHECK(aec_config->sample_rate == 16000);
    CHECK(aec_config->caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    CHECK(aec_config->mode == AEC_MODE_FD_LOW_COST);
    CHECK(aec_config->nlp_level == AEC_NLP_LEVEL_NORMAL);

    int16_t raw_samples[] = {
        100, 10, 99, 20,
        -200, 30, 99, 40,
        20000, 50, 99, 60,
    };
    int16_t out_samples[3] = {0, 0, 0};
    const h2_audio_frame_t raw_frame = {
        .data = raw_samples,
        .capacity = sizeof(raw_samples),
        .bytes = sizeof(raw_samples),
        .sample_rate_hz = 16000u,
        .channels = 4u,
        .sample_format = H2_AUDIO_SAMPLE_S16LE,
        .samples_per_channel = 3u,
    };
    h2_audio_frame_t out_frame = {
        .data = out_samples,
        .capacity = sizeof(out_samples),
        .sample_rate_hz = 16000u,
        .channels = 1u,
        .sample_format = H2_AUDIO_SAMPLE_S16LE,
    };
    CHECK(h2_esp_es8311_es7210_sr_process(
              &state, &raw_frame, &out_frame, 0u) == H2_AUDIO_OK);

    const int16_t *mic = fake_esp_aec_mic();
    CHECK(mic[0] == 10 && mic[1] == 30 && mic[2] == 50);
    CHECK(mic[3] == 20 && mic[4] == 40 && mic[5] == 60);
    const int16_t *ref = fake_esp_aec_ref();
    CHECK(ref[0] == 200 && ref[1] == -400 && ref[2] == INT16_MAX);
    CHECK(out_samples[0] == -170);
    CHECK(out_samples[1] == 470);
    CHECK(out_samples[2] == -32657);
    CHECK(out_frame.bytes == sizeof(out_samples));
    CHECK(out_frame.samples_per_channel == 3u);
    CHECK(fake_esp_aec_process_count() == 1);
    CHECK(observation_count == 1u && observed.frame == 1u);
    CHECK(observed.samples == 3u && observed.raw_channels == 4u);
    CHECK(observed.mic_lane == 1u && observed.ref_lane == 0u);
    CHECK(observed.lane_power[0] == 133350000u);
    CHECK(observed.lane_power[1] == 1166u);
    CHECK(observed.lane_power[2] == 9801u);
    CHECK(observed.lane_power[3] == 1866u);
    CHECK(observed.lane_peak[0] == 20000u);
    CHECK(observed.output_peak == 32657u);
    CHECK(observed.output_power == (uint32_t)((UINT64_C(170)*170 + UINT64_C(470)*470 + UINT64_C(32657)*32657)/3u));

    h2_esp_es8311_es7210_sr_reset(&state);
    CHECK(state.processed_frame_count == 0u);
    for (size_t sample = 0u; sample < 6u; ++sample) {
        CHECK(state.mic_frame[sample] == 0);
    }
    for (size_t sample = 0u; sample < 3u; ++sample) {
        CHECK(state.ref_frame[sample] == 0);
        CHECK(state.out_frame[sample] == 0);
    }

    h2_esp_es8311_es7210_sr_deinit(&state);
    CHECK(fake_esp_aec_destroy_count() == 1);
    CHECK(state.initialized == 0);

    h2_esp_es8311_es7210_audio_system_config_t aggressive = config;
    aggressive.aec_nlp_level = H2_ESP_ES8311_ES7210_AEC_NLP_AGGRESSIVE;
    CHECK(h2_esp_es8311_es7210_sr_init(&state, &aggressive) == H2_AUDIO_OK);
    CHECK(fake_esp_aec_config()->nlp_level == AEC_NLP_LEVEL_AGGR);
    raw_samples[0] = INT16_MIN;
    raw_samples[4] = INT16_MAX;
    CHECK(h2_esp_es8311_es7210_sr_process(&state, &raw_frame, &out_frame, 0u) == H2_AUDIO_OK);
    CHECK(observed.lane_clipped[0] == 2u);
    CHECK(observed.lane_peak[0] == 32768u);
    h2_esp_es8311_es7210_sr_deinit(&state);
    aggressive.aec_nlp_level = (h2_esp_es8311_es7210_aec_nlp_level_t)9;
    CHECK(h2_esp_es8311_es7210_sr_init(&state, &aggressive) == H2_AUDIO_ERR_INVALID_ARG);
    CHECK(state.aec_handle == NULL);
    aggressive = config;
    aggressive.raw_channels = 5u;
    CHECK(h2_esp_es8311_es7210_sr_init(&state, &aggressive) == H2_AUDIO_ERR_UNSUPPORTED);
    CHECK(state.aec_handle == NULL);
    return 0;
}
