#include "h2_esp_board_private.h"
#include "h2_esp_board_internal.h"

#include "h2_esp_lazy_audio.h"

#include "h2_esp_es8311_audio_system.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdlib.h>
#include <string.h>

#define H2_AMOLED_AUDIO_SAMPLE_RATE 16000u
#define H2_AMOLED_AUDIO_MCLK_MULTIPLE 384u
#define H2_AMOLED_AUDIO_FRAME_SAMPLES 512u
#define H2_AMOLED_AUDIO_MIC_CHANNELS 2u
#define H2_AMOLED_AUDIO_PROCESSED_CHANNELS 1u
#define H2_AMOLED_AUDIO_I2C_PORT 0
#define H2_AMOLED_AUDIO_I2C_SDA_GPIO GPIO_NUM_15
#define H2_AMOLED_AUDIO_I2C_SCL_GPIO GPIO_NUM_14
#define H2_AMOLED_AUDIO_I2C_SPEED_HZ 100000u
#define H2_AMOLED_AUDIO_ES8311_ADDR 0x18
#define H2_AMOLED_AUDIO_I2S_PORT 0
#define H2_AMOLED_AUDIO_MCLK_GPIO GPIO_NUM_16
#define H2_AMOLED_AUDIO_BCLK_GPIO GPIO_NUM_9
#define H2_AMOLED_AUDIO_WS_GPIO GPIO_NUM_45
#define H2_AMOLED_AUDIO_DOUT_GPIO GPIO_NUM_8
#define H2_AMOLED_AUDIO_DIN_GPIO GPIO_NUM_10
#define H2_AMOLED_AUDIO_PA_GPIO GPIO_NUM_46
#define H2_AMOLED_AUDIO_CODEC_VOLUME_DEFAULT 0xcbu
#define H2_AMOLED_AUDIO_ADC_DIGITAL_VOLUME 0xc8u
#define H2_AMOLED_AUDIO_MIC_GAIN_DEFAULT_DB 18u
#define H2_AMOLED_AUDIO_MAX_TRACKS 4u
#define H2_AMOLED_AUDIO_TRACK_QUEUE_FRAMES 4u
#define H2_AMOLED_AUDIO_MIC_QUEUE_FRAMES 4u
#define H2_AMOLED_AUDIO_MIC_TASK_STACK 6144u
#define H2_AMOLED_AUDIO_SPEAKER_TASK_STACK 4096u

static const char *TAG = "h2_amoled_audio";

static h2_esp_es8311_audio_system_t s_audio_system;
static int s_audio_system_initialized;
static h2_esp_lazy_audio_t s_lazy_audio;
static int s_lazy_audio_initialized;

static h2_pal_audio_t *resolve_audio(void *user) {
    (void)user;
    if (!s_audio_system_initialized) {
        const h2_esp_es8311_audio_system_config_t config = {
            .sample_rate_hz = H2_AMOLED_AUDIO_SAMPLE_RATE,
            .frame_samples_per_channel = H2_AMOLED_AUDIO_FRAME_SAMPLES,
            .raw_channels = H2_AMOLED_AUDIO_MIC_CHANNELS,
            .processed_channels = H2_AMOLED_AUDIO_PROCESSED_CHANNELS,
            .mic_channel_index = 0u,
            .ref_channel_index = 1u,
            .i2c_bus = h2_esp_amoled_board_i2c_bus(),
            .i2c_port = H2_AMOLED_AUDIO_I2C_PORT,
            .i2c_sda_gpio = H2_AMOLED_AUDIO_I2C_SDA_GPIO,
            .i2c_scl_gpio = H2_AMOLED_AUDIO_I2C_SCL_GPIO,
            .i2c_speed_hz = H2_AMOLED_AUDIO_I2C_SPEED_HZ,
            .codec_i2c_addr = H2_AMOLED_AUDIO_ES8311_ADDR,
            .i2s_port = H2_AMOLED_AUDIO_I2S_PORT,
            .mclk_multiple = H2_AMOLED_AUDIO_MCLK_MULTIPLE,
            .mclk_gpio = H2_AMOLED_AUDIO_MCLK_GPIO,
            .bclk_gpio = H2_AMOLED_AUDIO_BCLK_GPIO,
            .ws_gpio = H2_AMOLED_AUDIO_WS_GPIO,
            .dout_gpio = H2_AMOLED_AUDIO_DOUT_GPIO,
            .din_gpio = H2_AMOLED_AUDIO_DIN_GPIO,
            .pa_gpio = H2_AMOLED_AUDIO_PA_GPIO,
            .codec_volume_default = H2_AMOLED_AUDIO_CODEC_VOLUME_DEFAULT,
            .adc_digital_volume = H2_AMOLED_AUDIO_ADC_DIGITAL_VOLUME,
            .mic_gain_db = H2_AMOLED_AUDIO_MIC_GAIN_DEFAULT_DB,
            .max_tracks = H2_AMOLED_AUDIO_MAX_TRACKS,
            .track_queue_frames = H2_AMOLED_AUDIO_TRACK_QUEUE_FRAMES,
            .mic_queue_frames = H2_AMOLED_AUDIO_MIC_QUEUE_FRAMES,
            .mic_task_stack_size = H2_AMOLED_AUDIO_MIC_TASK_STACK,
            .mic_task_priority = tskIDLE_PRIORITY + 5u,
            .mic_task_core_id = tskNO_AFFINITY,
            .speaker_task_stack_size = H2_AMOLED_AUDIO_SPEAKER_TASK_STACK,
            .speaker_task_priority = tskIDLE_PRIORITY + 4u,
            .speaker_task_core_id = tskNO_AFFINITY,
            .allocator = h2_esp_board_default_allocator(),
            .queue_api = h2_esp_board_queue_api(),
            .sync_api = h2_esp_board_sync_api(),
            .enable_aec = 1,
        };
        if (config.i2c_bus == NULL) {
            ESP_LOGE(TAG, "shared i2c bus unavailable");
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

h2_pal_audio_t *h2_esp_board_audio(void) {
    if (!s_lazy_audio_initialized) {
        if (h2_esp_lazy_audio_init(&s_lazy_audio, NULL, resolve_audio) != H2_AUDIO_OK) {
            return NULL;
        }
        s_lazy_audio_initialized = 1;
    }
    return h2_esp_lazy_audio_api(&s_lazy_audio);
}

h2_pal_audio_t *h2_esp_board_audio_if_initialized(void) {
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
