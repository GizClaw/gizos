/* SDK/provider double containing only the configuration this board consumes. */
#ifndef H2_TEST_ES8311_AUDIO_SYSTEM_H
#define H2_TEST_ES8311_AUDIO_SYSTEM_H
#include "hal/h2_pal_audio.h"
#include "os/h2_pal_mem.h"
#include "os/h2_pal_queue.h"
#include "os/h2_pal_sync.h"
#include "driver/i2c_master.h"
#include <stdint.h>
typedef struct {
  uint32_t sample_rate_hz, frame_samples_per_channel;
  int raw_channels, processed_channels, mic_channel_index, ref_channel_index;
  int i2c_port, i2c_sda_gpio, i2c_scl_gpio;
  uint32_t i2c_speed_hz;
  int codec_i2c_addr;
  i2c_master_bus_handle_t i2c_bus;
  int i2s_port, i2s_dma_desc_num, i2s_dma_frame_num, mclk_multiple;
  int mclk_gpio, bclk_gpio, ws_gpio, dout_gpio, din_gpio, pa_gpio;
  int (*pa_set)(void*, int);
  int codec_volume_default, adc_digital_volume, mic_gain_db;
  int mic_gain_min_db, mic_gain_max_db, max_tracks, track_queue_frames;
  int mic_queue_frames, mic_task_stack_size, mic_task_priority,
      mic_task_core_id;
  int speaker_task_stack_size, speaker_task_priority, speaker_task_core_id;
  const h2_pal_mem_api_t* allocator;
  const h2_pal_queue_api_t* queue_api;
  const h2_pal_sync_api_t* sync_api;
  int enable_aec;
  struct {
    unsigned point_count;
    struct {
      unsigned percent, attenuation_half_db;
    } points[5];
  } speaker_volume;
} h2_esp_es8311_audio_system_config_t;
typedef struct {
  h2_pal_audio_t audio;
} h2_esp_es8311_audio_system_t;
int h2_esp_es8311_audio_system_init(h2_esp_es8311_audio_system_t*,
                                    const h2_esp_es8311_audio_system_config_t*);
int h2_esp_es8311_audio_system_deinit(h2_esp_es8311_audio_system_t*);
int h2_esp_es8311_audio_system_power_down(h2_esp_es8311_audio_system_t*);
int h2_esp_es8311_audio_system_prepare(h2_esp_es8311_audio_system_t*);
h2_pal_audio_t* h2_esp_es8311_audio_system_audio(h2_esp_es8311_audio_system_t*);
#endif
