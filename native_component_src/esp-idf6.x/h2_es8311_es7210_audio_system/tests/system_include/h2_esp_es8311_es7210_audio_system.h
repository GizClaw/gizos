#ifndef H2_ESP_ES8311_ES7210_AUDIO_SYSTEM_CONFIG_TEST_STUB_H
#define H2_ESP_ES8311_ES7210_AUDIO_SYSTEM_CONFIG_TEST_STUB_H

#include <stddef.h>
#include <stdint.h>

#define H2_ESP_ES8311_ES7210_AUDIO_SYSTEM_MAX_FRAME_SAMPLES 512u
#define H2_ESP_ES8311_ES7210_AUDIO_SYSTEM_MAX_RAW_CHANNELS 4u
#define H2_ESP_ES8311_ES7210_AUDIO_SYSTEM_MAX_MIC_CHANNELS 2u

#define H2_AUDIO_OK 0
#define H2_AUDIO_ERR_INVALID_ARG -1
#define H2_AUDIO_ERR_UNSUPPORTED -2
#define H2_AUDIO_SAMPLE_S16LE 1

typedef struct h2_pal_queue_api {
  int unused;
} h2_pal_queue_api_t;

typedef struct h2_pal_sync_api {
  int unused;
} h2_pal_sync_api_t;

typedef int (*h2_esp_es8311_es7210_set_pa_fn)(void *user, int enabled);

typedef struct h2_audio_frame {
  void *data;
  size_t capacity;
  size_t bytes;
  uint32_t sample_rate_hz;
  uint16_t samples_per_channel;
  uint8_t channels;
  int sample_format;
} h2_audio_frame_t;

static inline size_t h2_audio_frame_frame_bytes(const h2_audio_frame_t *frame) {
  return frame != NULL && frame->sample_format == H2_AUDIO_SAMPLE_S16LE
             ? (size_t)frame->channels * sizeof(int16_t)
             : 0u;
}

typedef struct h2_esp_es8311_es7210_audio_system_config {
  uint32_t sample_rate_hz;
  uint16_t frame_samples_per_channel;
  uint8_t raw_channels;
  uint8_t processed_channels;
  uint8_t mic_channel_count;
  uint8_t
      mic_channel_indices[H2_ESP_ES8311_ES7210_AUDIO_SYSTEM_MAX_MIC_CHANNELS];
  uint8_t ref_channel_index;
  uint8_t es7210_input_mask;
  uint8_t es7210_ref_input_index;
  uint32_t aec_reference_gain_milli;
  uint16_t mclk_multiple;
  int pa_gpio;
  h2_esp_es8311_es7210_set_pa_fn set_pa;
  uint8_t codec_volume_default;
  uint8_t es7210_input_gain_mask;
  uint8_t max_tracks;
  uint8_t track_queue_frames;
  uint8_t mic_queue_frames;
  uint32_t mic_task_stack_size;
  uint32_t speaker_task_stack_size;
  const h2_pal_queue_api_t *queue_api;
  const h2_pal_sync_api_t *sync_api;
  int enable_aec;
} h2_esp_es8311_es7210_audio_system_config_t;

typedef struct h2_esp_es8311_es7210_sr_state {
  int available;
} h2_esp_es8311_es7210_sr_state_t;

typedef struct h2_esp_es8311_es7210_audio_system {
  h2_esp_es8311_es7210_audio_system_config_t config;
  h2_esp_es8311_es7210_sr_state_t sr;
  int sr_initialized;
  uint32_t speaker_volume_percent;
} h2_esp_es8311_es7210_audio_system_t;

int h2_esp_es8311_es7210_audio_system_init(
    h2_esp_es8311_es7210_audio_system_t *system,
    const h2_esp_es8311_es7210_audio_system_config_t *config);
int h2_esp_es8311_es7210_audio_system_process_mic(
    h2_esp_es8311_es7210_audio_system_t *system,
    const h2_audio_frame_t *raw_frame, h2_audio_frame_t *out_frame,
    uint32_t timeout_ms);
int h2_esp_es8311_es7210_sr_init(
    h2_esp_es8311_es7210_sr_state_t *state,
    const h2_esp_es8311_es7210_audio_system_config_t *config);
int h2_esp_es8311_es7210_sr_process(h2_esp_es8311_es7210_sr_state_t *state,
                                    const h2_audio_frame_t *raw_frame,
                                    h2_audio_frame_t *out_frame,
                                    uint32_t timeout_ms);
void h2_esp_es8311_es7210_sr_deinit(h2_esp_es8311_es7210_sr_state_t *state);

#endif
