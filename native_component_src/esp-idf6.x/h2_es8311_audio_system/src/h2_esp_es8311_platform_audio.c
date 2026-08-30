#include "h2_audio_mixer.h"
#include "h2_esp_es8311_audio_system.h"
#include "h2/pal/hal/h2_pal_audio_task_names.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <string.h>

#define ES8311_REG_RESET 0x00
#define ES8311_REG_CLK_MANAGER_01 0x01
#define ES8311_REG_CLK_MANAGER_02 0x02
#define ES8311_REG_CLK_MANAGER_03 0x03
#define ES8311_REG_CLK_MANAGER_04 0x04
#define ES8311_REG_CLK_MANAGER_05 0x05
#define ES8311_REG_CLK_MANAGER_06 0x06
#define ES8311_REG_CLK_MANAGER_07 0x07
#define ES8311_REG_CLK_MANAGER_08 0x08
#define ES8311_REG_SDP_IN 0x09
#define ES8311_REG_SDP_OUT 0x0a
#define ES8311_REG_SYSTEM_0B 0x0b
#define ES8311_REG_SYSTEM_0C 0x0c
#define ES8311_REG_SYSTEM_0D 0x0d
#define ES8311_REG_SYSTEM_0E 0x0e
#define ES8311_REG_SYSTEM_10 0x10
#define ES8311_REG_SYSTEM_11 0x11
#define ES8311_REG_SYSTEM_12 0x12
#define ES8311_REG_SYSTEM_13 0x13
#define ES8311_REG_SYSTEM_14 0x14
#define ES8311_REG_ADC_15 0x15
#define ES8311_REG_ADC_16 0x16
#define ES8311_REG_ADC_17 0x17
#define ES8311_REG_ADC_1B 0x1b
#define ES8311_REG_ADC_1C 0x1c
#define ES8311_REG_DAC_31 0x31
#define ES8311_REG_DAC_32 0x32
#define ES8311_REG_DAC_37 0x37
#define ES8311_REG_GPIO_44 0x44
#define ES8311_REG_GP_45 0x45
#define ES8311_REG_CHIP_ID1 0xfd
#define ES8311_REG_CHIP_ID2 0xfe

#define H2_ESP_ES8311_CONTROL_TIMEOUT_MS 100u
#define H2_ESP_ES8311_IO_TIMEOUT_MS 100u
#define H2_ESP_ES8311_STOP_TIMEOUT_MS 200u
#define H2_ESP_ES8311_RETRY_DELAY_MS 20u

static const char *TAG = "h2_es8311_audio";

static int map_esp_err(esp_err_t err) {
    if (err == ESP_OK) {
        return H2_AUDIO_OK;
    }
    if (err == ESP_ERR_NO_MEM) {
        return H2_AUDIO_ERR_NO_MEMORY;
    }
    if (err == ESP_ERR_INVALID_ARG) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return H2_AUDIO_ERR_INVALID_STATE;
    }
    if (err == ESP_ERR_TIMEOUT) {
        return H2_AUDIO_ERR_WOULD_BLOCK;
    }
    return H2_AUDIO_ERR_IO;
}

static TickType_t es8311_audio_stop_ticks_remaining(TickType_t started_at) {
    const TickType_t budget = pdMS_TO_TICKS(H2_ESP_ES8311_STOP_TIMEOUT_MS);
    const TickType_t elapsed = xTaskGetTickCount() - started_at;
    return elapsed < budget ? budget - elapsed : 0u;
}

static TickType_t es8311_audio_control_wait_ticks(TickType_t remaining) {
    const TickType_t control_timeout = pdMS_TO_TICKS(H2_ESP_ES8311_CONTROL_TIMEOUT_MS);
    return remaining < control_timeout ? remaining : control_timeout;
}

static h2_audio_pcm_format_t es8311_audio_mic_format(const h2_esp_es8311_audio_system_t *state) {
    h2_audio_pcm_format_t format = {
        .sample_rate_hz = state->config.sample_rate_hz,
        .frame_samples_per_channel = state->config.frame_samples_per_channel,
        .channels = state->config.processed_channels,
        .sample_format = H2_AUDIO_SAMPLE_S16LE,
    };
    return format;
}

static h2_audio_pcm_format_t es8311_audio_raw_mic_format(const h2_esp_es8311_audio_system_t *state) {
    h2_audio_pcm_format_t format = {
        .sample_rate_hz = state->config.sample_rate_hz,
        .frame_samples_per_channel = state->config.frame_samples_per_channel,
        .channels = state->config.raw_channels,
        .sample_format = H2_AUDIO_SAMPLE_S16LE,
    };
    return format;
}

static h2_audio_pcm_format_t es8311_audio_playback_format(const h2_esp_es8311_audio_system_t *state) {
    h2_audio_pcm_format_t format = {
        .sample_rate_hz = state->config.sample_rate_hz,
        .frame_samples_per_channel = state->config.frame_samples_per_channel,
        .channels = 1u,
        .sample_format = H2_AUDIO_SAMPLE_S16LE,
    };
    return format;
}

static int es8311_audio_init_pa(h2_esp_es8311_audio_system_t *state) {
    if (state->pa_initialized) {
        return H2_AUDIO_OK;
    }
    if (state->config.pa_set != NULL) {
        const int rc = state->config.pa_set(state->config.pa_user, 0);
        if (rc != H2_AUDIO_OK) {
            return rc;
        }
        state->pa_initialized = 1;
        return H2_AUDIO_OK;
    }
    if (state->config.pa_gpio < 0) {
        state->pa_initialized = 1;
        return H2_AUDIO_OK;
    }
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << (uint32_t)state->config.pa_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    int rc = map_esp_err(gpio_config(&cfg));
    if (rc != H2_AUDIO_OK) {
        return rc;
    }
    rc = map_esp_err(gpio_set_level((gpio_num_t)state->config.pa_gpio, 0));
    if (rc != H2_AUDIO_OK) {
        return rc;
    }
    state->pa_initialized = 1;
    return H2_AUDIO_OK;
}

static int es8311_audio_set_pa(h2_esp_es8311_audio_system_t *state, int enabled) {
    if (state->config.pa_set != NULL) {
        return state->config.pa_set(state->config.pa_user, enabled);
    }
    if (state->config.pa_gpio < 0) {
        return H2_AUDIO_OK;
    }
    return map_esp_err(gpio_set_level((gpio_num_t)state->config.pa_gpio, enabled ? 1 : 0));
}

static esp_err_t es8311_write_reg(h2_esp_es8311_audio_system_t *state, uint8_t reg, uint8_t value) {
    const uint8_t data[2] = { reg, value };
    return i2c_master_transmit(state->codec, data, sizeof(data), pdMS_TO_TICKS(100));
}

static esp_err_t es8311_read_reg(h2_esp_es8311_audio_system_t *state, uint8_t reg, uint8_t *value) {
    return i2c_master_transmit_receive(state->codec, &reg, sizeof(reg), value, sizeof(*value), pdMS_TO_TICKS(100));
}

static esp_err_t es8311_update_reg(h2_esp_es8311_audio_system_t *state, uint8_t reg, uint8_t mask, uint8_t value) {
    uint8_t regv = 0;
    ESP_RETURN_ON_ERROR(es8311_read_reg(state, reg, &regv), TAG, "es8311 read 0x%02x", reg);
    regv = (uint8_t)((regv & (uint8_t)~mask) | (value & mask));
    return es8311_write_reg(state, reg, regv);
}

static uint8_t es8311_mic_gain_from_db(uint32_t db) {
    if (db < 6u) {
        return 0u;
    }
    if (db < 12u) {
        return 1u;
    }
    if (db < 18u) {
        return 2u;
    }
    if (db < 24u) {
        return 3u;
    }
    if (db < 30u) {
        return 4u;
    }
    if (db < 36u) {
        return 5u;
    }
    if (db < 42u) {
        return 6u;
    }
    return 7u;
}

static uint8_t es8311_volume_from_percent_with_default(uint32_t percent, uint8_t codec_volume_default) {
    const uint32_t scaled = (percent * (uint32_t)codec_volume_default) / 100u;
    if (scaled > 0xffu) {
        return 0xffu;
    }
    return (uint8_t)scaled;
}

static esp_err_t es8311_set_sample_rate_16k(h2_esp_es8311_audio_system_t *state) {
    if (state->config.sample_rate_hz != 16000u || state->config.mclk_multiple != 384u) {
        ESP_LOGE(TAG, "unsupported es8311 clock sample_rate=%u mclk_multiple=%u",
            (unsigned)state->config.sample_rate_hz,
            (unsigned)state->config.mclk_multiple);
        return ESP_ERR_INVALID_ARG;
    }

    /* Match the Waveshare AMOLED 1.8 ES8311 example:
       sample_rate=16kHz, mclk_multiple=384, mclk=6.144MHz. */
    uint8_t regv = 0;
    ESP_RETURN_ON_ERROR(es8311_read_reg(state, ES8311_REG_CLK_MANAGER_02, &regv), TAG, "clk2");
    regv &= 0x07u;
    regv |= 0x48u;
    ESP_RETURN_ON_ERROR(es8311_write_reg(state, ES8311_REG_CLK_MANAGER_02, regv), TAG, "clk2");
    ESP_RETURN_ON_ERROR(es8311_write_reg(state, ES8311_REG_CLK_MANAGER_05, 0x00), TAG, "clk5");
    ESP_RETURN_ON_ERROR(es8311_update_reg(state, ES8311_REG_CLK_MANAGER_03, 0x7fu, 0x10), TAG, "clk3");
    ESP_RETURN_ON_ERROR(es8311_update_reg(state, ES8311_REG_CLK_MANAGER_04, 0x7fu, 0x10), TAG, "clk4");
    ESP_RETURN_ON_ERROR(es8311_update_reg(state, ES8311_REG_CLK_MANAGER_07, 0x3fu, 0x00), TAG, "clk7");
    ESP_RETURN_ON_ERROR(es8311_write_reg(state, ES8311_REG_CLK_MANAGER_08, 0xff), TAG, "clk8");
    ESP_RETURN_ON_ERROR(es8311_update_reg(state, ES8311_REG_CLK_MANAGER_06, 0x1fu, 0x03), TAG, "clk6");
    return ESP_OK;
}

static esp_err_t es8311_open(h2_esp_es8311_audio_system_t *state) {
    esp_err_t err = es8311_write_reg(state, ES8311_REG_GPIO_44, 0x08);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "initial gpio44 write failed (%s), retrying", esp_err_to_name(err));
    }
    ESP_RETURN_ON_ERROR(es8311_write_reg(state, ES8311_REG_GPIO_44, 0x08), TAG, "gpio44");
    ESP_RETURN_ON_ERROR(es8311_write_reg(state, ES8311_REG_CLK_MANAGER_01, 0x30), TAG, "clk1");
    ESP_RETURN_ON_ERROR(es8311_write_reg(state, ES8311_REG_CLK_MANAGER_02, 0x00), TAG, "clk2");
    ESP_RETURN_ON_ERROR(es8311_write_reg(state, ES8311_REG_CLK_MANAGER_03, 0x10), TAG, "clk3");
    ESP_RETURN_ON_ERROR(es8311_write_reg(state, ES8311_REG_ADC_16, 0x24), TAG, "adc16");
    ESP_RETURN_ON_ERROR(es8311_write_reg(state, ES8311_REG_CLK_MANAGER_04, 0x10), TAG, "clk4");
    ESP_RETURN_ON_ERROR(es8311_write_reg(state, ES8311_REG_CLK_MANAGER_05, 0x00), TAG, "clk5");
    ESP_RETURN_ON_ERROR(es8311_write_reg(state, ES8311_REG_SYSTEM_0B, 0x00), TAG, "sys0b");
    ESP_RETURN_ON_ERROR(es8311_write_reg(state, ES8311_REG_SYSTEM_0C, 0x00), TAG, "sys0c");
    ESP_RETURN_ON_ERROR(es8311_write_reg(state, ES8311_REG_SYSTEM_10, 0x1f), TAG, "sys10");
    ESP_RETURN_ON_ERROR(es8311_write_reg(state, ES8311_REG_SYSTEM_11, 0x7f), TAG, "sys11");
    ESP_RETURN_ON_ERROR(es8311_write_reg(state, ES8311_REG_RESET, 0x80), TAG, "reset");
    ESP_RETURN_ON_ERROR(es8311_update_reg(state, ES8311_REG_RESET, 0x40, 0x00), TAG, "slave");
    ESP_RETURN_ON_ERROR(es8311_write_reg(state, ES8311_REG_CLK_MANAGER_01, 0x3f), TAG, "clk1 on");
    ESP_RETURN_ON_ERROR(es8311_update_reg(state, ES8311_REG_CLK_MANAGER_06, 0x20, 0x00), TAG, "clk6");
    ESP_RETURN_ON_ERROR(es8311_write_reg(state, ES8311_REG_SYSTEM_13, 0x10), TAG, "sys13");
    ESP_RETURN_ON_ERROR(es8311_write_reg(state, ES8311_REG_ADC_1B, 0x0a), TAG, "adc1b");
    ESP_RETURN_ON_ERROR(es8311_write_reg(state, ES8311_REG_ADC_1C, 0x6a), TAG, "adc1c");
    ESP_RETURN_ON_ERROR(es8311_write_reg(state, ES8311_REG_GPIO_44, 0x58), TAG, "dac ref");
    return ESP_OK;
}

static esp_err_t es8311_start(h2_esp_es8311_audio_system_t *state) {
    ESP_RETURN_ON_ERROR(es8311_write_reg(state, ES8311_REG_RESET, 0x80), TAG, "reset start");
    ESP_RETURN_ON_ERROR(es8311_write_reg(state, ES8311_REG_CLK_MANAGER_01, 0x3f), TAG, "clk1 start");
    ESP_RETURN_ON_ERROR(es8311_update_reg(state, ES8311_REG_SDP_IN, 0x60, 0x00), TAG, "sdp in lrp");
    ESP_RETURN_ON_ERROR(es8311_update_reg(state, ES8311_REG_SDP_OUT, 0x60, 0x00), TAG, "sdp out lrp");
    ESP_RETURN_ON_ERROR(es8311_write_reg(state, ES8311_REG_ADC_17, state->config.adc_digital_volume), TAG, "adc volume");
    ESP_RETURN_ON_ERROR(es8311_write_reg(state, ES8311_REG_SYSTEM_0E, 0x02), TAG, "sys0e");
    ESP_RETURN_ON_ERROR(es8311_write_reg(state, ES8311_REG_SYSTEM_12, 0x00), TAG, "sys12");
    ESP_RETURN_ON_ERROR(es8311_write_reg(state, ES8311_REG_SYSTEM_14, 0x1a), TAG, "sys14");
    ESP_RETURN_ON_ERROR(es8311_update_reg(state, ES8311_REG_SYSTEM_14, 0x40, 0x00), TAG, "analog mic");
    ESP_RETURN_ON_ERROR(es8311_write_reg(state, ES8311_REG_SYSTEM_0D, 0x01), TAG, "sys0d");
    ESP_RETURN_ON_ERROR(es8311_write_reg(state, ES8311_REG_ADC_15, 0x40), TAG, "adc15");
    ESP_RETURN_ON_ERROR(es8311_write_reg(state, ES8311_REG_DAC_37, 0x08), TAG, "dac37");
    ESP_RETURN_ON_ERROR(es8311_write_reg(state, ES8311_REG_GP_45, 0x00), TAG, "gp45");
    return ESP_OK;
}

static int es8311_audio_init_i2c(h2_esp_es8311_audio_system_t *state) {
    if (state->codec != NULL) {
        return H2_AUDIO_OK;
    }

    esp_err_t err = ESP_OK;
    if (state->config.i2c_bus != NULL) {
        state->i2c_bus = state->config.i2c_bus;
        state->owns_i2c_bus = 0;
    } else {
        i2c_master_bus_config_t bus_cfg = {
            .i2c_port = state->config.i2c_port,
            .sda_io_num = (gpio_num_t)state->config.i2c_sda_gpio,
            .scl_io_num = (gpio_num_t)state->config.i2c_scl_gpio,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags = {
                .enable_internal_pullup = true,
            },
        };
        err = i2c_new_master_bus(&bus_cfg, &state->i2c_bus);
        if (err == ESP_ERR_INVALID_STATE) {
            err = i2c_master_get_bus_handle(state->config.i2c_port, &state->i2c_bus);
            state->owns_i2c_bus = 0;
        } else {
            state->owns_i2c_bus = err == ESP_OK ? 1 : 0;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus init failed err=0x%x", (unsigned)err);
        return map_esp_err(err);
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = state->config.codec_i2c_addr,
        .scl_speed_hz = state->config.i2c_speed_hz,
    };
    err = i2c_master_bus_add_device(state->i2c_bus, &dev_cfg, &state->codec);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "es8311 device add failed err=0x%x", (unsigned)err);
        return map_esp_err(err);
    }
    return H2_AUDIO_OK;
}

static int es8311_audio_init_i2s(h2_esp_es8311_audio_system_t *state) {
    if (state->tx_chan != NULL && state->rx_chan != NULL) {
        return H2_AUDIO_OK;
    }
    if (state->write_mutex == NULL) {
        state->write_mutex = xSemaphoreCreateMutex();
        if (state->write_mutex == NULL) {
            return H2_AUDIO_ERR_NO_MEMORY;
        }
    }

    /* A previous image may have driven DIN low during reboot cleanup. */
    esp_err_t err = gpio_output_disable((gpio_num_t)state->config.din_gpio);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s din output disable failed err=0x%x", (unsigned)err);
        return map_esp_err(err);
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(state->config.i2s_port, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    if (state->config.i2s_dma_desc_num > 0u) {
        chan_cfg.dma_desc_num = state->config.i2s_dma_desc_num;
    }
    if (state->config.i2s_dma_frame_num > 0u) {
        chan_cfg.dma_frame_num = state->config.i2s_dma_frame_num;
    }
    err = i2s_new_channel(&chan_cfg, &state->tx_chan, &state->rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s new channel failed err=0x%x", (unsigned)err);
        return map_esp_err(err);
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(state->config.sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)state->config.mclk_gpio,
            .bclk = (gpio_num_t)state->config.bclk_gpio,
            .ws = (gpio_num_t)state->config.ws_gpio,
            .dout = (gpio_num_t)state->config.dout_gpio,
            .din = (gpio_num_t)state->config.din_gpio,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = state->config.mclk_multiple;

    err = i2s_channel_init_std_mode(state->tx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s tx init failed err=0x%x", (unsigned)err);
        return map_esp_err(err);
    }
    err = i2s_channel_init_std_mode(state->rx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s rx init failed err=0x%x", (unsigned)err);
        return map_esp_err(err);
    }
    err = i2s_channel_enable(state->tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s tx enable failed err=0x%x", (unsigned)err);
        return map_esp_err(err);
    }
    err = i2s_channel_enable(state->rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s rx enable failed err=0x%x", (unsigned)err);
        return map_esp_err(err);
    }
    return H2_AUDIO_OK;
}

static int es8311_audio_init_codec(h2_esp_es8311_audio_system_t *state) {
    int rc = es8311_audio_init_i2c(state);
    if (rc != H2_AUDIO_OK) {
        return rc;
    }
    esp_err_t err = es8311_open(state);
    if (err == ESP_OK) {
        uint8_t id1 = 0;
        uint8_t id2 = 0;
        err = es8311_read_reg(state, ES8311_REG_CHIP_ID1, &id1);
        if (err == ESP_OK) {
            err = es8311_read_reg(state, ES8311_REG_CHIP_ID2, &id2);
        }
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "es8311 chip_id=0x%02x%02x", id1, id2);
        }
    }
    if (err == ESP_OK) {
        err = es8311_set_sample_rate_16k(state);
    }
    if (err == ESP_OK) {
        err = es8311_update_reg(state, ES8311_REG_SDP_IN, 0x1c, 0x0c);
    }
    if (err == ESP_OK) {
        err = es8311_update_reg(state, ES8311_REG_SDP_OUT, 0x1c, 0x0c);
    }
    if (err == ESP_OK) {
        err = es8311_update_reg(state, ES8311_REG_SDP_IN, 0x03, 0x00);
    }
    if (err == ESP_OK) {
        err = es8311_update_reg(state, ES8311_REG_SDP_OUT, 0x03, 0x00);
    }
    if (err == ESP_OK) {
        err = es8311_write_reg(state, ES8311_REG_ADC_16, es8311_mic_gain_from_db(state->config.mic_gain_db));
    }
    if (err == ESP_OK) {
        err = es8311_start(state);
    }
    if (err == ESP_OK) {
        err = es8311_write_reg(state, ES8311_REG_DAC_32, es8311_volume_from_percent_with_default(
            state->speaker_volume_percent,
            state->config.codec_volume_default));
    }
    if (err == ESP_OK) {
        err = es8311_update_reg(state, ES8311_REG_DAC_31, 0x60, 0x00);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "es8311 init failed err=0x%x", (unsigned)err);
        return map_esp_err(err);
    }
    return H2_AUDIO_OK;
}

static int es8311_audio_init_system(h2_esp_es8311_audio_system_t *state) {
    if (state->sr_initialized) {
        return H2_AUDIO_OK;
    }
    if (state->config.enable_aec) {
        int rc = h2_esp_es8311_sr_init(&state->sr, &state->config);
        if (rc != H2_AUDIO_OK) {
            return rc;
        }
    }
    state->sr_initialized = 1;
    return H2_AUDIO_OK;
}

static int es8311_audio_init_mixer(h2_esp_es8311_audio_system_t *state) {
    if (state->mixer_initialized) {
        return H2_AUDIO_OK;
    }
    h2_audio_mixer_config_t config = {
        .format = es8311_audio_playback_format(state),
        .max_tracks = state->config.max_tracks,
        .track_queue_frames = state->config.track_queue_frames,
        .allocator = state->config.allocator,
        .queue_api = state->config.queue_api,
        .sync_api = state->config.sync_api,
    };
    int rc = h2_audio_mixer_init(&state->mixer, &config);
    if (rc != H2_AUDIO_OK) {
        return rc;
    }
    state->mixer_initialized = 1;
    return H2_AUDIO_OK;
}

static int es8311_audio_init_mic_queue(h2_esp_es8311_audio_system_t *state) {
    if (state->mic_queue_initialized) {
        return H2_AUDIO_OK;
    }
    h2_pal_queue_config_t config = {
        .name = "es8311_mic",
        .item_size = sizeof(h2_esp_es8311_mic_queue_frame_t),
        .item_count = state->config.mic_queue_frames,
        .allocator = state->config.allocator,
    };
    int rc = h2_pal_queue_create(state->config.queue_api, &config, &state->mic_queue);
    if (rc != H2_PAL_QUEUE_OK) {
        return rc == H2_PAL_QUEUE_ERR_NO_MEMORY ? H2_AUDIO_ERR_NO_MEMORY : H2_AUDIO_ERR_IO;
    }
    state->mic_queue_initialized = 1;
    return H2_AUDIO_OK;
}

static int es8311_audio_wait_mic_exit(
    h2_esp_es8311_audio_system_t *state) {
    for (uint32_t waited_ms = 0u;
         state->mic_task != NULL && waited_ms < H2_ESP_ES8311_STOP_TIMEOUT_MS;
         waited_ms += 10u) {
        vTaskDelay(pdMS_TO_TICKS(10u));
    }
    return state->mic_task == NULL ? H2_AUDIO_OK : H2_AUDIO_ERR_WOULD_BLOCK;
}

static int map_queue_recv_rc(int rc) {
    if (rc == H2_PAL_QUEUE_OK) {
        return H2_AUDIO_OK;
    }
    if (rc == H2_PAL_QUEUE_ERR_TIMEOUT) {
        return H2_AUDIO_ERR_WOULD_BLOCK;
    }
    if (rc == H2_PAL_QUEUE_ERR_CLOSED) {
        return H2_AUDIO_ERR_INVALID_STATE;
    }
    if (rc == H2_PAL_QUEUE_ERR_INVALID_ARG) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    return H2_AUDIO_ERR_IO;
}

static void es8311_audio_mic_task(void *arg) {
    h2_esp_es8311_audio_system_t *state = (h2_esp_es8311_audio_system_t *)arg;
    const h2_audio_pcm_format_t raw_format = es8311_audio_raw_mic_format(state);
    const h2_audio_pcm_format_t processed_format = es8311_audio_mic_format(state);
    const size_t raw_frame_bytes = (size_t)raw_format.channels * sizeof(int16_t);
    const size_t requested = (size_t)raw_format.frame_samples_per_channel * raw_frame_bytes;

    while (state->mic_task_started) {
        if (!state->mic_started || state->rx_chan == NULL || state->mic_queue == NULL) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        size_t bytes_read = 0u;
        esp_err_t err = i2s_channel_read(
            state->rx_chan,
            state->mic_raw_scratch,
            requested,
            &bytes_read,
            pdMS_TO_TICKS(100));
        if (!state->mic_started) {
            continue;
        }
        if (err == ESP_ERR_TIMEOUT || bytes_read == 0u) {
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2s mic read failed err=0x%x", (unsigned)err);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        h2_audio_frame_t raw_frame = h2_audio_frame_for_buffer(
            state->mic_raw_scratch,
            sizeof(state->mic_raw_scratch),
            raw_format);
        raw_frame.bytes = bytes_read;
        raw_frame.samples_per_channel = (uint16_t)(bytes_read / raw_frame_bytes);

        h2_audio_frame_t processed = h2_audio_frame_for_buffer(
            state->mic_processed_scratch,
            sizeof(state->mic_processed_scratch),
            processed_format);
        state->mic_processing = 1;
        if (!state->mic_started) {
            state->mic_processing = 0;
            continue;
        }
        int rc = h2_esp_es8311_audio_system_process_mic(state, &raw_frame, &processed, 0u);
        state->mic_processing = 0;
        if (rc != H2_AUDIO_OK || processed.bytes == 0u) {
            ESP_LOGW(TAG, "mic process failed rc=%d", rc);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        h2_esp_es8311_mic_queue_frame_t item;
        item.bytes = processed.bytes;
        item.samples_per_channel = processed.samples_per_channel;
        memcpy(item.samples, processed.data, processed.bytes);
        (void)h2_pal_queue_send_latest(state->config.queue_api, state->mic_queue, &item);
    }
    state->mic_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

static int es8311_audio_start_mic_task(h2_esp_es8311_audio_system_t *state) {
    if (state->mic_task_started) {
        return H2_AUDIO_OK;
    }
    state->mic_task_started = 1;
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
        es8311_audio_mic_task,
        h2_pal_audio_mic_task_name,
        state->config.mic_task_stack_size,
        state,
        state->config.mic_task_priority,
        &state->mic_task,
        state->config.mic_task_core_id,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        state->mic_task_started = 0;
        state->mic_task = NULL;
        return H2_AUDIO_ERR_NO_MEMORY;
    }
    ESP_LOGI(
        TAG,
        "mic task ready stack=psram size=%u priority=%u core=%d",
        (unsigned)state->config.mic_task_stack_size,
        (unsigned)state->config.mic_task_priority,
        (int)state->config.mic_task_core_id);
    return H2_AUDIO_OK;
}

static void es8311_audio_abort_mic_start(
    h2_esp_es8311_audio_system_t *state) {
    state->mic_started = 0;
}

static void es8311_audio_playback_task(void *arg) {
    h2_esp_es8311_audio_system_t *state = (h2_esp_es8311_audio_system_t *)arg;
    for (;;) {
        if (xSemaphoreTake(
                state->write_mutex,
                pdMS_TO_TICKS(H2_ESP_ES8311_CONTROL_TIMEOUT_MS)) != pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(H2_ESP_ES8311_RETRY_DELAY_MS));
            continue;
        }
        if (!state->playback_started) {
            state->playback_task = NULL;
            xSemaphoreGive(state->write_mutex);
            vTaskDeleteWithCaps(NULL);
            return;
        }
        if (state->tx_chan == NULL || !state->mixer_initialized) {
            state->playback_started = 0;
            state->playback_task = NULL;
            xSemaphoreGive(state->write_mutex);
            ESP_LOGE(TAG, "playback resources disappeared while active");
            vTaskDeleteWithCaps(NULL);
            return;
        }
        h2_audio_frame_t frame = h2_audio_frame_for_buffer(
            state->playback_scratch,
            sizeof(state->playback_scratch),
            es8311_audio_playback_format(state));
        int rc = h2_audio_mixer_read(&state->mixer, &frame);
        xSemaphoreGive(state->write_mutex);
        if (rc != H2_AUDIO_OK || frame.bytes == 0u) {
            vTaskDelay(pdMS_TO_TICKS(H2_ESP_ES8311_RETRY_DELAY_MS));
            continue;
        }

        const int16_t *mono = (const int16_t *)frame.data;
        const size_t samples = frame.bytes / sizeof(int16_t);
        for (size_t i = 0u; i < samples; ++i) {
            state->stereo_scratch[(i * 2u) + 0u] = mono[i];
            state->stereo_scratch[(i * 2u) + 1u] = mono[i];
        }
        size_t bytes_written = 0u;
        esp_err_t err = i2s_channel_write(
            state->tx_chan,
            state->stereo_scratch,
            samples * 2u * sizeof(int16_t),
            &bytes_written,
            pdMS_TO_TICKS(H2_ESP_ES8311_IO_TIMEOUT_MS));
        if (err != ESP_OK || bytes_written != samples * 2u * sizeof(int16_t)) {
            ESP_LOGW(TAG, "i2s playback write failed err=0x%x bytes=%u/%u",
                (unsigned)err,
                (unsigned)bytes_written,
                (unsigned)(samples * 2u * sizeof(int16_t)));
            vTaskDelay(pdMS_TO_TICKS(H2_ESP_ES8311_RETRY_DELAY_MS));
        }
    }
}

static int es8311_audio_start_playback_task_locked(h2_esp_es8311_audio_system_t *state) {
    if (state->playback_task_started) {
        return H2_AUDIO_OK;
    }
    if (state->playback_task != NULL) {
        return H2_AUDIO_ERR_INVALID_STATE;
    }
    state->playback_task_started = 1;
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
        es8311_audio_playback_task,
        h2_pal_audio_mix_task_name,
        state->config.speaker_task_stack_size,
        state,
        state->config.speaker_task_priority,
        &state->playback_task,
        state->config.speaker_task_core_id,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        state->playback_task_started = 0;
        state->playback_task = NULL;
        return H2_AUDIO_ERR_NO_MEMORY;
    }
    ESP_LOGI(
        TAG,
        "playback task ready stack=psram size=%u priority=%u core=%d",
        (unsigned)state->config.speaker_task_stack_size,
        (unsigned)state->config.speaker_task_priority,
        (int)state->config.speaker_task_core_id);
    return H2_AUDIO_OK;
}

static void es8311_audio_cleanup_failed_speaker_start(
    h2_esp_es8311_audio_system_t *state) {
    (void)es8311_audio_set_pa(state, 0);
    if (state->mixer_initialized) {
        h2_audio_mixer_deinit(&state->mixer);
        state->mixer_initialized = 0;
    }
}

static int es8311_audio_wait_playback_exit(
    h2_esp_es8311_audio_system_t *state,
    TickType_t stop_started_at) {
    for (;;) {
        TickType_t remaining = es8311_audio_stop_ticks_remaining(stop_started_at);
        const TickType_t wait_ticks = es8311_audio_control_wait_ticks(remaining);
        if (xSemaphoreTake(state->write_mutex, wait_ticks) == pdTRUE) {
            const int stopped = state->playback_task == NULL;
            xSemaphoreGive(state->write_mutex);
            if (stopped) {
                return H2_AUDIO_OK;
            }
        }
        remaining = es8311_audio_stop_ticks_remaining(stop_started_at);
        if (remaining == 0u) {
            return H2_AUDIO_ERR_WOULD_BLOCK;
        }
        TickType_t delay_ticks = pdMS_TO_TICKS(10u);
        if (delay_ticks > remaining) {
            delay_ticks = remaining;
        }
        vTaskDelay(delay_ticks);
    }
}

static int es8311_audio_get_info(void *user, h2_audio_info_t *info) {
    memset(info, 0, sizeof(*info));
    info->available = 1;
    info->mic_supported = 1;
    info->playback_supported = 1;
    h2_esp_es8311_audio_system_t *state = (h2_esp_es8311_audio_system_t *)user;
    info->mic_format = es8311_audio_mic_format(state);
    info->playback_format = es8311_audio_playback_format(state);
    info->mic_queue_frames = state->config.mic_queue_frames;
    info->track_queue_frames = state->config.track_queue_frames;
    info->max_tracks = state->config.max_tracks;
    return H2_AUDIO_OK;
}

static int es8311_audio_open(void *user) {
    h2_esp_es8311_audio_system_t *state = (h2_esp_es8311_audio_system_t *)user;
    if (state->opened) {
        return H2_AUDIO_OK;
    }
    int rc = es8311_audio_init_pa(state);
    if (rc != H2_AUDIO_OK) {
        return rc;
    }
    rc = es8311_audio_init_i2s(state);
    if (rc != H2_AUDIO_OK) {
        return rc;
    }
    rc = es8311_audio_init_codec(state);
    if (rc != H2_AUDIO_OK) {
        return rc;
    }
    rc = es8311_audio_init_system(state);
    if (rc != H2_AUDIO_OK) {
        return rc;
    }
    state->opened = 1;
    ESP_LOGI(TAG, "audio opened i2s=%d i2c=%d addr=0x%x",
        state->config.i2s_port,
        state->config.i2c_port,
        state->config.codec_i2c_addr);
    return H2_AUDIO_OK;
}

int h2_esp_es8311_audio_system_prepare(
    h2_esp_es8311_audio_system_t *system) {
    if (system == NULL) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    return es8311_audio_open(system);
}

static int es8311_audio_read_mic(
    void *user,
    h2_audio_frame_t *out_frame,
    uint32_t timeout_ms) {
    h2_esp_es8311_audio_system_t *state = (h2_esp_es8311_audio_system_t *)user;
    if (!state->mic_started || state->mic_queue == NULL) {
        return H2_AUDIO_ERR_INVALID_STATE;
    }
    const h2_audio_pcm_format_t processed_format = es8311_audio_mic_format(state);
    if (out_frame->sample_rate_hz != processed_format.sample_rate_hz ||
        out_frame->channels != processed_format.channels ||
        out_frame->sample_format != processed_format.sample_format) {
        return H2_AUDIO_ERR_UNSUPPORTED;
    }
    const size_t processed_frame_bytes = h2_audio_frame_frame_bytes(out_frame);
    if (processed_frame_bytes == 0u || out_frame->capacity < processed_frame_bytes) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }

    h2_esp_es8311_mic_queue_frame_t item;
    int rc = h2_pal_queue_recv(state->config.queue_api, state->mic_queue, &item, timeout_ms);
    if (rc != H2_PAL_QUEUE_OK) {
        return map_queue_recv_rc(rc);
    }
    if (item.bytes > out_frame->capacity) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    memcpy(out_frame->data, item.samples, item.bytes);
    out_frame->bytes = item.bytes;
    out_frame->samples_per_channel = item.samples_per_channel;
    return H2_AUDIO_OK;
}

static int es8311_audio_start_mic(void *user) {
    h2_esp_es8311_audio_system_t *state = (h2_esp_es8311_audio_system_t *)user;
    int rc = es8311_audio_open(user);
    if (rc != H2_AUDIO_OK) {
        return rc;
    }
    if (state->rx_chan == NULL) {
        return H2_AUDIO_ERR_INVALID_STATE;
    }
    if (state->config.enable_aec && !state->sr_initialized) {
        rc = h2_esp_es8311_sr_init(&state->sr, &state->config);
        if (rc != H2_AUDIO_OK) {
            return rc;
        }
        state->sr_initialized = 1;
    }
    rc = es8311_audio_init_mic_queue(state);
    if (rc != H2_AUDIO_OK) {
        es8311_audio_abort_mic_start(state);
        return rc;
    }
    (void)h2_pal_queue_reset(state->config.queue_api, state->mic_queue);
    state->mic_started = 1;
    h2_esp_es8311_sr_reset(&state->sr);
    rc = es8311_audio_start_mic_task(state);
    if (rc != H2_AUDIO_OK) {
        es8311_audio_abort_mic_start(state);
    }
    return rc;
}

static int es8311_audio_stop_mic(void *user) {
    h2_esp_es8311_audio_system_t *state = (h2_esp_es8311_audio_system_t *)user;
    state->mic_started = 0;
    for (uint32_t waited_ms = 0u; state->mic_processing && waited_ms < 200u;
         waited_ms += 10u) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (state->mic_processing) {
        return H2_AUDIO_ERR_INVALID_STATE;
    }
    if (state->mic_queue != NULL) {
        (void)h2_pal_queue_reset(state->config.queue_api, state->mic_queue);
    }
    return H2_AUDIO_OK;
}

static int es8311_audio_start_speaker(void *user) {
    h2_esp_es8311_audio_system_t *state = (h2_esp_es8311_audio_system_t *)user;
    int rc = es8311_audio_open(user);
    if (rc != H2_AUDIO_OK) {
        return rc;
    }
    if (xSemaphoreTake(
            state->write_mutex,
            pdMS_TO_TICKS(H2_ESP_ES8311_CONTROL_TIMEOUT_MS)) != pdTRUE) {
        return H2_AUDIO_ERR_WOULD_BLOCK;
    }
    if (state->playback_started) {
        const int active = state->playback_task != NULL && state->playback_task_started;
        xSemaphoreGive(state->write_mutex);
        return active ? H2_AUDIO_OK : H2_AUDIO_ERR_INVALID_STATE;
    }
    if (state->playback_task != NULL || state->playback_task_started) {
        xSemaphoreGive(state->write_mutex);
        return H2_AUDIO_ERR_INVALID_STATE;
    }
    xSemaphoreGive(state->write_mutex);

    rc = es8311_audio_init_mixer(state);
    if (rc != H2_AUDIO_OK) {
        return rc;
    }
    const size_t silence_samples =
        (size_t)state->config.frame_samples_per_channel * 2u;
    memset(state->stereo_scratch, 0, silence_samples * sizeof(int16_t));
    size_t bytes_written = 0u;
    const esp_err_t write_err = i2s_channel_write(
        state->tx_chan, state->stereo_scratch,
        silence_samples * sizeof(int16_t), &bytes_written,
        pdMS_TO_TICKS(H2_ESP_ES8311_IO_TIMEOUT_MS));
    if (write_err != ESP_OK ||
        bytes_written != silence_samples * sizeof(int16_t)) {
        es8311_audio_cleanup_failed_speaker_start(state);
        return H2_AUDIO_ERR_IO;
    }
    rc = es8311_audio_set_pa(state, 1);
    if (rc != H2_AUDIO_OK) {
        es8311_audio_cleanup_failed_speaker_start(state);
        return rc;
    }
    if (xSemaphoreTake(
            state->write_mutex,
            pdMS_TO_TICKS(H2_ESP_ES8311_CONTROL_TIMEOUT_MS)) != pdTRUE) {
        es8311_audio_cleanup_failed_speaker_start(state);
        return H2_AUDIO_ERR_WOULD_BLOCK;
    }
    state->playback_started = 1;
    rc = es8311_audio_start_playback_task_locked(state);
    if (rc != H2_AUDIO_OK) {
        state->playback_started = 0;
    }
    xSemaphoreGive(state->write_mutex);
    if (rc != H2_AUDIO_OK) {
        es8311_audio_cleanup_failed_speaker_start(state);
    }
    return rc;
}

static int es8311_audio_stop_speaker(void *user) {
    h2_esp_es8311_audio_system_t *state = (h2_esp_es8311_audio_system_t *)user;
    if (state->write_mutex == NULL) {
        state->playback_started = 0;
        state->playback_task_started = 0;
        state->playback_task = NULL;
        return state->pa_initialized
            ? es8311_audio_set_pa(state, 0)
            : H2_AUDIO_OK;
    }
    const TickType_t stop_started_at = xTaskGetTickCount();
    int first_rc = H2_AUDIO_OK;
    int stop_published = 0;

    const TickType_t remaining = es8311_audio_stop_ticks_remaining(stop_started_at);
    if (xSemaphoreTake(
            state->write_mutex,
            es8311_audio_control_wait_ticks(remaining)) == pdTRUE) {
        state->playback_started = 0;
        stop_published = 1;
        xSemaphoreGive(state->write_mutex);
    } else {
        first_rc = H2_AUDIO_ERR_WOULD_BLOCK;
    }

    const int pa_rc = es8311_audio_set_pa(state, 0);
    if (first_rc == H2_AUDIO_OK && pa_rc != H2_AUDIO_OK) {
        first_rc = pa_rc;
    }
    if (stop_published) {
        const int join_rc = es8311_audio_wait_playback_exit(state, stop_started_at);
        if (first_rc == H2_AUDIO_OK && join_rc != H2_AUDIO_OK) {
            first_rc = join_rc;
        }
        if (join_rc == H2_AUDIO_OK) {
            TickType_t cleanup_remaining =
                es8311_audio_stop_ticks_remaining(stop_started_at);
            if (xSemaphoreTake(
                    state->write_mutex,
                    es8311_audio_control_wait_ticks(cleanup_remaining)) == pdTRUE) {
                state->playback_task_started = 0;
                xSemaphoreGive(state->write_mutex);
            } else if (first_rc == H2_AUDIO_OK) {
                first_rc = H2_AUDIO_ERR_WOULD_BLOCK;
            }
        }
    }
    return first_rc;
}

int h2_esp_es8311_audio_system_deinit(
    h2_esp_es8311_audio_system_t *system) {
    if (system == NULL) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    if (system->config.queue_api == NULL) {
        memset(system, 0, sizeof(*system));
        return H2_AUDIO_OK;
    }

    int rc = es8311_audio_stop_mic(system);
    if (rc != H2_AUDIO_OK) {
        return rc;
    }
    system->mic_task_started = 0;
    rc = es8311_audio_wait_mic_exit(system);
    if (rc != H2_AUDIO_OK) {
        return rc;
    }

    rc = es8311_audio_stop_speaker(system);
    if (rc != H2_AUDIO_OK) {
        return rc;
    }
    if (system->playback_task != NULL) {
        return H2_AUDIO_ERR_WOULD_BLOCK;
    }

    if (system->mic_queue != NULL) {
        h2_pal_queue_destroy(system->config.queue_api, system->mic_queue);
        system->mic_queue = NULL;
    }
    system->mic_queue_initialized = 0;
    if (system->mixer_initialized) {
        h2_audio_mixer_deinit(&system->mixer);
        system->mixer_initialized = 0;
    }
    if (system->tx_chan != NULL) {
        (void)i2s_channel_disable(system->tx_chan);
        (void)i2s_del_channel(system->tx_chan);
        system->tx_chan = NULL;
    }
    if (system->rx_chan != NULL) {
        (void)i2s_channel_disable(system->rx_chan);
        (void)i2s_del_channel(system->rx_chan);
        system->rx_chan = NULL;
    }
    if (system->write_mutex != NULL) {
        vSemaphoreDelete(system->write_mutex);
        system->write_mutex = NULL;
    }
    if (system->codec != NULL) {
        (void)i2c_master_bus_rm_device(system->codec);
        system->codec = NULL;
    }
    if (system->owns_i2c_bus && system->i2c_bus != NULL) {
        (void)i2c_del_master_bus(system->i2c_bus);
    }
    system->i2c_bus = NULL;
    system->owns_i2c_bus = 0;
    if (system->sr_initialized) {
        h2_esp_es8311_sr_deinit(&system->sr);
    }
    memset(system, 0, sizeof(*system));
    return H2_AUDIO_OK;
}

static int es8311_audio_create_track(
    void *user,
    const h2_audio_track_config_t *config,
    h2_pal_audio_track_t **out_track) {
    h2_esp_es8311_audio_system_t *state = (h2_esp_es8311_audio_system_t *)user;
    const h2_audio_pcm_format_t format = es8311_audio_playback_format(state);
    if (config->format.sample_rate_hz != format.sample_rate_hz ||
        config->format.channels != format.channels ||
        config->format.sample_format != format.sample_format) {
        return H2_AUDIO_ERR_UNSUPPORTED;
    }
    int rc = es8311_audio_init_mixer(state);
    if (rc != H2_AUDIO_OK) {
        return rc;
    }
    return h2_audio_mixer_create_track(&state->mixer, &state->audio, config, out_track);
}

static int es8311_audio_get_speaker_volume_percent(void *user, uint32_t *out_percent) {
    h2_esp_es8311_audio_system_t *state = (h2_esp_es8311_audio_system_t *)user;
    *out_percent = state->speaker_volume_percent;
    return H2_AUDIO_OK;
}

static int es8311_audio_set_speaker_volume_percent(void *user, uint32_t percent) {
    h2_esp_es8311_audio_system_t *state = (h2_esp_es8311_audio_system_t *)user;
    state->speaker_volume_percent = percent;
    if (state->codec == NULL) {
        return H2_AUDIO_OK;
    }
    return map_esp_err(es8311_write_reg(state, ES8311_REG_DAC_32, es8311_volume_from_percent_with_default(
        percent,
        state->config.codec_volume_default)));
}

h2_pal_audio_t *h2_esp_es8311_audio_system_audio(h2_esp_es8311_audio_system_t *system) {
    if (system == NULL) {
        return NULL;
    }
    static const h2_pal_audio_vtable_t vtable = {
        .get_info = es8311_audio_get_info,
        .start_mic = es8311_audio_start_mic,
        .stop_mic = es8311_audio_stop_mic,
        .start_speaker = es8311_audio_start_speaker,
        .stop_speaker = es8311_audio_stop_speaker,
        .mic_read = es8311_audio_read_mic,
        .create_track = es8311_audio_create_track,
        .get_speaker_volume_percent = es8311_audio_get_speaker_volume_percent,
        .set_speaker_volume_percent = es8311_audio_set_speaker_volume_percent,
    };
    system->audio.user = system;
    system->audio.vtable = &vtable;
    return &system->audio;
}
