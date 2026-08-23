#ifndef H2_ESP_ES8311_ES7210_AUDIO_SYSTEM_TEST_STUB_H
#define H2_ESP_ES8311_ES7210_AUDIO_SYSTEM_TEST_STUB_H

#include <stddef.h>
#include <stdint.h>

#define H2_ESP_ES8311_ES7210_AUDIO_SYSTEM_MAX_MIC_CHANNELS 2u

#define H2_AUDIO_OK 0
#define H2_AUDIO_ERR_INVALID_ARG -1
#define H2_AUDIO_ERR_UNSUPPORTED -2
#define H2_AUDIO_ERR_NO_MEMORY -3

#define H2_AUDIO_SAMPLE_S16LE 1

typedef struct h2_audio_frame {
    void *data;
    size_t capacity;
    size_t bytes;
    uint32_t sample_rate_hz;
    uint8_t channels;
    int sample_format;
    uint16_t samples_per_channel;
} h2_audio_frame_t;

typedef struct h2_esp_es8311_es7210_audio_system_config {
    uint32_t sample_rate_hz;
    uint16_t frame_samples_per_channel;
    uint8_t raw_channels;
    uint8_t processed_channels;
    uint8_t mic_channel_count;
    uint8_t mic_channel_indices[H2_ESP_ES8311_ES7210_AUDIO_SYSTEM_MAX_MIC_CHANNELS];
    uint8_t ref_channel_index;
    uint32_t aec_reference_gain_milli;
    int enable_aec;
} h2_esp_es8311_es7210_audio_system_config_t;

typedef struct h2_esp_es8311_es7210_sr_state {
    int initialized;
    int available;
    uint32_t sample_rate_hz;
    size_t frame_samples;
    uint8_t raw_channels;
    uint8_t mic_channel_count;
    uint8_t mic_channel_indices[H2_ESP_ES8311_ES7210_AUDIO_SYSTEM_MAX_MIC_CHANNELS];
    uint8_t ref_channel_index;
    uint32_t reference_gain_milli;
    void *aec_handle;
    int16_t *mic_frame;
    int16_t *ref_frame;
    int16_t *out_frame;
    uint32_t processed_frame_count;
} h2_esp_es8311_es7210_sr_state_t;

int h2_esp_es8311_es7210_sr_init(
    h2_esp_es8311_es7210_sr_state_t *state,
    const h2_esp_es8311_es7210_audio_system_config_t *config);

int h2_esp_es8311_es7210_sr_process(
    h2_esp_es8311_es7210_sr_state_t *state,
    const h2_audio_frame_t *raw_frame,
    h2_audio_frame_t *out_frame,
    uint32_t timeout_ms);

void h2_esp_es8311_es7210_sr_reset(h2_esp_es8311_es7210_sr_state_t *state);
void h2_esp_es8311_es7210_sr_deinit(h2_esp_es8311_es7210_sr_state_t *state);

#endif
