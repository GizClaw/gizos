#include "h2_esp_es8311_audio_system.h"
#include "h2_esp_es8311_gain.h"

#include "h2_pcm_channels.h"

#include <string.h>

static int process_mic_fallback(
    const h2_esp_es8311_audio_system_config_t* config,
    const h2_audio_frame_t* raw_frame, h2_audio_frame_t* out_frame) {
  if (raw_frame->sample_rate_hz != config->sample_rate_hz ||
      out_frame->sample_rate_hz != config->sample_rate_hz ||
      raw_frame->channels != config->raw_channels ||
      out_frame->channels != config->processed_channels ||
      config->processed_channels != 1u) {
    return H2_AUDIO_ERR_UNSUPPORTED;
  }
  return h2_pcm_s16_select_mono(raw_frame, out_frame,
                                &config->mic_channel_index, 1u);
}

int h2_esp_es8311_audio_system_init(
    h2_esp_es8311_audio_system_t* system,
    const h2_esp_es8311_audio_system_config_t* config) {
  if (system == NULL || config == NULL) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  if (config->sample_rate_hz == 0u || config->frame_samples_per_channel == 0u ||
      config->frame_samples_per_channel >
          H2_ESP_ES8311_AUDIO_SYSTEM_MAX_FRAME_SAMPLES ||
      config->raw_channels == 0u ||
      config->raw_channels > H2_ESP_ES8311_AUDIO_SYSTEM_MAX_RAW_CHANNELS ||
      config->processed_channels != 1u ||
      config->mic_channel_index >= config->raw_channels ||
      config->ref_channel_index >= config->raw_channels ||
      config->max_tracks == 0u || config->track_queue_frames == 0u ||
      config->mic_queue_frames == 0u || config->mclk_multiple == 0u ||
      config->mic_gain_db > 30u || config->mic_gain_min_db % 3u != 0u ||
      (config->mic_gain_max_db != 0u && config->mic_gain_max_db % 3u != 0u) ||
      config->mic_gain_min_db >=
          (config->mic_gain_max_db == 0u ? 30u : config->mic_gain_max_db) ||
      (config->mic_gain_max_db != 0u && config->mic_gain_max_db > 30u) ||
      config->mic_gain_db < config->mic_gain_min_db ||
      config->mic_gain_db >
          (config->mic_gain_max_db == 0u ? 30u : config->mic_gain_max_db) ||
      !h2_es8311_volume_is_valid(&config->speaker_volume,
                                 config->codec_volume_default) ||
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
  const uint32_t max_mic_gain_db =
      config->mic_gain_max_db == 0u ? 30u : config->mic_gain_max_db;
  const uint32_t applied_mic_gain_db =
      (uint32_t)h2_esp_es8311_mic_gain_register(config->mic_gain_db) * 3u;
  system->mic_gain_percent = (applied_mic_gain_db - config->mic_gain_min_db) *
                             100u / (max_mic_gain_db - config->mic_gain_min_db);
  if (config->enable_aec) {
    int rc = h2_esp_es8311_sr_init(&system->sr, config);
    if (rc != H2_AUDIO_OK) {
      return rc;
    }
    system->sr_initialized = 1;
  }
  return H2_AUDIO_OK;
}

int h2_esp_es8311_audio_system_process_mic(h2_esp_es8311_audio_system_t* system,
                                           const h2_audio_frame_t* raw_frame,
                                           h2_audio_frame_t* out_frame,
                                           uint32_t timeout_ms) {
  if (system == NULL || raw_frame == NULL || out_frame == NULL) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  if (system->sr.available) {
    int rc =
        h2_esp_es8311_sr_process(&system->sr, raw_frame, out_frame, timeout_ms);
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
