#include "h2_esp_board_private.h"
#include "h2_esp_board_config.h"
#include "h2_esp_tiga_board_internal.h"

#include "h2_esp_lazy_audio.h"

#include "h2_esp_es8311_audio_system.h"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdlib.h>
#include <string.h>

#define H2_TIGA_ESP_AUDIO_SAMPLE_RATE 16000u
#define H2_TIGA_ESP_AUDIO_MCLK_MULTIPLE 384u
#define H2_TIGA_ESP_AUDIO_FRAME_SAMPLES 512u
#define H2_TIGA_ESP_AUDIO_RAW_CHANNELS 2u
#define H2_TIGA_ESP_AUDIO_PROCESSED_CHANNELS 1u
#define H2_TIGA_ESP_AUDIO_I2C_PORT 0
#define H2_TIGA_ESP_AUDIO_I2C_SDA_GPIO GPIO_NUM_2
#define H2_TIGA_ESP_AUDIO_I2C_SCL_GPIO GPIO_NUM_1
#define H2_TIGA_ESP_AUDIO_I2S_PORT 0
#define H2_TIGA_ESP_AUDIO_I2S_DMA_DESC_NUM 4u
#define H2_TIGA_ESP_AUDIO_I2S_DMA_FRAME_NUM 240u
#define H2_TIGA_ESP_AUDIO_MCLK_GPIO GPIO_NUM_47
#define H2_TIGA_ESP_AUDIO_BCLK_GPIO GPIO_NUM_48
#define H2_TIGA_ESP_AUDIO_WS_GPIO GPIO_NUM_19
#define H2_TIGA_ESP_AUDIO_DOUT_GPIO GPIO_NUM_16
#define H2_TIGA_ESP_AUDIO_DIN_GPIO GPIO_NUM_20
/* Leave analog and digital headroom before AEC; validate with enclosed
 * hardware. */
#define H2_TIGA_ESP_AUDIO_CODEC_VOLUME_DEFAULT 0xaau
#define H2_TIGA_ESP_AUDIO_ADC_DIGITAL_VOLUME 0xc5u
#define H2_TIGA_ESP_AUDIO_MIC_GAIN_DEFAULT_DB 21u
#define H2_TIGA_ESP_AUDIO_MAX_TRACKS 4u
#define H2_TIGA_ESP_AUDIO_TRACK_QUEUE_FRAMES 4u
#define H2_TIGA_ESP_AUDIO_MIC_QUEUE_FRAMES 16u
#define H2_TIGA_ESP_AUDIO_MIC_TASK_STACK 6144u
#define H2_TIGA_ESP_AUDIO_SPEAKER_TASK_STACK 4096u
#define H2_TIGA_ESP_AUDIO_MEDIA_CORE 1

static const char* TAG = "h2_tiga_esp_audio";

/* Task-owned PCM staging; the I2S driver allocates its separate Internal DMA
 * rings. */
static EXT_RAM_BSS_ATTR h2_esp_es8311_audio_system_t s_audio_system;
static int s_audio_system_initialized;
static h2_esp_lazy_audio_t s_lazy_audio;
static int s_lazy_audio_initialized;

static h2_pal_audio_t* resolve_audio(void* user) {
  (void)user;
  if (h2_esp_tiga_board_modem_atomics_prepare() != H2_PAL_OK) {
    ESP_LOGE(TAG, "audio route atomic initialization failed");
    return NULL;
  }
  if (!s_audio_system_initialized) {
    if (h2_esp_tiga_board_enable_peripheral_power() != H2_PAL_OK) {
      ESP_LOGE(TAG, "peripheral power enable failed");
      return NULL;
    }
    const h2_esp_es8311_audio_system_config_t config = {
        .sample_rate_hz = H2_TIGA_ESP_AUDIO_SAMPLE_RATE,
        .frame_samples_per_channel = H2_TIGA_ESP_AUDIO_FRAME_SAMPLES,
        .raw_channels = H2_TIGA_ESP_AUDIO_RAW_CHANNELS,
        .processed_channels = H2_TIGA_ESP_AUDIO_PROCESSED_CHANNELS,
        .mic_channel_index = 0u,
        .ref_channel_index = 1u,
        .i2c_port = H2_TIGA_ESP_AUDIO_I2C_PORT,
        .i2c_sda_gpio = H2_TIGA_ESP_AUDIO_I2C_SDA_GPIO,
        .i2c_scl_gpio = H2_TIGA_ESP_AUDIO_I2C_SCL_GPIO,
        .i2c_speed_hz = H2_TIGA_ESP_PERIPHERAL_I2C_SPEED_HZ,
        .codec_i2c_addr = H2_TIGA_ESP_AUDIO_ES8311_ADDR,
        .i2c_bus = h2_esp_tiga_board_i2c_bus(),
        .i2s_port = H2_TIGA_ESP_AUDIO_I2S_PORT,
        .i2s_dma_desc_num = H2_TIGA_ESP_AUDIO_I2S_DMA_DESC_NUM,
        .i2s_dma_frame_num = H2_TIGA_ESP_AUDIO_I2S_DMA_FRAME_NUM,
        .mclk_multiple = H2_TIGA_ESP_AUDIO_MCLK_MULTIPLE,
        .mclk_gpio = H2_TIGA_ESP_AUDIO_MCLK_GPIO,
        .bclk_gpio = H2_TIGA_ESP_AUDIO_BCLK_GPIO,
        .ws_gpio = H2_TIGA_ESP_AUDIO_WS_GPIO,
        .dout_gpio = H2_TIGA_ESP_AUDIO_DOUT_GPIO,
        .din_gpio = H2_TIGA_ESP_AUDIO_DIN_GPIO,
        .pa_gpio = -1,
        .pa_set = h2_esp_tiga_board_codec_pa_set,
        .codec_volume_default = H2_TIGA_ESP_AUDIO_CODEC_VOLUME_DEFAULT,
        .adc_digital_volume = H2_TIGA_ESP_AUDIO_ADC_DIGITAL_VOLUME,
        .mic_gain_db = H2_TIGA_ESP_AUDIO_MIC_GAIN_DEFAULT_DB,
        .mic_gain_min_db = 0u,
        .mic_gain_max_db = 30u,
        .max_tracks = H2_TIGA_ESP_AUDIO_MAX_TRACKS,
        .track_queue_frames = H2_TIGA_ESP_AUDIO_TRACK_QUEUE_FRAMES,
        .mic_queue_frames = H2_TIGA_ESP_AUDIO_MIC_QUEUE_FRAMES,
        .mic_task_stack_size = H2_TIGA_ESP_AUDIO_MIC_TASK_STACK,
        .mic_task_priority = tskIDLE_PRIORITY + 17u,
        .mic_task_core_id = H2_TIGA_ESP_AUDIO_MEDIA_CORE,
        .speaker_task_stack_size = H2_TIGA_ESP_AUDIO_SPEAKER_TASK_STACK,
        .speaker_task_priority = tskIDLE_PRIORITY + 17u,
        .speaker_task_core_id = H2_TIGA_ESP_AUDIO_MEDIA_CORE,
        .allocator = h2_esp_board_psram_allocator(),
        .queue_api = h2_esp_board_queue_api(),
        .sync_api = h2_esp_board_sync_api(),
        .enable_aec = 1,
        /* Relative attenuation preserves the board's maximum DAC gain. */
        .speaker_volume =
            {
                .point_count = 5u,
                .points =
                    {
                        {.percent = 1u, .attenuation_half_db = 120u},
                        {.percent = 25u, .attenuation_half_db = 36u},
                        {.percent = 50u, .attenuation_half_db = 12u},
                        {.percent = 75u, .attenuation_half_db = 5u},
                        {.percent = 100u, .attenuation_half_db = 0u},
                    },
            },
    };
    gpio_config_t audio_route = {
        .pin_bit_mask = 1ULL << H2_TIGA_ESP_AUDIO_MODEM_ROUTE_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&audio_route) != ESP_OK ||
        gpio_set_level((gpio_num_t)H2_TIGA_ESP_AUDIO_MODEM_ROUTE_GPIO,
                       H2_TIGA_ESP_AUDIO_MODEM_ROUTE_CODEC_LEVEL) != ESP_OK) {
      ESP_LOGE(TAG, "audio route init failed");
      return NULL;
    }
    int rc = h2_esp_es8311_audio_system_init(&s_audio_system, &config);
    if (rc != H2_AUDIO_OK) {
      ESP_LOGE(TAG, "es8311 audio system init failed rc=%d", rc);
      return NULL;
    }
    s_audio_system_initialized = 1;
  }
  return h2_esp_es8311_audio_system_audio(&s_audio_system);
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
  return h2_esp_es8311_audio_system_audio(&s_audio_system);
}

h2_pal_result_t h2_esp_board_audio_deinit(void) {
  if (s_audio_system_initialized) {
    const int rc = h2_esp_es8311_audio_system_deinit(&s_audio_system);
    if (rc != H2_AUDIO_OK) {
      return rc;
    }
    s_audio_system_initialized = 0;
  }
  memset(&s_lazy_audio, 0, sizeof(s_lazy_audio));
  s_lazy_audio_initialized = 0;
  return H2_PAL_OK;
}

h2_pal_result_t h2_esp_board_audio_power_down(void) {
  if (s_audio_system_initialized) {
    const int rc = h2_esp_es8311_audio_system_power_down(&s_audio_system);
    if (rc != H2_AUDIO_OK) {
      return rc;
    }
    s_audio_system_initialized = 0;
  }
  /* A failed sleep request can return while Runtime still borrows this API.
   * Keep the dispatch callable so a later consumer can recreate the backend.
   * Only full deinit, after consumers have exited, invalidates the API. */
  return H2_PAL_OK;
}

h2_pal_result_t h2_esp_board_prepare_audio(void) {
  if (resolve_audio(NULL) == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  ESP_LOGI(
      TAG,
      "audio DMA before internal_free=%u internal_largest=%u dma_free=%u "
      "dma_largest=%u psram_free=%u",
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                 MALLOC_CAP_8BIT),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_8BIT),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA |
                                                 MALLOC_CAP_8BIT),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  int rc = h2_esp_es8311_audio_system_prepare(&s_audio_system);
  ESP_LOGI(
      TAG,
      "audio DMA after rc=%d internal_free=%u internal_largest=%u "
      "dma_free=%u dma_largest=%u psram_free=%u",
      rc,
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                 MALLOC_CAP_8BIT),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_8BIT),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA |
                                                 MALLOC_CAP_8BIT),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (rc == H2_AUDIO_OK) {
    return H2_PAL_OK;
  }
  return rc == H2_AUDIO_ERR_NO_MEMORY ? H2_PAL_ERR_NO_MEMORY : H2_PAL_ERR_IO;
}
