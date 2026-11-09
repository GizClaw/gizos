#include "h2_esp_board_private.h"

#include "h2_esp_board_config.h"
#include "h2_esp_h200_board_internal.h"
#include "h2_esp_lazy_audio.h"
#include "h2_esp_es8311_es7210_audio_system.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

#define H2_H200_AUDIO_SAMPLE_RATE 16000u
#define H2_H200_AUDIO_FRAME_SAMPLES 512u

static const char* TAG = "h2_h200_audio";
static h2_esp_es8311_es7210_audio_system_t s_audio_system;
static h2_esp_lazy_audio_t s_lazy_audio;
static int s_audio_system_initialized;
static int s_lazy_audio_initialized;

static int set_pa(void* user, int enabled) {
  (void)user;
  return h2_esp_h200_board_set_pa(enabled);
}

static h2_pal_audio_t* resolve_audio(void* user) {
  (void)user;
  if (!s_audio_system_initialized) {
    if (h2_esp_h200_board_init_io() != H2_PAL_OK) {
      ESP_LOGE(TAG, "board io unavailable");
      return NULL;
    }
    i2c_master_bus_handle_t bus = h2_esp_h200_board_i2c_bus();
    if (bus == NULL) {
      ESP_LOGE(TAG, "i2c bus unavailable");
      return NULL;
    }
    const h2_esp_es8311_es7210_audio_system_config_t config = {
        .sample_rate_hz = H2_H200_AUDIO_SAMPLE_RATE,
        .frame_samples_per_channel = H2_H200_AUDIO_FRAME_SAMPLES,
        .raw_channels = 4u,
        .processed_channels = 1u,
        .mic_channel_count = 1u,
        .mic_channel_indices = {1u, 0u},
        .ref_channel_index = 0u,
        .es7210_input_mask = 0x07u,
        .es7210_ref_input_index = 2u,
        .aec_reference_gain_milli = 1000u,
        .i2c_port = H2_H200_I2C_PORT,
        .i2c_sda_gpio = H2_H200_I2C_SDA_GPIO,
        .i2c_scl_gpio = H2_H200_I2C_SCL_GPIO,
        .i2c_speed_hz = H2_H200_I2C_SPEED_HZ,
        .i2c_bus = bus,
        .es8311_i2c_addr = 0x18u,
        .es7210_i2c_addr = 0x40u,
        .i2s_port = H2_H200_AUDIO_I2S_PORT,
        .mclk_multiple = 384u,
        .mclk_gpio = H2_H200_AUDIO_MCLK_GPIO,
        .bclk_gpio = H2_H200_AUDIO_BCLK_GPIO,
        .ws_gpio = H2_H200_AUDIO_WS_GPIO,
        .dout_gpio = H2_H200_AUDIO_DOUT_GPIO,
        .din_gpio = H2_H200_AUDIO_DIN_GPIO,
        .pa_gpio = -1,
        .set_pa = set_pa,
        .codec_volume_default = 0xb0u,
        .mic_gain_db = 30u,
        .es7210_input_gain_mask = 0x07u,
        .es7210_input_gain_db = {30u, 30u, 12u, 0u},
        .max_tracks = 4u,
        .track_queue_frames = 4u,
        .mic_queue_frames = 4u,
        .mic_task_stack_size = 6144u,
        .mic_task_priority = tskIDLE_PRIORITY + 5u,
        .mic_task_core_id = tskNO_AFFINITY,
        .speaker_task_stack_size = 4096u,
        .speaker_task_priority = tskIDLE_PRIORITY + 4u,
        .speaker_task_core_id = tskNO_AFFINITY,
        .allocator = h2_esp_board_default_allocator(),
        .queue_api = h2_esp_board_queue_api(),
        .sync_api = h2_esp_board_sync_api(),
        .enable_aec = 1,
    };
    int rc = h2_esp_es8311_es7210_audio_system_init(&s_audio_system, &config);
    if (rc != H2_AUDIO_OK) {
      ESP_LOGE(TAG, "audio init failed rc=%d", rc);
      return NULL;
    }
    s_audio_system_initialized = 1;
  }
  return h2_esp_es8311_es7210_audio_system_audio(&s_audio_system);
}

h2_pal_audio_t* h2_esp_board_audio(void) {
  if (!s_lazy_audio_initialized) {
    if (h2_esp_lazy_audio_init(&s_lazy_audio, NULL, resolve_audio) !=
        H2_AUDIO_OK) {
      return NULL;
    }
    s_lazy_audio_initialized = 1;
  }
  return h2_esp_lazy_audio_api(&s_lazy_audio);
}

h2_pal_result_t h2_esp_board_audio_deinit(void) {
  if (s_audio_system_initialized) {
    const int rc = h2_esp_es8311_es7210_audio_system_deinit(&s_audio_system);
    if (rc != H2_AUDIO_OK) {
      return rc;
    }
    s_audio_system_initialized = 0;
  }
  memset(&s_lazy_audio, 0, sizeof(s_lazy_audio));
  s_lazy_audio_initialized = 0;
  return H2_PAL_OK;
}
