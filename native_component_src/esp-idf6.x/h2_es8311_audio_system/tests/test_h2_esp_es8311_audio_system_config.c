#include "h2_esp_es8311_audio_system.h"

#include <assert.h>

int h2_esp_es8311_sr_init(h2_esp_es8311_sr_state_t *state,
                          const h2_esp_es8311_audio_system_config_t *config) {
  (void)state;
  (void)config;
  return H2_AUDIO_OK;
}

int h2_esp_es8311_sr_process(h2_esp_es8311_sr_state_t *state,
                             const h2_audio_frame_t *raw_frame,
                             h2_audio_frame_t *out_frame, uint32_t timeout_ms) {
  (void)state;
  (void)raw_frame;
  (void)out_frame;
  (void)timeout_ms;
  return H2_AUDIO_ERR_UNSUPPORTED;
}

int main(void) {
  const h2_pal_queue_api_t queue_api = {0};
  const h2_esp_es8311_audio_system_config_t config = {
      .sample_rate_hz = 16000u,
      .frame_samples_per_channel = 320u,
      .raw_channels = 2u,
      .processed_channels = 1u,
      .mic_channel_index = 0u,
      .ref_channel_index = 1u,
      .mclk_multiple = 256u,
      .codec_volume_default = 1u,
      .max_tracks = 1u,
      .track_queue_frames = 1u,
      .mic_queue_frames = 1u,
      .mic_task_stack_size = 1u,
      .speaker_task_stack_size = 1u,
      .queue_api = &queue_api,
      .sync_api = NULL,
  };
  h2_esp_es8311_audio_system_t system;
  assert(h2_esp_es8311_audio_system_init(&system, &config) ==
         H2_AUDIO_ERR_INVALID_ARG);
  return 0;
}
