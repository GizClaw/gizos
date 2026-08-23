#include "h2_esp_es8311_es7210_audio_system.h"

#include <stdlib.h>
#include <string.h>

#include "esp_aec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#define H2_ESP_ES8311_ES7210_AEC_SAMPLE_RATE 16000u
#define H2_ESP_ES8311_ES7210_AEC_FILTER_LENGTH 4
#define H2_ESP_ES8311_ES7210_AEC_LOG_INTERVAL_FRAMES 64u

static const char *TAG = "h2_es7210_aec";

static void release_aec(h2_esp_es8311_es7210_sr_state_t *state) {
    if (state == NULL) {
        return;
    }
    if (state->aec_handle != NULL) {
        aec_destroy((aec_handle_t *)state->aec_handle);
    }
    heap_caps_free(state->mic_frame);
    heap_caps_free(state->ref_frame);
    heap_caps_free(state->out_frame);
    state->aec_handle = NULL;
    state->mic_frame = NULL;
    state->ref_frame = NULL;
    state->out_frame = NULL;
    state->available = 0;
    state->frame_samples = 0u;
}

static void *aec_calloc(size_t count, size_t size) {
    return heap_caps_aligned_calloc(
        16u, count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static uint32_t sample_magnitude(int16_t sample) {
    return sample < 0 ? (uint32_t)(-(int32_t)sample) : (uint32_t)sample;
}

static int16_t scale_reference(int16_t sample, uint32_t gain_milli) {
    int32_t scaled = ((int32_t)sample * (int32_t)gain_milli) / 1000;
    if (scaled > INT16_MAX) {
        return INT16_MAX;
    }
    if (scaled < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)scaled;
}

int h2_esp_es8311_es7210_sr_init(
    h2_esp_es8311_es7210_sr_state_t *state,
    const h2_esp_es8311_es7210_audio_system_config_t *config) {
    if (state == NULL || config == NULL) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    memset(state, 0, sizeof(*state));
    state->initialized = 1;
    if (!config->enable_aec) {
        return H2_AUDIO_OK;
    }
    if (config->sample_rate_hz != H2_ESP_ES8311_ES7210_AEC_SAMPLE_RATE ||
        config->processed_channels != 1u ||
        config->mic_channel_count == 0u ||
        config->mic_channel_count > H2_ESP_ES8311_ES7210_AUDIO_SYSTEM_MAX_MIC_CHANNELS ||
        config->raw_channels == 0u ||
        config->ref_channel_index >= config->raw_channels) {
        return H2_AUDIO_ERR_UNSUPPORTED;
    }
    for (uint8_t mic = 0u; mic < config->mic_channel_count; ++mic) {
        if (config->mic_channel_indices[mic] >= config->raw_channels) {
            return H2_AUDIO_ERR_UNSUPPORTED;
        }
    }

    aec_config_t aec_config = {
        .mic_num = (int)config->mic_channel_count,
        .ref_num = 1,
        .out_num = 1,
        .filter_length = H2_ESP_ES8311_ES7210_AEC_FILTER_LENGTH,
        .sample_rate = (int)config->sample_rate_hz,
        .caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
        .mode = AEC_MODE_FD_LOW_COST,
        .nlp_level = AEC_NLP_LEVEL_NORMAL,
    };
    state->aec_handle = aec_create_from_config(&aec_config);
    if (state->aec_handle == NULL) {
        return H2_AUDIO_ERR_NO_MEMORY;
    }
    const int chunk_samples = aec_get_chunksize((aec_handle_t *)state->aec_handle);
    if (chunk_samples <= 0 ||
        (uint16_t)chunk_samples != config->frame_samples_per_channel) {
        release_aec(state);
        return H2_AUDIO_ERR_UNSUPPORTED;
    }

    state->frame_samples = (size_t)chunk_samples;
    state->sample_rate_hz = config->sample_rate_hz;
    state->raw_channels = config->raw_channels;
    state->mic_channel_count = config->mic_channel_count;
    memcpy(
        state->mic_channel_indices,
        config->mic_channel_indices,
        sizeof(state->mic_channel_indices));
    state->ref_channel_index = config->ref_channel_index;
    state->reference_gain_milli = config->aec_reference_gain_milli;

    const size_t mic_sample_count = state->frame_samples * state->mic_channel_count;
    state->mic_frame = aec_calloc(mic_sample_count, sizeof(int16_t));
    state->ref_frame = aec_calloc(state->frame_samples, sizeof(int16_t));
    state->out_frame = aec_calloc(state->frame_samples, sizeof(int16_t));
    if (state->mic_frame == NULL || state->ref_frame == NULL ||
        state->out_frame == NULL) {
        release_aec(state);
        return H2_AUDIO_ERR_NO_MEMORY;
    }

    state->available = 1;
    ESP_LOGI(
        TAG,
        "enabled sample_rate=%lu frame=%u mics=%u lanes=%u,%u ref=%u ref_gain=%lu",
        (unsigned long)config->sample_rate_hz,
        (unsigned)config->frame_samples_per_channel,
        (unsigned)config->mic_channel_count,
        (unsigned)config->mic_channel_indices[0],
        (unsigned)config->mic_channel_indices[1],
        (unsigned)config->ref_channel_index,
        (unsigned long)config->aec_reference_gain_milli);
    return H2_AUDIO_OK;
}

int h2_esp_es8311_es7210_sr_process(
    h2_esp_es8311_es7210_sr_state_t *state,
    const h2_audio_frame_t *raw_frame,
    h2_audio_frame_t *out_frame,
    uint32_t timeout_ms) {
    (void)timeout_ms;
    if (state == NULL || raw_frame == NULL || out_frame == NULL ||
        raw_frame->data == NULL || out_frame->data == NULL) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    const size_t raw_bytes =
        state->frame_samples * state->raw_channels * sizeof(int16_t);
    const size_t out_bytes = state->frame_samples * sizeof(int16_t);
    if (!state->available || state->aec_handle == NULL ||
        raw_frame->sample_rate_hz != state->sample_rate_hz ||
        out_frame->sample_rate_hz != state->sample_rate_hz ||
        raw_frame->sample_format != H2_AUDIO_SAMPLE_S16LE ||
        out_frame->sample_format != H2_AUDIO_SAMPLE_S16LE ||
        raw_frame->channels != state->raw_channels ||
        out_frame->channels != 1u ||
        raw_frame->samples_per_channel != state->frame_samples ||
        raw_frame->bytes < raw_bytes ||
        out_frame->capacity < out_bytes) {
        return H2_AUDIO_ERR_UNSUPPORTED;
    }

    const int16_t *raw = (const int16_t *)raw_frame->data;
    uint32_t ref_raw_peak = 0u;
    uint32_t ref_peak = 0u;
    uint32_t mic_peaks[H2_ESP_ES8311_ES7210_AUDIO_SYSTEM_MAX_MIC_CHANNELS] = {0u, 0u};
    for (size_t sample = 0u; sample < state->frame_samples; ++sample) {
        const size_t raw_offset = sample * state->raw_channels;
        const int16_t ref_raw = raw[raw_offset + state->ref_channel_index];
        const int16_t ref = scale_reference(ref_raw, state->reference_gain_milli);
        state->ref_frame[sample] = ref;
        const uint32_t ref_raw_magnitude = sample_magnitude(ref_raw);
        if (ref_raw_magnitude > ref_raw_peak) {
            ref_raw_peak = ref_raw_magnitude;
        }
        const uint32_t ref_magnitude = sample_magnitude(ref);
        if (ref_magnitude > ref_peak) {
            ref_peak = ref_magnitude;
        }
        for (uint8_t mic = 0u; mic < state->mic_channel_count; ++mic) {
            const int16_t value = raw[raw_offset + state->mic_channel_indices[mic]];
            state->mic_frame[(mic * state->frame_samples) + sample] = value;
            const uint32_t magnitude = sample_magnitude(value);
            if (magnitude > mic_peaks[mic]) {
                mic_peaks[mic] = magnitude;
            }
        }
    }

    aec_process(
        (aec_handle_t *)state->aec_handle,
        state->mic_frame,
        state->ref_frame,
        state->out_frame);
    memcpy(out_frame->data, state->out_frame, out_bytes);
    out_frame->bytes = out_bytes;
    out_frame->samples_per_channel = (uint16_t)state->frame_samples;

    uint32_t out_peak = 0u;
    for (size_t sample = 0u; sample < state->frame_samples; ++sample) {
        const uint32_t magnitude = sample_magnitude(state->out_frame[sample]);
        if (magnitude > out_peak) {
            out_peak = magnitude;
        }
    }
    state->processed_frame_count++;
    if ((state->processed_frame_count % H2_ESP_ES8311_ES7210_AEC_LOG_INTERVAL_FRAMES) == 0u) {
        ESP_LOGI(
            TAG,
            "peaks ref_raw=%lu ref=%lu mic0=%lu mic1=%lu out=%lu",
            (unsigned long)ref_raw_peak,
            (unsigned long)ref_peak,
            (unsigned long)mic_peaks[0],
            (unsigned long)mic_peaks[1],
            (unsigned long)out_peak);
    }
    return H2_AUDIO_OK;
}

void h2_esp_es8311_es7210_sr_reset(h2_esp_es8311_es7210_sr_state_t *state) {
    if (state == NULL || state->frame_samples == 0u) {
        return;
    }
    if (state->mic_frame != NULL) {
        memset(
            state->mic_frame,
            0,
            state->frame_samples * state->mic_channel_count * sizeof(int16_t));
    }
    if (state->ref_frame != NULL) {
        memset(state->ref_frame, 0, state->frame_samples * sizeof(int16_t));
    }
    if (state->out_frame != NULL) {
        memset(state->out_frame, 0, state->frame_samples * sizeof(int16_t));
    }
    state->processed_frame_count = 0u;
}

void h2_esp_es8311_es7210_sr_deinit(h2_esp_es8311_es7210_sr_state_t *state) {
    if (state == NULL) {
        return;
    }
    release_aec(state);
    memset(state, 0, sizeof(*state));
}
