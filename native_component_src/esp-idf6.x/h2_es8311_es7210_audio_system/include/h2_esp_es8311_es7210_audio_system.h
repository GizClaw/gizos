#ifndef H2_ESP_ES8311_ES7210_AUDIO_SYSTEM_H
#define H2_ESP_ES8311_ES7210_AUDIO_SYSTEM_H

#include "h2_audio_mixer.h"
#include "h2/pal/hal/h2_pal_audio.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/os/h2_pal_queue.h"

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_ESP_ES8311_ES7210_AUDIO_SYSTEM_MAX_FRAME_SAMPLES 512u
#define H2_ESP_ES8311_ES7210_AUDIO_SYSTEM_MAX_RAW_CHANNELS 4u
#define H2_ESP_ES8311_ES7210_AUDIO_SYSTEM_MAX_MIC_CHANNELS 2u
#define H2_ESP_ES8311_ES7210_AUDIO_SYSTEM_ES7210_INPUT_COUNT 4u
#define H2_ESP_ES8311_ES7210_AUDIO_SYSTEM_MAX_RAW_SAMPLES \
    (H2_ESP_ES8311_ES7210_AUDIO_SYSTEM_MAX_FRAME_SAMPLES * H2_ESP_ES8311_ES7210_AUDIO_SYSTEM_MAX_RAW_CHANNELS)

typedef int (*h2_esp_es8311_es7210_set_pa_fn)(void *user, int enabled);

typedef struct h2_esp_es8311_es7210_mic_queue_frame {
    size_t bytes;
    uint16_t samples_per_channel;
    int16_t samples[H2_ESP_ES8311_ES7210_AUDIO_SYSTEM_MAX_FRAME_SAMPLES];
} h2_esp_es8311_es7210_mic_queue_frame_t;

typedef struct h2_esp_es8311_es7210_audio_system_config {
    uint32_t sample_rate_hz;
    uint16_t frame_samples_per_channel;
    uint8_t raw_channels;
    uint8_t processed_channels;
    uint8_t mic_channel_count;
    uint8_t mic_channel_indices[H2_ESP_ES8311_ES7210_AUDIO_SYSTEM_MAX_MIC_CHANNELS];
    uint8_t ref_channel_index;
    uint8_t es7210_input_mask;
    uint8_t es7210_ref_input_index;
    uint32_t aec_reference_gain_milli;
    int i2c_port;
    int i2c_sda_gpio;
    int i2c_scl_gpio;
    uint32_t i2c_speed_hz;
    i2c_master_bus_handle_t i2c_bus;
    uint8_t es8311_i2c_addr;
    uint8_t es7210_i2c_addr;
    int i2s_port;
    uint16_t mclk_multiple;
    int mclk_gpio;
    int bclk_gpio;
    int ws_gpio;
    int dout_gpio;
    int din_gpio;
    int pa_gpio;
    h2_esp_es8311_es7210_set_pa_fn set_pa;
    void *set_pa_user;
    uint8_t codec_volume_default;
    uint32_t mic_gain_db;
    uint8_t es7210_input_gain_mask;
    uint8_t es7210_input_gain_db[H2_ESP_ES8311_ES7210_AUDIO_SYSTEM_ES7210_INPUT_COUNT];
    uint8_t max_tracks;
    uint8_t track_queue_frames;
    uint8_t mic_queue_frames;
    uint32_t mic_task_stack_size;
    UBaseType_t mic_task_priority;
    BaseType_t mic_task_core_id;
    uint32_t speaker_task_stack_size;
    UBaseType_t speaker_task_priority;
    BaseType_t speaker_task_core_id;
    const h2_pal_mem_api_t *allocator;
    const h2_pal_queue_api_t *queue_api;
    const h2_pal_sync_api_t *sync_api;
    int enable_aec;
} h2_esp_es8311_es7210_audio_system_config_t;

typedef struct h2_esp_es8311_es7210_sr_state {
    int initialized;
    int available;
    uint32_t sample_rate_hz;
    size_t frame_samples;
    uint8_t raw_channels;
    uint8_t mic_channel_count;
    uint8_t mic_channel_indices[H2_ESP_ES8311_ES7210_AUDIO_SYSTEM_MAX_MIC_CHANNELS];
    uint8_t ref_channel_index;
    uint32_t reference_gain_milli;
    void *aec_handle;
    int16_t *mic_frame;
    int16_t *ref_frame;
    int16_t *out_frame;
    uint32_t processed_frame_count;
} h2_esp_es8311_es7210_sr_state_t;

typedef struct h2_esp_es8311_es7210_audio_system {
    h2_esp_es8311_es7210_audio_system_config_t config;
    h2_esp_es8311_es7210_sr_state_t sr;
    int pa_initialized;
    int opened;
    int sr_initialized;
    volatile int mic_started;
    int mic_task_started;
    int mic_task_with_caps;
    int mic_queue_initialized;
    int playback_started;
    int mixer_initialized;
    int playback_task_started;
    int playback_task_with_caps;
    uint32_t speaker_volume_percent;
    i2c_master_bus_handle_t i2c_bus;
    i2c_master_dev_handle_t es8311;
    i2c_master_dev_handle_t es7210;
    int owns_i2c_bus;
    i2s_chan_handle_t tx_chan;
    i2s_chan_handle_t rx_chan;
    SemaphoreHandle_t write_mutex;
    TaskHandle_t mic_task;
    TaskHandle_t playback_task;
    h2_pal_queue_t *mic_queue;
    h2_audio_mixer_t mixer;
    int16_t mic_raw_scratch[H2_ESP_ES8311_ES7210_AUDIO_SYSTEM_MAX_RAW_SAMPLES];
    int16_t mic_processed_scratch[H2_ESP_ES8311_ES7210_AUDIO_SYSTEM_MAX_FRAME_SAMPLES];
    int16_t playback_scratch[H2_ESP_ES8311_ES7210_AUDIO_SYSTEM_MAX_FRAME_SAMPLES];
    int32_t stereo_scratch[H2_ESP_ES8311_ES7210_AUDIO_SYSTEM_MAX_FRAME_SAMPLES * 2u];
    h2_pal_audio_t audio;
} h2_esp_es8311_es7210_audio_system_t;

int h2_esp_es8311_es7210_audio_system_init(
    h2_esp_es8311_es7210_audio_system_t *system,
    const h2_esp_es8311_es7210_audio_system_config_t *config);

/**
 * Stop all workers and release every resource owned by an initialized system.
 *
 * The caller owns `system` storage and must ensure that no consumer uses the
 * returned Audio PAL after this call. The operation is idempotent. If a worker
 * cannot stop within its bounded deadline, the function returns an Audio error
 * and preserves the remaining resources so the caller can retry safely.
 */
int h2_esp_es8311_es7210_audio_system_deinit(
    h2_esp_es8311_es7210_audio_system_t *system);

h2_pal_audio_t *h2_esp_es8311_es7210_audio_system_audio(h2_esp_es8311_es7210_audio_system_t *system);

int h2_esp_es8311_es7210_audio_system_process_mic(
    h2_esp_es8311_es7210_audio_system_t *system,
    const h2_audio_frame_t *raw_frame,
    h2_audio_frame_t *out_frame,
    uint32_t timeout_ms);

int h2_esp_es8311_es7210_sr_init(
    h2_esp_es8311_es7210_sr_state_t *state,
    const h2_esp_es8311_es7210_audio_system_config_t *config);

int h2_esp_es8311_es7210_sr_process(
    h2_esp_es8311_es7210_sr_state_t *state,
    const h2_audio_frame_t *raw_frame,
    h2_audio_frame_t *out_frame,
    uint32_t timeout_ms);

void h2_esp_es8311_es7210_sr_reset(h2_esp_es8311_es7210_sr_state_t *state);
void h2_esp_es8311_es7210_sr_deinit(h2_esp_es8311_es7210_sr_state_t *state);

#ifdef __cplusplus
}
#endif

#endif
