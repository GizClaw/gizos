#include "h2_esp_es8311_es7210_audio_system.h"

#include <assert.h>

int h2_esp_es8311_es7210_sr_init(
    h2_esp_es8311_es7210_sr_state_t *state,
    const h2_esp_es8311_es7210_audio_system_config_t *config) {
  (void)state;
  (void)config;
  return H2_AUDIO_OK;
}

int h2_esp_es8311_es7210_sr_process(h2_esp_es8311_es7210_sr_state_t *state,
                                    const h2_audio_frame_t *raw_frame,
                                    h2_audio_frame_t *out_frame,
                                    uint32_t timeout_ms) {
  (void)state;
  (void)raw_frame;
  (void)out_frame;
  (void)timeout_ms;
  return H2_AUDIO_ERR_UNSUPPORTED;
}

void h2_esp_es8311_es7210_sr_deinit(h2_esp_es8311_es7210_sr_state_t *state) {
  (void)state;
}

int main(void) {
  const h2_pal_queue_api_t queue_api = {0};
  h2_esp_es8311_es7210_audio_system_config_t config = {
      .sample_rate_hz = 16000u,
      .frame_samples_per_channel = 320u,
      .raw_channels = 4u,
      .processed_channels = 1u,
      .mic_channel_count = 2u,
      .mic_channel_indices = {1u, 2u},
      .ref_channel_index = 0u,
      .es7210_input_mask = 0x0fu,
      .es7210_ref_input_index = 0u,
      .aec_reference_gain_milli = 1000u,
      .mclk_multiple = 256u,
      .pa_gpio = 0,
      .codec_volume_default = 1u,
      .max_tracks = 1u,
      .track_queue_frames = 1u,
      .mic_queue_frames = 1u,
      .mic_task_stack_size = 1u,
      .speaker_task_stack_size = 1u,
      .queue_api = &queue_api,
      .sync_api = NULL,
  };
  h2_esp_es8311_es7210_audio_system_t system;
  assert(h2_esp_es8311_es7210_audio_system_init(&system, &config) ==
         H2_AUDIO_ERR_INVALID_ARG);
  const h2_pal_sync_api_t sync_api = {0};
  config.sync_api = &sync_api;
  assert(h2_esp_es8311_es7210_audio_system_init(&system, &config) == H2_AUDIO_OK);
  config.codec_volume_default = 0xb0u;
  config.speaker_volume = (h2_es8311_volume_config_t){
      .point_count = 3u, .points = {{1u, 120u}, {50u, 12u}, {100u, 0u}},
  };
  assert(h2_esp_es8311_es7210_audio_system_init(&system, &config) == H2_AUDIO_OK);
  config.speaker_volume.points[1].attenuation_half_db = 20u;
  assert(h2_es8311_volume_from_percent(&system.config.speaker_volume,
                                      system.config.codec_volume_default, 50u) == 164u);
  config.speaker_volume.point_count = 9u;
  assert(h2_esp_es8311_es7210_audio_system_init(&system, &config) == H2_AUDIO_ERR_INVALID_ARG);
  return 0;
}
