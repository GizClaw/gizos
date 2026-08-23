#include "h2_esp_board_private.h"

#include "h2_esp_lazy_audio.h"

#include "h2_esp_es8311_es7210_audio_system.h"
#include "h2_esp_szp_board_internal.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdlib.h>
#include <string.h>

#define H2_SZP_AUDIO_SAMPLE_RATE 16000u
#define H2_SZP_AUDIO_MCLK_MULTIPLE 384u
#define H2_SZP_AUDIO_FRAME_SAMPLES 256u
#define H2_SZP_AUDIO_RAW_CHANNELS 4u
#define H2_SZP_AUDIO_PROCESSED_CHANNELS 1u
#define H2_SZP_AUDIO_I2C_PORT 0
#define H2_SZP_AUDIO_I2C_SDA_GPIO 1
#define H2_SZP_AUDIO_I2C_SCL_GPIO 2
#define H2_SZP_AUDIO_I2C_SPEED_HZ 100000u
#define H2_SZP_AUDIO_ES8311_ADDR 0x18
#define H2_SZP_AUDIO_ES7210_ADDR 0x41
#define H2_SZP_AUDIO_I2S_PORT 1
#define H2_SZP_AUDIO_MCLK_GPIO 38
#define H2_SZP_AUDIO_BCLK_GPIO 14
#define H2_SZP_AUDIO_WS_GPIO 13
#define H2_SZP_AUDIO_DOUT_GPIO 45
#define H2_SZP_AUDIO_DIN_GPIO 12
#define H2_SZP_AUDIO_CODEC_VOLUME_DEFAULT 0xb0u
#define H2_SZP_AUDIO_MIC_GAIN_DEFAULT_DB 24u
#define H2_SZP_AUDIO_MAX_TRACKS 4u
#define H2_SZP_AUDIO_TRACK_QUEUE_FRAMES 4u
#define H2_SZP_AUDIO_MIC_QUEUE_FRAMES 4u
#define H2_SZP_AUDIO_MIC_TASK_STACK 6144u
#define H2_SZP_AUDIO_SPEAKER_TASK_STACK 4096u

static const char *TAG = "h2_szp_audio";
static h2_esp_es8311_es7210_audio_system_t s_audio_system;
static int s_audio_system_initialized;
static h2_esp_lazy_audio_t s_lazy_audio;
static int s_lazy_audio_initialized;

static int set_pa(void *user, int enabled) {
    (void)user;
    return h2_esp_szp_board_set_pa(enabled);
}

static h2_pal_audio_t *resolve_audio(void *user) {
    (void)user;
    if (!s_audio_system_initialized) {
        if (h2_esp_szp_board_init_io() != 0) {
            ESP_LOGE(TAG, "szp io init failed");
            return NULL;
        }

        i2c_master_bus_handle_t i2c_bus = h2_esp_szp_board_i2c_bus();
        if (i2c_bus == NULL) {
            ESP_LOGE(TAG, "szp i2c bus unavailable");
            return NULL;
        }

        const h2_esp_es8311_es7210_audio_system_config_t config = {
            .sample_rate_hz = H2_SZP_AUDIO_SAMPLE_RATE,
            .frame_samples_per_channel = H2_SZP_AUDIO_FRAME_SAMPLES,
            .raw_channels = H2_SZP_AUDIO_RAW_CHANNELS,
            .processed_channels = H2_SZP_AUDIO_PROCESSED_CHANNELS,
            .mic_channel_count = 2u,
            .mic_channel_indices = {1u, 3u},
            .ref_channel_index = 0u,
            .es7210_input_mask = 0x0fu,
            .es7210_ref_input_index = 2u,
            .aec_reference_gain_milli = 1000u,
            .i2c_port = H2_SZP_AUDIO_I2C_PORT,
            .i2c_sda_gpio = H2_SZP_AUDIO_I2C_SDA_GPIO,
            .i2c_scl_gpio = H2_SZP_AUDIO_I2C_SCL_GPIO,
            .i2c_speed_hz = H2_SZP_AUDIO_I2C_SPEED_HZ,
            .i2c_bus = i2c_bus,
            .es8311_i2c_addr = H2_SZP_AUDIO_ES8311_ADDR,
            .es7210_i2c_addr = H2_SZP_AUDIO_ES7210_ADDR,
            .i2s_port = H2_SZP_AUDIO_I2S_PORT,
            .mclk_multiple = H2_SZP_AUDIO_MCLK_MULTIPLE,
            .mclk_gpio = H2_SZP_AUDIO_MCLK_GPIO,
            .bclk_gpio = H2_SZP_AUDIO_BCLK_GPIO,
            .ws_gpio = H2_SZP_AUDIO_WS_GPIO,
            .dout_gpio = H2_SZP_AUDIO_DOUT_GPIO,
            .din_gpio = H2_SZP_AUDIO_DIN_GPIO,
            .pa_gpio = -1,
            .set_pa = set_pa,
            .codec_volume_default = H2_SZP_AUDIO_CODEC_VOLUME_DEFAULT,
            .mic_gain_db = H2_SZP_AUDIO_MIC_GAIN_DEFAULT_DB,
            .max_tracks = H2_SZP_AUDIO_MAX_TRACKS,
            .track_queue_frames = H2_SZP_AUDIO_TRACK_QUEUE_FRAMES,
            .mic_queue_frames = H2_SZP_AUDIO_MIC_QUEUE_FRAMES,
            .mic_task_stack_size = H2_SZP_AUDIO_MIC_TASK_STACK,
            .mic_task_priority = tskIDLE_PRIORITY + 5u,
            .mic_task_core_id = tskNO_AFFINITY,
            .speaker_task_stack_size = H2_SZP_AUDIO_SPEAKER_TASK_STACK,
            .speaker_task_priority = tskIDLE_PRIORITY + 4u,
            .speaker_task_core_id = tskNO_AFFINITY,
            .allocator = h2_esp_board_default_allocator(),
            .queue_api = h2_esp_board_queue_api(),
            .sync_api = h2_esp_board_sync_api(),
            .enable_aec = 0,
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
    return h2_esp_es8311_es7210_audio_system_audio(&s_audio_system);
}

h2_pal_result_t h2_esp_board_audio_deinit(void) {
    if (s_audio_system_initialized) {
        const int rc = h2_esp_es8311_es7210_audio_system_deinit(
            &s_audio_system);
        if (rc != H2_AUDIO_OK) {
            return rc;
        }
        s_audio_system_initialized = 0;
    }
    memset(&s_lazy_audio, 0, sizeof(s_lazy_audio));
    s_lazy_audio_initialized = 0;
    return H2_PAL_OK;
}
