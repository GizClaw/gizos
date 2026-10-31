#ifndef H2_TEST_ES8311_SYSTEM_H
#define H2_TEST_ES8311_SYSTEM_H
#include "h2_pal.h"
#include "h2_es8311_volume.h"
typedef struct {
  int unused;
} h2_esp_es8311_audio_system_t;
typedef struct {
  int sample_rate_hz, frame_samples_per_channel, raw_channels,
      processed_channels, mic_channel_index, ref_channel_index;
  void* i2c_bus;
  int i2c_port, i2c_sda_gpio, i2c_scl_gpio, i2c_speed_hz, codec_i2c_addr,
      i2s_port, mclk_multiple;
  int mclk_gpio, bclk_gpio, ws_gpio, dout_gpio, din_gpio, pa_gpio,
      codec_volume_default;
  h2_es8311_volume_config_t speaker_volume;
  int adc_digital_volume, mic_gain_db, mic_gain_max_db, max_tracks,
      track_queue_frames, mic_queue_frames;
  int mic_task_stack_size, mic_task_priority, mic_task_core_id,
      speaker_task_stack_size, speaker_task_priority, speaker_task_core_id;
  h2_pal_mem_api_t* allocator;
  const h2_pal_queue_api_t* queue_api;
  const h2_pal_sync_api_t* sync_api;
  int enable_aec;
} h2_esp_es8311_audio_system_config_t;
int h2_esp_es8311_audio_system_init(h2_esp_es8311_audio_system_t*,
                                    const h2_esp_es8311_audio_system_config_t*);
int h2_esp_es8311_audio_system_deinit(h2_esp_es8311_audio_system_t*);
const h2_pal_audio_api_t* h2_esp_es8311_audio_system_audio(
    h2_esp_es8311_audio_system_t*);
#endif
