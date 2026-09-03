#include "h2_esp_es8311_audio_system.h"

#include <string.h>

static int frame_is_s16_16k(const h2_audio_frame_t *frame, uint32_t sample_rate_hz) {
    return frame != NULL &&
           frame->data != NULL &&
           frame->sample_rate_hz == sample_rate_hz &&
           frame->sample_format == H2_AUDIO_SAMPLE_S16LE;
}

static int process_mic_fallback(
    const h2_esp_es8311_audio_system_config_t *config,
    const h2_audio_frame_t *raw_frame,
    h2_audio_frame_t *out_frame) {
    if (!frame_is_s16_16k(raw_frame, config->sample_rate_hz) ||
        !frame_is_s16_16k(out_frame, config->sample_rate_hz) ||
        raw_frame->channels != config->raw_channels ||
        out_frame->channels != config->processed_channels ||
        config->processed_channels != 1u ||
        config->raw_channels == 0u ||
        config->mic_channel_index >= config->raw_channels) {
        return H2_AUDIO_ERR_UNSUPPORTED;
    }

    const size_t raw_frame_bytes = h2_audio_frame_frame_bytes(raw_frame);
    const size_t out_frame_bytes = h2_audio_frame_frame_bytes(out_frame);
    if (raw_frame_bytes == 0u || out_frame_bytes == 0u) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }

    const size_t raw_frames = raw_frame->bytes / raw_frame_bytes;
    size_t out_frames = out_frame->capacity / out_frame_bytes;
    if (out_frames > raw_frames) {
        out_frames = raw_frames;
    }
    if (out_frames == 0u) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }

    const int16_t *raw = (const int16_t *)raw_frame->data;
    int16_t *out = (int16_t *)out_frame->data;
    for (size_t i = 0u; i < out_frames; ++i) {
        out[i] = raw[(i * config->raw_channels) + config->mic_channel_index];
    }
    out_frame->bytes = out_frames * out_frame_bytes;
    out_frame->samples_per_channel = (uint16_t)out_frames;
    return H2_AUDIO_OK;
}

int h2_esp_es8311_audio_system_init(
    h2_esp_es8311_audio_system_t *system,
    const h2_esp_es8311_audio_system_config_t *config) {
    if (system == NULL || config == NULL) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    if (config->sample_rate_hz == 0u ||
        config->frame_samples_per_channel == 0u ||
        config->frame_samples_per_channel > H2_ESP_ES8311_AUDIO_SYSTEM_MAX_FRAME_SAMPLES ||
        config->raw_channels == 0u ||
        config->raw_channels > H2_ESP_ES8311_AUDIO_SYSTEM_MAX_RAW_CHANNELS ||
        config->processed_channels != 1u ||
        config->mic_channel_index >= config->raw_channels ||
        config->ref_channel_index >= config->raw_channels ||
        config->max_tracks == 0u ||
        config->track_queue_frames == 0u ||
        config->mic_queue_frames == 0u ||
        config->mclk_multiple == 0u ||
        config->codec_volume_default == 0u ||
        config->mic_task_stack_size == 0u ||
        config->speaker_task_stack_size == 0u ||
        (config->aec_nlp_level != H2_ESP_ES8311_AEC_NLP_NORMAL &&
         config->aec_nlp_level != H2_ESP_ES8311_AEC_NLP_AGGRESSIVE) ||
        config->queue_api == NULL || config->sync_api == NULL) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    memset(system, 0, sizeof(*system));
    system->config = *config;
    system->speaker_volume_percent = 100u;
    if (config->enable_aec) {
        int rc = h2_esp_es8311_sr_init(&system->sr, config);
        if (rc != H2_AUDIO_OK) {
            return rc;
        }
        system->sr_initialized = 1;
    }
    return H2_AUDIO_OK;
}

int h2_esp_es8311_audio_system_process_mic(
    h2_esp_es8311_audio_system_t *system,
    const h2_audio_frame_t *raw_frame,
    h2_audio_frame_t *out_frame,
    uint32_t timeout_ms) {
    if (system == NULL || raw_frame == NULL || out_frame == NULL) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    if (system->sr.available) {
        int rc = h2_esp_es8311_sr_process(&system->sr, raw_frame, out_frame, timeout_ms);
        if (rc == H2_AUDIO_OK) {
            return rc;
        }
        if (system->config.enable_aec) {
            return rc;
        }
    }
    if (system->config.enable_aec) {
        return H2_AUDIO_ERR_UNSUPPORTED;
    }
    (void)timeout_ms;
    return process_mic_fallback(&system->config, raw_frame, out_frame);
}
