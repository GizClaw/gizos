#include "h2_esp_es8311_es7210_audio_system.h"
#include "h2_esp_es8311_es7210_gain.h"
#include "h2/pal/hal/h2_pal_audio_task_names.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"

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
#define ES8311_REG_SYSTEM_0B 0x0b
#define ES8311_REG_SYSTEM_0C 0x0c
#define ES8311_REG_SYSTEM_10 0x10
#define ES8311_REG_SYSTEM_11 0x11
#define ES8311_REG_SYSTEM_12 0x12
#define ES8311_REG_SYSTEM_13 0x13
#define ES8311_REG_SYSTEM_14 0x14
#define ES8311_REG_DAC_31 0x31
#define ES8311_REG_DAC_32 0x32
#define ES8311_REG_DAC_37 0x37
#define ES8311_REG_GPIO_44 0x44
#define ES8311_REG_GP_45 0x45
#define ES8311_REG_CHIP_ID1 0xfd
#define ES8311_REG_CHIP_ID2 0xfe

#define ES7210_REG_RESET 0x00
#define ES7210_REG_CLOCK_OFF 0x01
#define ES7210_REG_MAIN_CLK 0x02
#define ES7210_REG_POWER_DOWN 0x06
#define ES7210_REG_OSR 0x07
#define ES7210_REG_MODE_CONFIG 0x08
#define ES7210_REG_TIME_CONTROL0 0x09
#define ES7210_REG_TIME_CONTROL1 0x0a
#define ES7210_REG_SDP_INTERFACE1 0x11
#define ES7210_REG_SDP_INTERFACE2 0x12
#define ES7210_REG_ADC34_MUTERANGE 0x14
#define ES7210_REG_ADC12_MUTERANGE 0x15
#define ES7210_REG_ADC34_HPF2 0x20
#define ES7210_REG_ADC34_HPF1 0x21
#define ES7210_REG_ADC12_HPF1 0x22
#define ES7210_REG_ADC12_HPF2 0x23
#define ES7210_REG_ANALOG 0x40
#define ES7210_REG_MIC12_BIAS 0x41
#define ES7210_REG_MIC34_BIAS 0x42
#define ES7210_REG_MIC1_GAIN 0x43
#define ES7210_REG_MIC2_GAIN 0x44
#define ES7210_REG_MIC3_GAIN 0x45
#define ES7210_REG_MIC4_GAIN 0x46
#define ES7210_REG_MIC1_POWER 0x47
#define ES7210_REG_MIC2_POWER 0x48
#define ES7210_REG_MIC3_POWER 0x49
#define ES7210_REG_MIC4_POWER 0x4a
#define ES7210_REG_MIC12_POWER 0x4b
#define ES7210_REG_MIC34_POWER 0x4c

#define H2_ESP_ES8311_ES7210_CONTROL_TIMEOUT_MS 100u
#define H2_ESP_ES8311_ES7210_IO_TIMEOUT_MS 100u
#define H2_ESP_ES8311_ES7210_STOP_TIMEOUT_MS 200u
#define H2_ESP_ES8311_ES7210_RETRY_DELAY_MS 20u

static const char *TAG = "h2_es8311_es7210";

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

static TickType_t playback_stop_ticks_remaining(TickType_t started_at) {
    const TickType_t budget = pdMS_TO_TICKS(H2_ESP_ES8311_ES7210_STOP_TIMEOUT_MS);
    const TickType_t elapsed = xTaskGetTickCount() - started_at;
    return elapsed < budget ? budget - elapsed : 0u;
}

static TickType_t playback_control_wait_ticks(TickType_t remaining) {
    const TickType_t control_timeout =
        pdMS_TO_TICKS(H2_ESP_ES8311_ES7210_CONTROL_TIMEOUT_MS);
    return remaining < control_timeout ? remaining : control_timeout;
}

static h2_audio_pcm_format_t audio_mic_format(const h2_esp_es8311_es7210_audio_system_t *state) {
    h2_audio_pcm_format_t format = {
        .sample_rate_hz = state->config.sample_rate_hz,
        .frame_samples_per_channel = state->config.frame_samples_per_channel,
        .channels = state->config.processed_channels,
        .sample_format = H2_AUDIO_SAMPLE_S16LE,
    };
    return format;
}

static h2_audio_pcm_format_t audio_raw_mic_format(const h2_esp_es8311_es7210_audio_system_t *state) {
    h2_audio_pcm_format_t format = {
        .sample_rate_hz = state->config.sample_rate_hz,
        .frame_samples_per_channel = state->config.frame_samples_per_channel,
        .channels = state->config.raw_channels,
        .sample_format = H2_AUDIO_SAMPLE_S16LE,
    };
    return format;
}

static h2_audio_pcm_format_t audio_playback_format(const h2_esp_es8311_es7210_audio_system_t *state) {
    h2_audio_pcm_format_t format = {
        .sample_rate_hz = state->config.sample_rate_hz,
        .frame_samples_per_channel = state->config.frame_samples_per_channel,
        .channels = 1u,
        .sample_format = H2_AUDIO_SAMPLE_S16LE,
    };
    return format;
}

static esp_err_t write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value) {
    const uint8_t data[2] = { reg, value };
    return i2c_master_transmit(dev, data, sizeof(data), pdMS_TO_TICKS(100));
}

static esp_err_t read_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *value) {
    return i2c_master_transmit_receive(dev, &reg, sizeof(reg), value, sizeof(*value), pdMS_TO_TICKS(100));
}

static esp_err_t update_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t mask, uint8_t value) {
    uint8_t regv = 0;
    ESP_RETURN_ON_ERROR(read_reg(dev, reg, &regv), TAG, "read 0x%02x", reg);
    regv = (uint8_t)((regv & (uint8_t)~mask) | (value & mask));
    return write_reg(dev, reg, regv);
}

static uint8_t volume_from_percent(uint32_t percent, uint8_t max_volume) {
    uint32_t scaled = (percent * (uint32_t)max_volume) / 100u;
    return scaled > 0xffu ? 0xffu : (uint8_t)scaled;
}

static int init_pa(h2_esp_es8311_es7210_audio_system_t *state) {
    if (state->pa_initialized) {
        return H2_AUDIO_OK;
    }
    if (state->config.pa_gpio >= 0) {
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
    }
    state->pa_initialized = 1;
    return state->config.set_pa != NULL
        ? state->config.set_pa(state->config.set_pa_user, 0)
        : map_esp_err(gpio_set_level((gpio_num_t)state->config.pa_gpio, 0));
}

static int set_pa(h2_esp_es8311_es7210_audio_system_t *state, int enabled) {
    if (state->config.set_pa != NULL) {
        return state->config.set_pa(state->config.set_pa_user, enabled);
    }
    return map_esp_err(gpio_set_level((gpio_num_t)state->config.pa_gpio, enabled ? 1 : 0));
}

static int init_i2c(h2_esp_es8311_es7210_audio_system_t *state) {
    if (state->es8311 != NULL && state->es7210 != NULL) {
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
            .flags = { .enable_internal_pullup = true },
        };
        err = i2c_new_master_bus(&bus_cfg, &state->i2c_bus);
        if (err == ESP_ERR_INVALID_STATE) {
            err = i2c_master_get_bus_handle(state->config.i2c_port, &state->i2c_bus);
            state->owns_i2c_bus = 0;
        } else {
            state->owns_i2c_bus = err == ESP_OK ? 1 : 0;
        }
        if (err != ESP_OK) {
            return map_esp_err(err);
        }
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = state->config.es8311_i2c_addr,
        .scl_speed_hz = state->config.i2c_speed_hz,
    };
    err = i2c_master_bus_add_device(state->i2c_bus, &dev_cfg, &state->es8311);
    if (err != ESP_OK) {
        return map_esp_err(err);
    }
    dev_cfg.device_address = state->config.es7210_i2c_addr;
    err = i2c_master_bus_add_device(state->i2c_bus, &dev_cfg, &state->es7210);
    return map_esp_err(err);
}

static esp_err_t init_es8311(h2_esp_es8311_es7210_audio_system_t *state) {
    ESP_RETURN_ON_ERROR(write_reg(state->es8311, ES8311_REG_GPIO_44, 0x08), TAG, "es8311 gpio");
    ESP_RETURN_ON_ERROR(write_reg(state->es8311, ES8311_REG_CLK_MANAGER_01, 0x30), TAG, "es8311 clk1");
    ESP_RETURN_ON_ERROR(write_reg(state->es8311, ES8311_REG_CLK_MANAGER_02, 0x00), TAG, "es8311 clk2");
    ESP_RETURN_ON_ERROR(write_reg(state->es8311, ES8311_REG_CLK_MANAGER_03, 0x10), TAG, "es8311 clk3");
    ESP_RETURN_ON_ERROR(write_reg(state->es8311, ES8311_REG_CLK_MANAGER_04, 0x10), TAG, "es8311 clk4");
    ESP_RETURN_ON_ERROR(write_reg(state->es8311, ES8311_REG_CLK_MANAGER_05, 0x00), TAG, "es8311 clk5");
    ESP_RETURN_ON_ERROR(write_reg(state->es8311, ES8311_REG_SYSTEM_0B, 0x00), TAG, "es8311 sys0b");
    ESP_RETURN_ON_ERROR(write_reg(state->es8311, ES8311_REG_SYSTEM_0C, 0x00), TAG, "es8311 sys0c");
    ESP_RETURN_ON_ERROR(write_reg(state->es8311, ES8311_REG_SYSTEM_10, 0x1f), TAG, "es8311 sys10");
    ESP_RETURN_ON_ERROR(write_reg(state->es8311, ES8311_REG_SYSTEM_11, 0x7f), TAG, "es8311 sys11");
    ESP_RETURN_ON_ERROR(write_reg(state->es8311, ES8311_REG_RESET, 0x80), TAG, "es8311 reset");
    ESP_RETURN_ON_ERROR(update_reg(state->es8311, ES8311_REG_RESET, 0x40, 0x00), TAG, "es8311 slave");
    ESP_RETURN_ON_ERROR(write_reg(state->es8311, ES8311_REG_CLK_MANAGER_01, 0x3f), TAG, "es8311 clk1 on");
    ESP_RETURN_ON_ERROR(update_reg(state->es8311, ES8311_REG_CLK_MANAGER_06, 0x20, 0x00), TAG, "es8311 clk6");
    ESP_RETURN_ON_ERROR(write_reg(state->es8311, ES8311_REG_SYSTEM_13, 0x10), TAG, "es8311 sys13");
    ESP_RETURN_ON_ERROR(write_reg(state->es8311, ES8311_REG_GPIO_44, 0x58), TAG, "es8311 dac ref");

    if (state->config.sample_rate_hz != 16000u || state->config.mclk_multiple != 384u) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(write_reg(state->es8311, ES8311_REG_CLK_MANAGER_02, 0x48), TAG, "es8311 clk2 rate");
    ESP_RETURN_ON_ERROR(write_reg(state->es8311, ES8311_REG_CLK_MANAGER_05, 0x00), TAG, "es8311 clk5 rate");
    ESP_RETURN_ON_ERROR(update_reg(state->es8311, ES8311_REG_CLK_MANAGER_03, 0x7fu, 0x10), TAG, "es8311 clk3 rate");
    ESP_RETURN_ON_ERROR(update_reg(state->es8311, ES8311_REG_CLK_MANAGER_04, 0x7fu, 0x10), TAG, "es8311 clk4 rate");
    ESP_RETURN_ON_ERROR(update_reg(state->es8311, ES8311_REG_CLK_MANAGER_07, 0x3fu, 0x00), TAG, "es8311 clk7 rate");
    ESP_RETURN_ON_ERROR(write_reg(state->es8311, ES8311_REG_CLK_MANAGER_08, 0xff), TAG, "es8311 clk8 rate");
    ESP_RETURN_ON_ERROR(update_reg(state->es8311, ES8311_REG_CLK_MANAGER_06, 0x1fu, 0x03), TAG, "es8311 clk6 rate");
    ESP_RETURN_ON_ERROR(update_reg(state->es8311, ES8311_REG_SDP_IN, 0x1c, 0x0c), TAG, "es8311 in 16bit");
    ESP_RETURN_ON_ERROR(update_reg(state->es8311, ES8311_REG_SDP_IN, 0x03, 0x00), TAG, "es8311 i2s");
    ESP_RETURN_ON_ERROR(write_reg(state->es8311, ES8311_REG_SYSTEM_12, 0x00), TAG, "es8311 sys12");
    ESP_RETURN_ON_ERROR(write_reg(state->es8311, ES8311_REG_SYSTEM_14, 0x1a), TAG, "es8311 sys14");
    ESP_RETURN_ON_ERROR(write_reg(state->es8311, ES8311_REG_DAC_37, 0x08), TAG, "es8311 dac37");
    ESP_RETURN_ON_ERROR(write_reg(state->es8311, ES8311_REG_GP_45, 0x00), TAG, "es8311 gp45");
    ESP_RETURN_ON_ERROR(write_reg(state->es8311, ES8311_REG_DAC_32, volume_from_percent(state->speaker_volume_percent, state->config.codec_volume_default)), TAG, "es8311 vol");
    ESP_RETURN_ON_ERROR(update_reg(state->es8311, ES8311_REG_DAC_31, 0x60, 0x00), TAG, "es8311 unmute");

    uint8_t id1 = 0;
    uint8_t id2 = 0;
    if (read_reg(state->es8311, ES8311_REG_CHIP_ID1, &id1) == ESP_OK &&
        read_reg(state->es8311, ES8311_REG_CHIP_ID2, &id2) == ESP_OK) {
        ESP_LOGI(TAG, "es8311 chip_id=0x%02x%02x", id1, id2);
    }
    return ESP_OK;
}

static uint8_t es7210_input_gain(
    const h2_esp_es8311_es7210_audio_system_t *state,
    uint8_t input) {
    return h2_esp_es8311_es7210_input_gain_register(
        state->config.mic_gain_db,
        state->config.es7210_input_gain_mask,
        state->config.es7210_input_gain_db,
        input);
}

static esp_err_t es7210_select_inputs(h2_esp_es8311_es7210_audio_system_t *state) {
    ESP_RETURN_ON_ERROR(update_reg(state->es7210, ES7210_REG_MIC1_GAIN, 0x10, 0x00), TAG, "es7210 mic1 pga off");
    ESP_RETURN_ON_ERROR(update_reg(state->es7210, ES7210_REG_MIC2_GAIN, 0x10, 0x00), TAG, "es7210 mic2 pga off");
    ESP_RETURN_ON_ERROR(update_reg(state->es7210, ES7210_REG_MIC3_GAIN, 0x10, 0x00), TAG, "es7210 mic3 pga off");
    ESP_RETURN_ON_ERROR(update_reg(state->es7210, ES7210_REG_MIC4_GAIN, 0x10, 0x00), TAG, "es7210 mic4 pga off");
    ESP_RETURN_ON_ERROR(write_reg(state->es7210, ES7210_REG_MIC12_POWER, 0xff), TAG, "es7210 mic12 off");
    ESP_RETURN_ON_ERROR(write_reg(state->es7210, ES7210_REG_MIC34_POWER, 0xff), TAG, "es7210 mic34 off");
    for (uint8_t input = 0u; input < 4u; ++input) {
        if ((state->config.es7210_input_mask & (1u << input)) == 0u) {
            continue;
        }
        if (input < 2u) {
            ESP_RETURN_ON_ERROR(update_reg(state->es7210, ES7210_REG_CLOCK_OFF, 0x0b, 0x00), TAG, "es7210 mic12 clock");
            ESP_RETURN_ON_ERROR(write_reg(state->es7210, ES7210_REG_MIC12_POWER, 0x00), TAG, "es7210 mic12 power");
        } else {
            ESP_RETURN_ON_ERROR(update_reg(state->es7210, ES7210_REG_CLOCK_OFF, 0x15, 0x00), TAG, "es7210 mic34 clock");
            ESP_RETURN_ON_ERROR(write_reg(state->es7210, ES7210_REG_MIC34_POWER, 0x00), TAG, "es7210 mic34 power");
        }
        ESP_RETURN_ON_ERROR(
            write_reg(
                state->es7210,
                ES7210_REG_MIC1_GAIN + input,
                es7210_input_gain(state, input)),
            TAG,
            "es7210 input gain");
    }
    if (h2_esp_es8311_es7210_zero_legacy_ref_gain(
            state->config.enable_aec,
            state->config.es7210_input_gain_mask,
            state->config.es7210_ref_input_index)) {
        ESP_RETURN_ON_ERROR(update_reg(state->es7210, ES7210_REG_MIC1_GAIN + state->config.es7210_ref_input_index, 0x0f, 0x00), TAG, "es7210 ref gain");
    }
    const uint8_t selected_count = (uint8_t)__builtin_popcount((unsigned)state->config.es7210_input_mask);
    ESP_RETURN_ON_ERROR(write_reg(state->es7210, ES7210_REG_SDP_INTERFACE2, selected_count >= 3u ? 0x02 : 0x00), TAG, "es7210 tdm");
    return ESP_OK;
}

static esp_err_t init_es7210(h2_esp_es8311_es7210_audio_system_t *state) {
    ESP_RETURN_ON_ERROR(write_reg(state->es7210, ES7210_REG_RESET, 0xff), TAG, "es7210 reset ff");
    ESP_RETURN_ON_ERROR(write_reg(state->es7210, ES7210_REG_RESET, 0x41), TAG, "es7210 reset");
    ESP_RETURN_ON_ERROR(write_reg(state->es7210, ES7210_REG_CLOCK_OFF, 0x3f), TAG, "es7210 clock off");
    ESP_RETURN_ON_ERROR(write_reg(state->es7210, ES7210_REG_TIME_CONTROL0, 0x30), TAG, "es7210 time0");
    ESP_RETURN_ON_ERROR(write_reg(state->es7210, ES7210_REG_TIME_CONTROL1, 0x30), TAG, "es7210 time1");
    ESP_RETURN_ON_ERROR(write_reg(state->es7210, ES7210_REG_ADC12_HPF2, 0x2a), TAG, "es7210 hpf");
    ESP_RETURN_ON_ERROR(write_reg(state->es7210, ES7210_REG_ADC12_HPF1, 0x0a), TAG, "es7210 hpf");
    ESP_RETURN_ON_ERROR(write_reg(state->es7210, ES7210_REG_ADC34_HPF2, 0x0a), TAG, "es7210 hpf");
    ESP_RETURN_ON_ERROR(write_reg(state->es7210, ES7210_REG_ADC34_HPF1, 0x2a), TAG, "es7210 hpf");
    ESP_RETURN_ON_ERROR(write_reg(state->es7210, ES7210_REG_ADC12_MUTERANGE, 0x00), TAG, "es7210 unmute12");
    ESP_RETURN_ON_ERROR(write_reg(state->es7210, ES7210_REG_ADC34_MUTERANGE, 0x00), TAG, "es7210 unmute34");
    ESP_RETURN_ON_ERROR(update_reg(state->es7210, ES7210_REG_MODE_CONFIG, 0x01, 0x00), TAG, "es7210 slave");
    ESP_RETURN_ON_ERROR(write_reg(state->es7210, ES7210_REG_ANALOG, 0x43), TAG, "es7210 analog");
    ESP_RETURN_ON_ERROR(write_reg(state->es7210, ES7210_REG_MIC12_BIAS, 0x70), TAG, "es7210 bias12");
    ESP_RETURN_ON_ERROR(write_reg(state->es7210, ES7210_REG_MIC34_BIAS, 0x70), TAG, "es7210 bias34");
    ESP_RETURN_ON_ERROR(write_reg(state->es7210, ES7210_REG_OSR, 0x20), TAG, "es7210 osr");
    ESP_RETURN_ON_ERROR(write_reg(state->es7210, ES7210_REG_MAIN_CLK, 0xc1), TAG, "es7210 main clk");
    ESP_RETURN_ON_ERROR(es7210_select_inputs(state), TAG, "es7210 inputs");
    ESP_RETURN_ON_ERROR(write_reg(state->es7210, ES7210_REG_SDP_INTERFACE1, 0x60), TAG, "es7210 16bit i2s");
    ESP_RETURN_ON_ERROR(write_reg(state->es7210, ES7210_REG_ANALOG, 0x43), TAG, "es7210 analog final");
    ESP_RETURN_ON_ERROR(write_reg(state->es7210, ES7210_REG_RESET, 0x71), TAG, "es7210 start");
    ESP_RETURN_ON_ERROR(write_reg(state->es7210, ES7210_REG_RESET, 0x41), TAG, "es7210 normal");
    ESP_RETURN_ON_ERROR(write_reg(state->es7210, ES7210_REG_POWER_DOWN, 0x00), TAG, "es7210 power");
    ESP_RETURN_ON_ERROR(write_reg(state->es7210, ES7210_REG_MIC1_POWER, 0x08), TAG, "es7210 mic1 on");
    ESP_RETURN_ON_ERROR(write_reg(state->es7210, ES7210_REG_MIC2_POWER, 0x08), TAG, "es7210 mic2 on");
    ESP_RETURN_ON_ERROR(write_reg(state->es7210, ES7210_REG_MIC3_POWER, 0x08), TAG, "es7210 mic3 on");
    ESP_RETURN_ON_ERROR(write_reg(state->es7210, ES7210_REG_MIC4_POWER, 0x08), TAG, "es7210 mic4 on");
    ESP_LOGI(TAG, "es7210 ready addr=0x%02x", state->config.es7210_i2c_addr);
    return ESP_OK;
}

static int init_i2s(h2_esp_es8311_es7210_audio_system_t *state) {
    if (state->tx_chan != NULL && state->rx_chan != NULL) {
        return H2_AUDIO_OK;
    }
    if (state->write_mutex == NULL) {
        state->write_mutex = xSemaphoreCreateMutex();
        if (state->write_mutex == NULL) {
            return H2_AUDIO_ERR_NO_MEMORY;
        }
    }
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(state->config.i2s_port, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    esp_err_t err = i2s_new_channel(&chan_cfg, &state->tx_chan, &state->rx_chan);
    if (err != ESP_OK) {
        return map_esp_err(err);
    }
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(state->config.sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)state->config.mclk_gpio,
            .bclk = (gpio_num_t)state->config.bclk_gpio,
            .ws = (gpio_num_t)state->config.ws_gpio,
            .dout = (gpio_num_t)state->config.dout_gpio,
            .din = (gpio_num_t)state->config.din_gpio,
            .invert_flags = { 0 },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = state->config.mclk_multiple;
    err = i2s_channel_init_std_mode(state->tx_chan, &std_cfg);
    if (err == ESP_OK) {
        err = i2s_channel_init_std_mode(state->rx_chan, &std_cfg);
    }
    if (err == ESP_OK) {
        err = i2s_channel_enable(state->tx_chan);
    }
    if (err == ESP_OK) {
        err = i2s_channel_enable(state->rx_chan);
    }
    return map_esp_err(err);
}

static int init_codecs(h2_esp_es8311_es7210_audio_system_t *state) {
    int rc = init_i2c(state);
    if (rc != H2_AUDIO_OK) {
        return rc;
    }
    esp_err_t err = init_es8311(state);
    if (err == ESP_OK) {
        err = init_es7210(state);
    }
    return map_esp_err(err);
}

static int init_mixer(h2_esp_es8311_es7210_audio_system_t *state) {
    if (state->mixer_initialized) {
        return H2_AUDIO_OK;
    }
    h2_audio_mixer_config_t config = {
        .format = audio_playback_format(state),
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

static int init_mic_queue(h2_esp_es8311_es7210_audio_system_t *state) {
    if (state->mic_queue_initialized) {
        return H2_AUDIO_OK;
    }
    h2_pal_queue_config_t config = {
        .name = "es7210_mic",
        .item_size = sizeof(h2_esp_es8311_es7210_mic_queue_frame_t),
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

static int wait_task_exit(TaskHandle_t *task) {
    for (uint8_t i = 0u; i < 50u && task != NULL && *task != NULL; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return task == NULL || *task == NULL;
}

static void deinit_codecs(h2_esp_es8311_es7210_audio_system_t *state) {
    if (state->es7210 != NULL) {
        (void)i2c_master_bus_rm_device(state->es7210);
        state->es7210 = NULL;
    }
    if (state->es8311 != NULL) {
        (void)i2c_master_bus_rm_device(state->es8311);
        state->es8311 = NULL;
    }
    if (state->owns_i2c_bus && state->i2c_bus != NULL) {
        (void)i2c_del_master_bus(state->i2c_bus);
    }
    state->i2c_bus = NULL;
    state->owns_i2c_bus = 0;
}

static void deinit_i2s_if_idle(h2_esp_es8311_es7210_audio_system_t *state) {
    if (state->mic_started || state->mic_task != NULL) {
        return;
    }
    if (state->write_mutex != NULL) {
        if (xSemaphoreTake(
                state->write_mutex,
                pdMS_TO_TICKS(H2_ESP_ES8311_ES7210_CONTROL_TIMEOUT_MS)) != pdTRUE) {
            return;
        }
        const int playback_idle =
            !state->playback_started && state->playback_task == NULL;
        xSemaphoreGive(state->write_mutex);
        if (!playback_idle) {
            return;
        }
    }
    if (state->tx_chan != NULL) {
        (void)i2s_channel_disable(state->tx_chan);
        (void)i2s_del_channel(state->tx_chan);
        state->tx_chan = NULL;
    }
    if (state->rx_chan != NULL) {
        (void)i2s_channel_disable(state->rx_chan);
        (void)i2s_del_channel(state->rx_chan);
        state->rx_chan = NULL;
    }
    if (state->write_mutex != NULL) {
        vSemaphoreDelete(state->write_mutex);
        state->write_mutex = NULL;
    }
    deinit_codecs(state);
    state->opened = 0;
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

static int audio_open(void *user) {
    h2_esp_es8311_es7210_audio_system_t *state = (h2_esp_es8311_es7210_audio_system_t *)user;
    if (state->opened) {
        return H2_AUDIO_OK;
    }
    int rc = init_pa(state);
    if (rc == H2_AUDIO_OK) {
        rc = init_i2s(state);
    }
    if (rc == H2_AUDIO_OK) {
        rc = init_codecs(state);
    }
    if (rc != H2_AUDIO_OK) {
        deinit_i2s_if_idle(state);
        return rc;
    }
    state->opened = 1;
    ESP_LOGI(TAG, "audio opened i2s=%d es8311=0x%x es7210=0x%x",
        state->config.i2s_port,
        state->config.es8311_i2c_addr,
        state->config.es7210_i2c_addr);
    return H2_AUDIO_OK;
}

static void mic_task(void *arg) {
    h2_esp_es8311_es7210_audio_system_t *state = (h2_esp_es8311_es7210_audio_system_t *)arg;
    const h2_audio_pcm_format_t raw_format = audio_raw_mic_format(state);
    const h2_audio_pcm_format_t processed_format = audio_mic_format(state);
    const size_t raw_frame_bytes = (size_t)raw_format.channels * sizeof(int16_t);
    const size_t requested = (size_t)raw_format.frame_samples_per_channel * raw_frame_bytes;

    while (state->mic_task_started) {
        if (!state->mic_started || state->rx_chan == NULL || state->mic_queue == NULL) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        size_t bytes_read = 0u;
        esp_err_t err = i2s_channel_read(state->rx_chan, state->mic_raw_scratch, requested, &bytes_read, pdMS_TO_TICKS(100));
        if (!state->mic_started || err == ESP_ERR_TIMEOUT || bytes_read == 0u) {
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2s mic read failed err=0x%x", (unsigned)err);
            continue;
        }
        h2_audio_frame_t raw_frame = h2_audio_frame_for_buffer(state->mic_raw_scratch, sizeof(state->mic_raw_scratch), raw_format);
        raw_frame.bytes = bytes_read;
        raw_frame.samples_per_channel = (uint16_t)(bytes_read / raw_frame_bytes);

        h2_audio_frame_t processed = h2_audio_frame_for_buffer(state->mic_processed_scratch, sizeof(state->mic_processed_scratch), processed_format);
        int rc = h2_esp_es8311_es7210_audio_system_process_mic(state, &raw_frame, &processed, 0u);
        if (rc != H2_AUDIO_OK || processed.bytes == 0u) {
            continue;
        }
        h2_esp_es8311_es7210_mic_queue_frame_t item;
        item.bytes = processed.bytes;
        item.samples_per_channel = processed.samples_per_channel;
        memcpy(item.samples, processed.data, processed.bytes);
        (void)h2_pal_queue_send_latest(state->config.queue_api, state->mic_queue, &item);
    }

    const int task_with_caps = state->mic_task_with_caps;
    state->mic_task = NULL;
    state->mic_task_with_caps = 0;
    if (task_with_caps) {
        vTaskDeleteWithCaps(NULL);
    } else {
        vTaskDelete(NULL);
    }
}

static int start_mic_task(h2_esp_es8311_es7210_audio_system_t *state) {
    if (state->mic_task_started) {
        return H2_AUDIO_OK;
    }
    if (state->mic_task != NULL) {
        return H2_AUDIO_ERR_INVALID_STATE;
    }
    state->mic_task_started = 1;
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
        mic_task,
        H2_PAL_AUDIO_MIC_TASK_NAME_VALUE,
        state->config.mic_task_stack_size,
        state,
        state->config.mic_task_priority,
        &state->mic_task,
        state->config.mic_task_core_id,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    state->mic_task_with_caps = ok == pdPASS ? 1 : 0;
    if (ok != pdPASS) {
        ok = xTaskCreatePinnedToCore(mic_task, H2_PAL_AUDIO_MIC_TASK_NAME_VALUE, state->config.mic_task_stack_size, state, state->config.mic_task_priority, &state->mic_task, state->config.mic_task_core_id);
        state->mic_task_with_caps = 0;
    }
    if (ok != pdPASS) {
        state->mic_task_started = 0;
        return H2_AUDIO_ERR_NO_MEMORY;
    }
    return H2_AUDIO_OK;
}

static void playback_task(void *arg) {
    h2_esp_es8311_es7210_audio_system_t *state = (h2_esp_es8311_es7210_audio_system_t *)arg;
    for (;;) {
        if (xSemaphoreTake(
                state->write_mutex,
                pdMS_TO_TICKS(H2_ESP_ES8311_ES7210_CONTROL_TIMEOUT_MS)) != pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(H2_ESP_ES8311_ES7210_RETRY_DELAY_MS));
            continue;
        }
        if (!state->playback_started) {
            const int task_with_caps = state->playback_task_with_caps;
            state->playback_task = NULL;
            state->playback_task_with_caps = 0;
            xSemaphoreGive(state->write_mutex);
            if (task_with_caps) {
                vTaskDeleteWithCaps(NULL);
            } else {
                vTaskDelete(NULL);
            }
            return;
        }
        if (state->tx_chan == NULL || !state->mixer_initialized) {
            const int task_with_caps = state->playback_task_with_caps;
            state->playback_started = 0;
            state->playback_task = NULL;
            state->playback_task_with_caps = 0;
            xSemaphoreGive(state->write_mutex);
            ESP_LOGE(TAG, "playback resources disappeared while active");
            if (task_with_caps) {
                vTaskDeleteWithCaps(NULL);
            } else {
                vTaskDelete(NULL);
            }
            return;
        }
        h2_audio_frame_t frame = h2_audio_frame_for_buffer(state->playback_scratch, sizeof(state->playback_scratch), audio_playback_format(state));
        int rc = h2_audio_mixer_read(&state->mixer, &frame);
        xSemaphoreGive(state->write_mutex);
        if (rc != H2_AUDIO_OK || frame.bytes == 0u) {
            vTaskDelay(pdMS_TO_TICKS(H2_ESP_ES8311_ES7210_RETRY_DELAY_MS));
            continue;
        }
        const int16_t *mono = (const int16_t *)frame.data;
        const size_t samples = frame.bytes / sizeof(int16_t);
        for (size_t i = 0u; i < samples; ++i) {
            const int32_t sample = (int32_t)mono[i] << 16;
            state->stereo_scratch[(i * 2u) + 0u] = sample;
            state->stereo_scratch[(i * 2u) + 1u] = sample;
        }
        size_t bytes_written = 0u;
        esp_err_t err = i2s_channel_write(
            state->tx_chan,
            state->stereo_scratch,
            samples * 2u * sizeof(int32_t),
            &bytes_written,
            pdMS_TO_TICKS(H2_ESP_ES8311_ES7210_IO_TIMEOUT_MS));
        if (err != ESP_OK || bytes_written != samples * 2u * sizeof(int32_t)) {
            ESP_LOGW(TAG, "i2s playback write failed err=0x%x bytes=%u/%u",
                (unsigned)err,
                (unsigned)bytes_written,
                (unsigned)(samples * 2u * sizeof(int32_t)));
            vTaskDelay(pdMS_TO_TICKS(H2_ESP_ES8311_ES7210_RETRY_DELAY_MS));
        }
    }
}

static int start_playback_task_locked(h2_esp_es8311_es7210_audio_system_t *state) {
    if (state->playback_task_started) {
        return H2_AUDIO_OK;
    }
    if (state->playback_task != NULL) {
        return H2_AUDIO_ERR_INVALID_STATE;
    }
    state->playback_task_started = 1;
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
        playback_task,
        H2_PAL_AUDIO_MIX_TASK_NAME_VALUE,
        state->config.speaker_task_stack_size,
        state,
        state->config.speaker_task_priority,
        &state->playback_task,
        state->config.speaker_task_core_id,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    state->playback_task_with_caps = ok == pdPASS ? 1 : 0;
    if (ok != pdPASS) {
        state->playback_task = NULL;
        ok = xTaskCreatePinnedToCore(playback_task, H2_PAL_AUDIO_MIX_TASK_NAME_VALUE, state->config.speaker_task_stack_size, state, state->config.speaker_task_priority, &state->playback_task, state->config.speaker_task_core_id);
        state->playback_task_with_caps = 0;
    }
    if (ok != pdPASS) {
        state->playback_task_started = 0;
        state->playback_task = NULL;
        return H2_AUDIO_ERR_NO_MEMORY;
    }
    return H2_AUDIO_OK;
}

static int wait_playback_exit(
    h2_esp_es8311_es7210_audio_system_t *state,
    TickType_t stop_started_at) {
    for (;;) {
        TickType_t remaining = playback_stop_ticks_remaining(stop_started_at);
        const TickType_t wait_ticks = playback_control_wait_ticks(remaining);
        if (xSemaphoreTake(state->write_mutex, wait_ticks) == pdTRUE) {
            const int stopped = state->playback_task == NULL;
            xSemaphoreGive(state->write_mutex);
            if (stopped) {
                return H2_AUDIO_OK;
            }
        }
        remaining = playback_stop_ticks_remaining(stop_started_at);
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

static int audio_get_info(void *user, h2_audio_info_t *info) {
    memset(info, 0, sizeof(*info));
    h2_esp_es8311_es7210_audio_system_t *state = (h2_esp_es8311_es7210_audio_system_t *)user;
    info->available = 1;
    info->mic_supported = 1;
    info->playback_supported = 1;
    info->mic_format = audio_mic_format(state);
    info->playback_format = audio_playback_format(state);
    info->mic_queue_frames = state->config.mic_queue_frames;
    info->track_queue_frames = state->config.track_queue_frames;
    info->max_tracks = state->config.max_tracks;
    return H2_AUDIO_OK;
}

static int audio_read_mic(void *user, h2_audio_frame_t *out_frame, uint32_t timeout_ms) {
    h2_esp_es8311_es7210_audio_system_t *state = (h2_esp_es8311_es7210_audio_system_t *)user;
    if (!state->mic_started || state->mic_queue == NULL) {
        return H2_AUDIO_ERR_INVALID_STATE;
    }
    const h2_audio_pcm_format_t format = audio_mic_format(state);
    if (out_frame->sample_rate_hz != format.sample_rate_hz ||
        out_frame->channels != format.channels ||
        out_frame->sample_format != format.sample_format) {
        return H2_AUDIO_ERR_UNSUPPORTED;
    }
    h2_esp_es8311_es7210_mic_queue_frame_t item;
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

static int audio_start_mic(void *user) {
    h2_esp_es8311_es7210_audio_system_t *state = (h2_esp_es8311_es7210_audio_system_t *)user;
    int rc = audio_open(user);
    if (rc == H2_AUDIO_OK) {
        rc = init_mic_queue(state);
    }
    if (rc == H2_AUDIO_OK && state->config.enable_aec && !state->sr_initialized) {
        rc = h2_esp_es8311_es7210_sr_init(&state->sr, &state->config);
        if (rc == H2_AUDIO_OK) {
            state->sr_initialized = 1;
        }
    }
    if (rc != H2_AUDIO_OK) {
        if (state->mic_queue != NULL) {
            h2_pal_queue_destroy(state->config.queue_api, state->mic_queue);
            state->mic_queue = NULL;
        }
        state->mic_queue_initialized = 0;
        deinit_i2s_if_idle(state);
        return rc;
    }
    (void)h2_pal_queue_reset(state->config.queue_api, state->mic_queue);
    state->mic_started = 1;
    h2_esp_es8311_es7210_sr_reset(&state->sr);
    rc = start_mic_task(state);
    if (rc != H2_AUDIO_OK) {
        state->mic_started = 0;
        if (state->mic_queue != NULL) {
            h2_pal_queue_destroy(state->config.queue_api, state->mic_queue);
            state->mic_queue = NULL;
        }
        state->mic_queue_initialized = 0;
        deinit_i2s_if_idle(state);
    }
    return rc;
}

static int audio_stop_mic(void *user) {
    h2_esp_es8311_es7210_audio_system_t *state = (h2_esp_es8311_es7210_audio_system_t *)user;
    state->mic_started = 0;
    state->mic_task_started = 0;
    if (!wait_task_exit(&state->mic_task)) {
        return H2_AUDIO_ERR_INVALID_STATE;
    }
    if (state->mic_queue != NULL) {
        (void)h2_pal_queue_reset(state->config.queue_api, state->mic_queue);
        h2_pal_queue_destroy(state->config.queue_api, state->mic_queue);
        state->mic_queue = NULL;
    }
    state->mic_queue_initialized = 0;
    deinit_i2s_if_idle(state);
    return H2_AUDIO_OK;
}

static int audio_start_speaker(void *user) {
    h2_esp_es8311_es7210_audio_system_t *state = (h2_esp_es8311_es7210_audio_system_t *)user;
    int rc = audio_open(user);
    if (rc != H2_AUDIO_OK) {
        return rc;
    }
    if (xSemaphoreTake(
            state->write_mutex,
            pdMS_TO_TICKS(H2_ESP_ES8311_ES7210_CONTROL_TIMEOUT_MS)) != pdTRUE) {
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

    rc = init_mixer(state);
    if (rc == H2_AUDIO_OK) {
        rc = set_pa(state, 1);
    }
    if (rc != H2_AUDIO_OK) {
        if (state->mixer_initialized) {
            h2_audio_mixer_deinit(&state->mixer);
            state->mixer_initialized = 0;
        }
        (void)set_pa(state, 0);
        deinit_i2s_if_idle(state);
        return rc;
    }
    if (xSemaphoreTake(
            state->write_mutex,
            pdMS_TO_TICKS(H2_ESP_ES8311_ES7210_CONTROL_TIMEOUT_MS)) != pdTRUE) {
        h2_audio_mixer_deinit(&state->mixer);
        state->mixer_initialized = 0;
        (void)set_pa(state, 0);
        deinit_i2s_if_idle(state);
        return H2_AUDIO_ERR_WOULD_BLOCK;
    }
    state->playback_started = 1;
    rc = start_playback_task_locked(state);
    if (rc != H2_AUDIO_OK) {
        state->playback_started = 0;
    }
    xSemaphoreGive(state->write_mutex);
    if (rc != H2_AUDIO_OK) {
        if (state->mixer_initialized) {
            h2_audio_mixer_deinit(&state->mixer);
            state->mixer_initialized = 0;
        }
        (void)set_pa(state, 0);
        deinit_i2s_if_idle(state);
    }
    return rc;
}

static int audio_stop_speaker(void *user) {
    h2_esp_es8311_es7210_audio_system_t *state = (h2_esp_es8311_es7210_audio_system_t *)user;
    if (state->write_mutex == NULL) {
        state->playback_started = 0;
        state->playback_task_started = 0;
        state->playback_task = NULL;
        return state->pa_initialized ? set_pa(state, 0) : H2_AUDIO_OK;
    }
    const TickType_t stop_started_at = xTaskGetTickCount();
    int first_rc = H2_AUDIO_OK;
    int stop_published = 0;

    const TickType_t remaining = playback_stop_ticks_remaining(stop_started_at);
    if (xSemaphoreTake(
            state->write_mutex,
            playback_control_wait_ticks(remaining)) == pdTRUE) {
        state->playback_started = 0;
        stop_published = 1;
        xSemaphoreGive(state->write_mutex);
    } else {
        first_rc = H2_AUDIO_ERR_WOULD_BLOCK;
    }

    const int pa_rc = set_pa(state, 0);
    if (first_rc == H2_AUDIO_OK && pa_rc != H2_AUDIO_OK) {
        first_rc = pa_rc;
    }
    int join_rc = H2_AUDIO_ERR_WOULD_BLOCK;
    if (stop_published) {
        join_rc = wait_playback_exit(state, stop_started_at);
        if (first_rc == H2_AUDIO_OK && join_rc != H2_AUDIO_OK) {
            first_rc = join_rc;
        }
    }
    if (stop_published && join_rc == H2_AUDIO_OK) {
        TickType_t cleanup_remaining = playback_stop_ticks_remaining(stop_started_at);
        if (xSemaphoreTake(
                state->write_mutex,
                playback_control_wait_ticks(cleanup_remaining)) == pdTRUE) {
            state->playback_task_started = 0;
            if (state->mixer_initialized) {
                h2_audio_mixer_deinit(&state->mixer);
                state->mixer_initialized = 0;
            }
            xSemaphoreGive(state->write_mutex);
            deinit_i2s_if_idle(state);
        } else if (first_rc == H2_AUDIO_OK) {
            first_rc = H2_AUDIO_ERR_WOULD_BLOCK;
        }
    }
    return first_rc;
}

int h2_esp_es8311_es7210_audio_system_deinit(
    h2_esp_es8311_es7210_audio_system_t *system) {
    if (system == NULL) {
        return H2_AUDIO_ERR_INVALID_ARG;
    }
    if (system->config.queue_api == NULL) {
        memset(system, 0, sizeof(*system));
        return H2_AUDIO_OK;
    }

    int rc = audio_stop_mic(system);
    if (rc != H2_AUDIO_OK) {
        return rc;
    }
    rc = audio_stop_speaker(system);
    if (rc != H2_AUDIO_OK) {
        return rc;
    }
    if (system->mic_task != NULL || system->playback_task != NULL) {
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
    deinit_i2s_if_idle(system);
    if (system->tx_chan != NULL || system->rx_chan != NULL ||
        system->write_mutex != NULL || system->es8311 != NULL ||
        system->es7210 != NULL) {
        return H2_AUDIO_ERR_WOULD_BLOCK;
    }
    if (system->sr_initialized) {
        h2_esp_es8311_es7210_sr_deinit(&system->sr);
    }
    memset(system, 0, sizeof(*system));
    return H2_AUDIO_OK;
}

static int audio_create_track(void *user, const h2_audio_track_config_t *config, h2_pal_audio_track_t **out_track) {
    h2_esp_es8311_es7210_audio_system_t *state = (h2_esp_es8311_es7210_audio_system_t *)user;
    const h2_audio_pcm_format_t format = audio_playback_format(state);
    if (config->format.sample_rate_hz != format.sample_rate_hz ||
        config->format.channels != format.channels ||
        config->format.sample_format != format.sample_format) {
        return H2_AUDIO_ERR_UNSUPPORTED;
    }
    int rc = init_mixer(state);
    if (rc != H2_AUDIO_OK) {
        return rc;
    }
    return h2_audio_mixer_create_track(&state->mixer, &state->audio, config, out_track);
}

static int audio_get_speaker_volume_percent(void *user, uint32_t *out_percent) {
    h2_esp_es8311_es7210_audio_system_t *state = (h2_esp_es8311_es7210_audio_system_t *)user;
    *out_percent = state->speaker_volume_percent;
    return H2_AUDIO_OK;
}

static int audio_set_speaker_volume_percent(void *user, uint32_t percent) {
    h2_esp_es8311_es7210_audio_system_t *state = (h2_esp_es8311_es7210_audio_system_t *)user;
    state->speaker_volume_percent = percent;
    if (state->es8311 == NULL) {
        return H2_AUDIO_OK;
    }
    return map_esp_err(write_reg(state->es8311, ES8311_REG_DAC_32, volume_from_percent(percent, state->config.codec_volume_default)));
}

h2_pal_audio_t *h2_esp_es8311_es7210_audio_system_audio(h2_esp_es8311_es7210_audio_system_t *system) {
    if (system == NULL) {
        return NULL;
    }
    static const h2_pal_audio_vtable_t vtable = {
        .get_info = audio_get_info,
        .start_mic = audio_start_mic,
        .stop_mic = audio_stop_mic,
        .start_speaker = audio_start_speaker,
        .stop_speaker = audio_stop_speaker,
        .mic_read = audio_read_mic,
        .create_track = audio_create_track,
        .get_speaker_volume_percent = audio_get_speaker_volume_percent,
        .set_speaker_volume_percent = audio_set_speaker_volume_percent,
    };
    system->audio.user = system;
    system->audio.vtable = &vtable;
    return &system->audio;
}
