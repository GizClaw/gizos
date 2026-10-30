#include "h2_esp_board_private.h"
#include "h2_esp_board_config.h"
#include "h2_ws175c_audio_diagnostics.h"

#include "h2_esp_lazy_audio.h"

#include "h2_esp_es8311_es7210_audio_system.h"
#include "h2_esp_board_internal.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define H2_WS175C_AUDIO_SAMPLE_RATE 16000u
#define H2_WS175C_AUDIO_MCLK_MULTIPLE 384u
#define H2_WS175C_AUDIO_FRAME_SAMPLES 512u
#define H2_WS175C_AUDIO_RAW_CHANNELS 4u
#define H2_WS175C_AUDIO_PROCESSED_CHANNELS 1u
#define H2_WS175C_AUDIO_I2C_SPEED_HZ 100000u
#define H2_WS175C_AUDIO_ES8311_ADDR 0x18
#define H2_WS175C_AUDIO_ES7210_ADDR 0x40
#define H2_WS175C_AUDIO_I2S_PORT 1
/* ES8311 register 0x32: 0xBF is unity (0 dB). The former 0xB0 ceiling
 * attenuated even 100% playback by 7.5 dB. */
#define H2_WS175C_AUDIO_CODEC_VOLUME_DEFAULT 0xbfu
/* Integer API value 38 selects the physical maximum 37.5 dB PGA setting,
 * qualified with the existing
 * three-band/two-level AEC runner and an independent physical board source.
 * The DAC reference remains separately fixed at 12 dB. */
#define H2_WS175C_AUDIO_MIC_GAIN_DEFAULT_DB 38u
#define H2_WS175C_AUDIO_REFERENCE_GAIN_DB 12u
#define H2_WS175C_AUDIO_MAX_TRACKS 4u
#define H2_WS175C_AUDIO_TRACK_QUEUE_FRAMES 4u
#define H2_WS175C_AUDIO_MIC_QUEUE_FRAMES 4u
#define H2_WS175C_AUDIO_MIC_TASK_STACK 6144u
#define H2_WS175C_AUDIO_SPEAKER_TASK_STACK 4096u

static const char* TAG = "h2_ws175c_audio";
static h2_esp_es8311_es7210_audio_system_t s_audio_system;
static int s_audio_system_initialized;
static h2_esp_lazy_audio_t s_lazy_audio;
static int s_lazy_audio_initialized;
static uint8_t s_probe_microphones;
static uint8_t s_probe_reference_gain_db;

int h2_ws175c_audio_probe_select_aec(uint8_t microphones,
                                     uint8_t reference_gain_db) {
  if (microphones == 0u || microphones > 2u || reference_gain_db > 38u)
    return H2_AUDIO_ERR_INVALID_ARG;
  if (s_audio_system_initialized) return H2_AUDIO_ERR_INVALID_STATE;
  s_probe_microphones = microphones;
  s_probe_reference_gain_db = reference_gain_db;
  return h2_esp_board_audio() != NULL ? H2_AUDIO_OK
                                      : H2_AUDIO_ERR_INVALID_STATE;
}

int h2_ws175c_audio_probe_release(void) { return h2_esp_board_audio_deinit(); }

int h2_ws175c_audio_probe_read_controls(h2_ws175c_audio_controls_t* out) {
  if (out == NULL) return H2_AUDIO_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  if (!s_audio_system_initialized || s_audio_system.es8311 == NULL ||
      s_audio_system.es7210 == NULL || s_audio_system.mic_task != NULL ||
      s_audio_system.mic_started || s_audio_system.playback_task != NULL ||
      s_audio_system.playback_started)
    return H2_AUDIO_ERR_INVALID_STATE;
  const uint8_t dac = 0x32;
  esp_err_t rc = i2c_master_transmit_receive(s_audio_system.es8311, &dac, 1,
                                             &out->dac_register, 1, 100);
  for (unsigned i = 0; rc == ESP_OK && i < 4; ++i) {
    uint8_t reg = (uint8_t)(0x43 + i);
    rc = i2c_master_transmit_receive(s_audio_system.es7210, &reg, 1,
                                     &out->adc_gain_registers[i], 1, 100);
  }
  return rc == ESP_OK ? H2_AUDIO_OK : H2_AUDIO_ERR_IO;
}

static h2_pal_audio_t* resolve_audio(void* user) {
  (void)user;
  if (!s_audio_system_initialized) {
    if (h2_ws175c_board_audio_power_on() != H2_PAL_OK) {
      ESP_LOGE(TAG, "audio supply init failed");
      return NULL;
    }

    /* I2S RX routing enables input without clearing a retained output
     * enable. Release the ADC data line after a software reboot. */
    if (gpio_set_direction(H2_WS175C_AUDIO_DIN_GPIO, GPIO_MODE_INPUT) !=
        ESP_OK) {
      ESP_LOGE(TAG, "audio data input release failed");
      return NULL;
    }
    i2c_master_bus_handle_t i2c_bus = h2_ws175c_board_i2c_bus();
    if (i2c_bus == NULL) {
      ESP_LOGE(TAG, "shared I2C bus unavailable");
      return NULL;
    }

    const h2_esp_es8311_es7210_audio_system_config_t config = {
        .sample_rate_hz = H2_WS175C_AUDIO_SAMPLE_RATE,
        .frame_samples_per_channel = H2_WS175C_AUDIO_FRAME_SAMPLES,
        .raw_channels = H2_WS175C_AUDIO_RAW_CHANNELS,
        .processed_channels = H2_WS175C_AUDIO_PROCESSED_CHANNELS,
        .mic_channel_count =
            s_probe_microphones != 0u ? s_probe_microphones : 2u,
        /* ES7210 MIC1/MIC2 are microphones, MIC3 is the DAC reference.
         * The shared 32-bit stereo transport exposes them in lanes 1/3/0. */
        .mic_channel_indices = {1u, 3u},
        .ref_channel_index = 0u,
        .es7210_input_mask = 0x07u,
        .es7210_ref_input_index = 2u,
        .aec_reference_gain_milli = 1000u,
        .i2c_port = H2_WS175C_I2C_PORT,
        .i2c_sda_gpio = H2_WS175C_I2C_SDA_GPIO,
        .i2c_scl_gpio = H2_WS175C_I2C_SCL_GPIO,
        .i2c_speed_hz = H2_WS175C_AUDIO_I2C_SPEED_HZ,
        .i2c_bus = i2c_bus,
        .es8311_i2c_addr = H2_WS175C_AUDIO_ES8311_ADDR,
        .es7210_i2c_addr = H2_WS175C_AUDIO_ES7210_ADDR,
        .i2s_port = H2_WS175C_AUDIO_I2S_PORT,
        .mclk_multiple = H2_WS175C_AUDIO_MCLK_MULTIPLE,
        .mclk_gpio = H2_WS175C_AUDIO_MCLK_GPIO,
        .bclk_gpio = H2_WS175C_AUDIO_BCLK_GPIO,
        .ws_gpio = H2_WS175C_AUDIO_WS_GPIO,
        .dout_gpio = H2_WS175C_AUDIO_DOUT_GPIO,
        .din_gpio = H2_WS175C_AUDIO_DIN_GPIO,
        .pa_gpio = H2_WS175C_AUDIO_PA_GPIO,
        .codec_volume_default = H2_WS175C_AUDIO_CODEC_VOLUME_DEFAULT,
        /* Explicit 0..100 percent contract, with a board-owned attenuation
         * curve ending at unity DAC gain. */
        .speaker_volume =
            {
                .point_count = 3u,
                .points = {{1u, 80u}, {50u, 20u}, {100u, 0u}},
            },
        .mic_gain_db = H2_WS175C_AUDIO_MIC_GAIN_DEFAULT_DB,
        .mic_gain_min_db = 0u,
        .mic_gain_max_db = 38u,
        .es7210_input_gain_mask = 0x07u,
        .es7210_input_gain_db = {H2_WS175C_AUDIO_MIC_GAIN_DEFAULT_DB,
                                 H2_WS175C_AUDIO_MIC_GAIN_DEFAULT_DB,
                                 s_probe_microphones != 0u
                                     ? s_probe_reference_gain_db
                                     : H2_WS175C_AUDIO_REFERENCE_GAIN_DB,
                                 0u},
        .max_tracks = H2_WS175C_AUDIO_MAX_TRACKS,
        .track_queue_frames = H2_WS175C_AUDIO_TRACK_QUEUE_FRAMES,
        .mic_queue_frames = H2_WS175C_AUDIO_MIC_QUEUE_FRAMES,
        .mic_task_stack_size = H2_WS175C_AUDIO_MIC_TASK_STACK,
        .mic_task_priority = tskIDLE_PRIORITY + 5u,
        /* Keep continuous direct-AEC DSP off the Wi-Fi/network core. */
        .mic_task_core_id = 1,
        .speaker_task_stack_size = H2_WS175C_AUDIO_SPEAKER_TASK_STACK,
        .speaker_task_priority = tskIDLE_PRIORITY + 4u,
        .speaker_task_core_id = tskNO_AFFINITY,
        .allocator = h2_esp_board_psram_allocator(),
        .queue_api = h2_esp_board_queue_api(),
        .sync_api = h2_esp_board_sync_api(),
        .enable_aec = 1,
    };
    int rc = h2_esp_es8311_es7210_audio_system_init(&s_audio_system, &config);
    if (rc != H2_AUDIO_OK) {
      ESP_LOGE(TAG, "es8311/es7210 audio system init failed rc=%d", rc);
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

h2_pal_audio_t* h2_esp_board_audio_if_initialized(void) {
  if (!s_audio_system_initialized) {
    return NULL;
  }
  return h2_esp_es8311_es7210_audio_system_audio(&s_audio_system);
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

int h2_ws175c_audio_probe_raw(uint32_t duration_ms,
                              h2_ws175c_audio_raw_stats_t* out_stats) {
  if (out_stats == NULL || duration_ms < 100u || duration_ms > 10000u)
    return H2_AUDIO_ERR_INVALID_ARG;
  memset(out_stats, 0, sizeof(*out_stats));
  if (s_audio_system_initialized &&
      (s_audio_system.mic_task != NULL || s_audio_system.mic_started ||
       s_audio_system.playback_task != NULL || s_audio_system.playback_started))
    return H2_AUDIO_ERR_INVALID_STATE;
  h2_pal_audio_t* audio = resolve_audio(NULL);
  if (audio == NULL) return H2_AUDIO_ERR_INVALID_STATE;
  int rc = h2_pal_audio_start_speaker(audio);
  if (rc != H2_AUDIO_OK) return rc;
  (void)gpio_dump_io_configuration(stdout,
                                   (1ULL << H2_WS175C_AUDIO_MCLK_GPIO) |
                                       (1ULL << H2_WS175C_AUDIO_BCLK_GPIO) |
                                       (1ULL << H2_WS175C_AUDIO_WS_GPIO) |
                                       (1ULL << H2_WS175C_AUDIO_DOUT_GPIO) |
                                       (1ULL << H2_WS175C_AUDIO_DIN_GPIO));
  const uint8_t regs[] = {0x00, 0x01, 0x02, 0x06, 0x07, 0x08, 0x11,
                          0x12, 0x40, 0x43, 0x44, 0x45, 0x46, 0x47,
                          0x48, 0x49, 0x4a, 0x4b, 0x4c};
  for (size_t i = 0u; i < sizeof(regs); ++i) {
    uint8_t value = 0u;
    const esp_err_t read = i2c_master_transmit_receive(
        s_audio_system.es7210, &regs[i], 1u, &value, 1u, 100);
    printf("H2_WS175C_ADC_REG reg=%02x value=%02x rc=%d\n", (unsigned)regs[i],
           (unsigned)value, (int)read);
  }
  int16_t samples[1024];
  const int64_t end = esp_timer_get_time() + (int64_t)duration_ms * 1000;
  while (rc == H2_AUDIO_OK && esp_timer_get_time() < end) {
    size_t bytes = 0u;
    esp_err_t err = i2s_channel_read(s_audio_system.rx_chan, samples,
                                     sizeof(samples), &bytes, 1000);
    if (err != ESP_OK || bytes == 0u || bytes % (4u * sizeof(int16_t)) != 0u) {
      rc = H2_AUDIO_ERR_IO;
      break;
    }
    for (size_t i = 0u; i < bytes / sizeof(*samples); ++i) {
      const int32_t sample = samples[i];
      const uint32_t magnitude = (uint32_t)(sample < 0 ? -sample : sample);
      const size_t lane = i % 4u;
      if (magnitude > out_stats->peak[lane]) out_stats->peak[lane] = magnitude;
      out_stats->energy[lane] += (uint64_t)magnitude * magnitude;
    }
    out_stats->samples_per_lane += bytes / (4u * sizeof(int16_t));
    for (unsigned i = 0u; i < 256u; ++i) {
      out_stats->data_high_samples +=
          gpio_get_level(H2_WS175C_AUDIO_DIN_GPIO) != 0;
      ++out_stats->data_level_samples;
    }
  }
  const int stop = h2_pal_audio_stop_speaker(audio);
  return rc != H2_AUDIO_OK ? rc : stop;
}
