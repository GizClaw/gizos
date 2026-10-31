#include "h2_mosaico_audio.h"
#include "h2_mosaico_audio_config.h"
#include "h2_chip_board.h"
#include "h2_esp_es8311_audio_system.h"
#include "h2_esp_platform_core.h"
#include "bsp/esp_mosaico.h"

static h2_esp_es8311_audio_system_t audio_system;
static bool initialized;
static bool codec_powered;
static bool cleanup_pending;

#ifdef H2_MOSAICO_MFG_DAC_OSR20
/* Mosaico MFG uses the 6.144 MHz / 16 kHz DAC OSR coefficient 0x20.
 * Prepare opens the codec without workers; keep shared defaults unchanged. */
static int configure_mfg_dac_clock(void) {
  int rc = h2_esp_es8311_audio_system_prepare(&audio_system);
  if (rc != H2_AUDIO_OK) return rc;
  uint8_t reg = 0x04, value = 0;
  if (i2c_master_transmit_receive(audio_system.codec, &reg, 1, &value, 1,
                                  100) != ESP_OK)
    return H2_PAL_ERR_IO;
  uint8_t command[] = {reg, (uint8_t)((value & 0x80u) | 0x20u)};
  if (i2c_master_transmit(audio_system.codec, command, sizeof(command), 100) !=
      ESP_OK)
    return H2_PAL_ERR_IO;
  if (i2c_master_transmit_receive(audio_system.codec, &reg, 1, &value, 1,
                                  100) != ESP_OK ||
      value != command[1])
    return H2_PAL_ERR_IO;
  return H2_PAL_OK;
}
#endif

int h2_mosaico_audio_init(void) {
  if (initialized) return H2_PAL_OK;
  if (codec_powered) return H2_PAL_ERR_INVALID_STATE;
  bsp_board_variant_t variant;
  if (bsp_board_variant_get(&variant) != ESP_OK) return H2_PAL_ERR_UNSUPPORTED;
  if (bsp_power_set_vcc_3v3(true) != ESP_OK) return H2_PAL_ERR_IO;
  if (bsp_power_set_codec_3v3(true) != ESP_OK) return H2_PAL_ERR_IO;
  codec_powered = true;
  const h2_esp_es8311_audio_system_config_t config = {
      .sample_rate_hz = 16000,
      .frame_samples_per_channel = 512,
      .raw_channels = 2,
      .processed_channels = 1,
      .mic_channel_index = 0,
      .ref_channel_index = 1,
      .i2c_bus = bsp_i2c_get_handle(),
      .i2c_port = BSP_I2C_PORT,
      .i2c_sda_gpio = variant == BSP_BOARD_VARIANT_V1_0 ? BSP_I2C_SDA_V1_0
                                                        : BSP_I2C_SDA_V1_2,
      .i2c_scl_gpio = variant == BSP_BOARD_VARIANT_V1_0 ? BSP_I2C_SCL_V1_0
                                                        : BSP_I2C_SCL_V1_2,
      .i2c_speed_hz = 400000,
      .codec_i2c_addr = 0x19,
      .i2s_port = 0,
      .mclk_multiple = 384,
      .mclk_gpio = BSP_AUDIO_I2S_MCLK,
      .bclk_gpio = BSP_AUDIO_I2S_SCLK,
      .ws_gpio = BSP_AUDIO_I2S_LRCLK,
      .dout_gpio = BSP_AUDIO_I2S_SDOUT,
      .din_gpio = BSP_AUDIO_I2S_DSIN,
      .pa_gpio = BSP_AUDIO_PA_CTRL,
      .codec_volume_default = H2_MOSAICO_DAC_MAX,
      .speaker_volume = H2_MOSAICO_VOLUME_CURVE,
      .adc_digital_volume = 0xc8,
      .mic_gain_db = 18,
      .mic_gain_max_db = 30,
      .max_tracks = 4,
      .track_queue_frames = 4,
      .mic_queue_frames = 4,
      .mic_task_stack_size = 6144,
      .mic_task_priority = 5,
      .mic_task_core_id = tskNO_AFFINITY,
      .speaker_task_stack_size = 4096,
      .speaker_task_priority = 17,
      .speaker_task_core_id = 1,
      .allocator = h2_esp_platform_default_allocator(),
      .queue_api = h2_esp_platform_queue_api(),
      .sync_api = h2_esp_platform_sync_api(),
  /* Qualification images explicitly opt in; production stays unchanged. */
#ifdef H2_MOSAICO_AUDIO_AEC
      .enable_aec = 1,
#else
      .enable_aec = 0,
#endif
  };
  int rc = h2_esp_es8311_audio_system_init(&audio_system, &config);
#ifdef H2_MOSAICO_MFG_DAC_OSR20
  if (rc == H2_AUDIO_OK) {
    rc = configure_mfg_dac_clock();
    if (rc != H2_PAL_OK &&
        h2_esp_es8311_audio_system_deinit(&audio_system) != H2_AUDIO_OK) {
      cleanup_pending = true;
      return rc;
    }
  }
#endif
  if (rc == H2_AUDIO_OK)
    initialized = true;
  else if (bsp_power_set_codec_3v3(false) == ESP_OK)
    codec_powered = false;
  return rc;
}
int h2_mosaico_audio_deinit(void) {
  if (initialized || cleanup_pending) {
    const int rc = h2_esp_es8311_audio_system_deinit(&audio_system);
    if (rc != H2_AUDIO_OK) return rc;
    initialized = cleanup_pending = false;
  }
  if (codec_powered) {
    if (bsp_power_set_codec_3v3(false) != ESP_OK) return H2_PAL_ERR_IO;
    codec_powered = false;
  }
  return H2_PAL_OK;
}
const h2_pal_audio_api_t* h2_mosaico_audio(void) {
  return initialized ? h2_esp_es8311_audio_system_audio(&audio_system)
                     : h2_pal_unsupported_audio_api();
}
