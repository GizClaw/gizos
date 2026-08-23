#include "h2_esp_board_private.h"
#include "h2_esp_board_internal.h"

#include "h2_esp_platform_core.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_timer.h"

#include <stdint.h>

#define H2_AMOLED_DISPLAY_CS_GPIO GPIO_NUM_12
#define H2_AMOLED_DISPLAY_PCLK_GPIO GPIO_NUM_11
#define H2_AMOLED_DISPLAY_DATA0_GPIO GPIO_NUM_4
#define H2_AMOLED_DISPLAY_DATA1_GPIO GPIO_NUM_5
#define H2_AMOLED_DISPLAY_DATA2_GPIO GPIO_NUM_6
#define H2_AMOLED_DISPLAY_DATA3_GPIO GPIO_NUM_7

#define H2_AMOLED_AUDIO_MCLK_GPIO GPIO_NUM_16
#define H2_AMOLED_AUDIO_BCLK_GPIO GPIO_NUM_9
#define H2_AMOLED_AUDIO_WS_GPIO GPIO_NUM_45
#define H2_AMOLED_AUDIO_DOUT_GPIO GPIO_NUM_8
#define H2_AMOLED_AUDIO_DIN_GPIO GPIO_NUM_10
#define H2_AMOLED_AUDIO_PA_GPIO GPIO_NUM_46

#define H2_AMOLED_AXP2101_ADDRESS 0x34u
#define H2_AMOLED_AXP2101_SPEED_HZ 400000u
#define H2_AMOLED_AXP2101_TIMEOUT_MS 100
#define H2_AMOLED_AXP2101_IC_TYPE_REG 0x03u
#define H2_AMOLED_AXP2101_IC_TYPE 0x4au
#define H2_AMOLED_AXP2101_COMMON_CONFIG_REG 0x10u
#define H2_AMOLED_AXP2101_SOFT_OFF_MASK 0x01u
#define H2_AMOLED_AXP2101_PWROFF_ENABLE_REG 0x22u
#define H2_AMOLED_AXP2101_LONG_PRESS_POWER_OFF_MASK 0x02u
#define H2_AMOLED_AXP2101_LONG_PRESS_RESTART_MASK 0x01u
#define H2_AMOLED_AXP2101_KEY_LEVEL_CTRL_REG 0x27u
#define H2_AMOLED_AXP2101_POWER_ON_TIME_MASK 0x03u
#define H2_AMOLED_AXP2101_POWER_ON_TIME_2S 0x03u
#define H2_AMOLED_AXP2101_POWER_OFF_TIME_MASK 0x0cu
#define H2_AMOLED_AXP2101_POWER_OFF_TIME_4S 0x00u
#define H2_AMOLED_AXP2101_IRQ_ENABLE_2_REG 0x41u
#define H2_AMOLED_AXP2101_IRQ_STATUS_2_REG 0x49u
#define H2_AMOLED_AXP2101_POWER_KEY_POSITIVE_EDGE_MASK 0x01u
#define H2_AMOLED_AXP2101_POWER_KEY_NEGATIVE_EDGE_MASK 0x02u
#define H2_AMOLED_AXP2101_POWER_KEY_EDGE_MASK \
    (H2_AMOLED_AXP2101_POWER_KEY_POSITIVE_EDGE_MASK | \
     H2_AMOLED_AXP2101_POWER_KEY_NEGATIVE_EDGE_MASK)
#define H2_AMOLED_AXP2101_POWER_KEY_DEBOUNCE_US 30000u

static i2c_master_dev_handle_t s_axp2101_device;
static const h2_pal_power_api_t *s_platform_power;
static h2_pal_power_vtable_t s_board_power_vtable;
static h2_pal_power_api_t s_board_power_api;
static int s_axp2101_power_key_configured;
static int s_axp2101_power_key_pressed;
static int s_axp2101_power_key_candidate;
static uint64_t s_axp2101_power_key_candidate_since_us;

static void drive_gpio_group_low(uint64_t mask) {
    const gpio_config_t config = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (mask == 0u || gpio_config(&config) != ESP_OK) {
        return;
    }
    for (int gpio = 0; gpio < GPIO_NUM_MAX && gpio < 64; ++gpio) {
        if ((mask & (1ULL << (uint32_t)gpio)) != 0u) {
            (void)gpio_set_level((gpio_num_t)gpio, 0);
        }
    }
}

h2_pal_result_t h2_esp_platform_power_before_reboot(uint32_t reason) {
    (void)reason;

    (void)h2_esp_board_display_power_off();

    h2_pal_audio_t *audio = h2_esp_board_audio_if_initialized();
    if (audio != NULL) {
        (void)h2_pal_audio_stop_mic(audio);
        (void)h2_pal_audio_stop_speaker(audio);
    }

    drive_gpio_group_low(
        (1ULL << H2_AMOLED_DISPLAY_CS_GPIO) |
        (1ULL << H2_AMOLED_DISPLAY_PCLK_GPIO) |
        (1ULL << H2_AMOLED_DISPLAY_DATA0_GPIO) |
        (1ULL << H2_AMOLED_DISPLAY_DATA1_GPIO) |
        (1ULL << H2_AMOLED_DISPLAY_DATA2_GPIO) |
        (1ULL << H2_AMOLED_DISPLAY_DATA3_GPIO));
    drive_gpio_group_low(
        (1ULL << H2_AMOLED_AUDIO_MCLK_GPIO) |
        (1ULL << H2_AMOLED_AUDIO_BCLK_GPIO) |
        (1ULL << H2_AMOLED_AUDIO_WS_GPIO) |
        (1ULL << H2_AMOLED_AUDIO_DOUT_GPIO) |
        (1ULL << H2_AMOLED_AUDIO_DIN_GPIO) |
        (1ULL << H2_AMOLED_AUDIO_PA_GPIO));
    return H2_PAL_OK;
}

static h2_pal_result_t axp2101_result(esp_err_t err) {
    if (err == ESP_OK) {
        return H2_PAL_OK;
    }
    if (err == ESP_ERR_NO_MEM) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (err == ESP_ERR_INVALID_ARG) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (err == ESP_ERR_TIMEOUT) {
        return H2_PAL_ERR_TIMEOUT;
    }
    return H2_PAL_ERR_IO;
}

static h2_pal_result_t axp2101_ensure_device(void) {
    if (s_axp2101_device != NULL) {
        return H2_PAL_OK;
    }
    i2c_master_bus_handle_t bus = h2_esp_amoled_board_i2c_bus();
    if (bus == NULL) {
        return H2_PAL_ERR_UNAVAILABLE;
    }
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = H2_AMOLED_AXP2101_ADDRESS,
        .scl_speed_hz = H2_AMOLED_AXP2101_SPEED_HZ,
    };
    const esp_err_t err = i2c_master_bus_add_device(
        bus, &config, &s_axp2101_device);
    if (err != ESP_OK) {
        s_axp2101_device = NULL;
    }
    return axp2101_result(err);
}

static h2_pal_result_t axp2101_read_register(
    uint8_t reg,
    uint8_t *out_value) {
    if (out_value == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_value = 0u;
    h2_pal_result_t rc = axp2101_ensure_device();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return axp2101_result(i2c_master_transmit_receive(
        s_axp2101_device,
        &reg,
        sizeof(reg),
        out_value,
        sizeof(*out_value),
        H2_AMOLED_AXP2101_TIMEOUT_MS));
}

static h2_pal_result_t axp2101_write_register(
    uint8_t reg,
    uint8_t value) {
    h2_pal_result_t rc = axp2101_ensure_device();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    const uint8_t data[] = {reg, value};
    return axp2101_result(i2c_master_transmit(
        s_axp2101_device,
        data,
        sizeof(data),
        H2_AMOLED_AXP2101_TIMEOUT_MS));
}

static h2_pal_result_t axp2101_validate_chip(void) {
    uint8_t chip_id = 0u;
    h2_pal_result_t rc = axp2101_read_register(
        H2_AMOLED_AXP2101_IC_TYPE_REG, &chip_id);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return chip_id == H2_AMOLED_AXP2101_IC_TYPE
        ? H2_PAL_OK
        : H2_PAL_ERR_INVALID_STATE;
}

static h2_pal_result_t axp2101_configure_power_key(void) {
    if (s_axp2101_power_key_configured) {
        return H2_PAL_OK;
    }
    h2_pal_result_t rc = axp2101_validate_chip();
    if (rc != H2_PAL_OK) {
        return rc;
    }

    uint8_t key_config = 0u;
    rc = axp2101_read_register(
        H2_AMOLED_AXP2101_KEY_LEVEL_CTRL_REG, &key_config);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    key_config = (uint8_t)(
        (key_config & (uint8_t)~(
            H2_AMOLED_AXP2101_POWER_ON_TIME_MASK |
            H2_AMOLED_AXP2101_POWER_OFF_TIME_MASK)) |
        H2_AMOLED_AXP2101_POWER_ON_TIME_2S |
        H2_AMOLED_AXP2101_POWER_OFF_TIME_4S);
    rc = axp2101_write_register(
        H2_AMOLED_AXP2101_KEY_LEVEL_CTRL_REG, key_config);
    if (rc != H2_PAL_OK) {
        return rc;
    }

    uint8_t power_off_config = 0u;
    rc = axp2101_read_register(
        H2_AMOLED_AXP2101_PWROFF_ENABLE_REG, &power_off_config);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    power_off_config = (uint8_t)(
        (power_off_config | H2_AMOLED_AXP2101_LONG_PRESS_POWER_OFF_MASK) &
        (uint8_t)~H2_AMOLED_AXP2101_LONG_PRESS_RESTART_MASK);
    rc = axp2101_write_register(
        H2_AMOLED_AXP2101_PWROFF_ENABLE_REG, power_off_config);
    if (rc != H2_PAL_OK) {
        return rc;
    }

    uint8_t irq_enable = 0u;
    rc = axp2101_read_register(
        H2_AMOLED_AXP2101_IRQ_ENABLE_2_REG, &irq_enable);
    if (rc == H2_PAL_OK) {
        rc = axp2101_write_register(
            H2_AMOLED_AXP2101_IRQ_ENABLE_2_REG,
            irq_enable | H2_AMOLED_AXP2101_POWER_KEY_EDGE_MASK);
    }
    if (rc == H2_PAL_OK) {
        rc = axp2101_write_register(
            H2_AMOLED_AXP2101_IRQ_STATUS_2_REG,
            H2_AMOLED_AXP2101_POWER_KEY_EDGE_MASK);
    }
    if (rc != H2_PAL_OK) {
        return rc;
    }

    uint8_t key_verify = 0u;
    uint8_t power_off_verify = 0u;
    uint8_t irq_enable_verify = 0u;
    rc = axp2101_read_register(
        H2_AMOLED_AXP2101_KEY_LEVEL_CTRL_REG, &key_verify);
    if (rc == H2_PAL_OK) {
        rc = axp2101_read_register(
            H2_AMOLED_AXP2101_PWROFF_ENABLE_REG, &power_off_verify);
    }
    if (rc == H2_PAL_OK) {
        rc = axp2101_read_register(
            H2_AMOLED_AXP2101_IRQ_ENABLE_2_REG, &irq_enable_verify);
    }
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if ((key_verify & (H2_AMOLED_AXP2101_POWER_ON_TIME_MASK |
                       H2_AMOLED_AXP2101_POWER_OFF_TIME_MASK)) !=
            (H2_AMOLED_AXP2101_POWER_ON_TIME_2S |
             H2_AMOLED_AXP2101_POWER_OFF_TIME_4S) ||
        (power_off_verify & H2_AMOLED_AXP2101_LONG_PRESS_POWER_OFF_MASK) ==
            0u ||
        (power_off_verify & H2_AMOLED_AXP2101_LONG_PRESS_RESTART_MASK) !=
            0u ||
        (irq_enable_verify & H2_AMOLED_AXP2101_POWER_KEY_EDGE_MASK) !=
            H2_AMOLED_AXP2101_POWER_KEY_EDGE_MASK) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    s_axp2101_power_key_configured = 1;
    return H2_PAL_OK;
}

h2_pal_result_t h2_esp_amoled_board_power_button_read(int *out_pressed) {
    if (out_pressed == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc = axp2101_configure_power_key();
    if (rc != H2_PAL_OK) {
        return rc;
    }

    uint8_t status = 0u;
    rc = axp2101_read_register(
        H2_AMOLED_AXP2101_IRQ_STATUS_2_REG, &status);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    const uint8_t edges = status & H2_AMOLED_AXP2101_POWER_KEY_EDGE_MASK;
    int candidate = s_axp2101_power_key_candidate;
    if ((edges & H2_AMOLED_AXP2101_POWER_KEY_POSITIVE_EDGE_MASK) != 0u) {
        candidate = 0;
    } else if ((edges & H2_AMOLED_AXP2101_POWER_KEY_NEGATIVE_EDGE_MASK) != 0u) {
        candidate = 1;
    }
    if (edges != 0u) {
        rc = axp2101_write_register(
            H2_AMOLED_AXP2101_IRQ_STATUS_2_REG, edges);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    if (candidate != s_axp2101_power_key_candidate) {
        s_axp2101_power_key_candidate = candidate;
        s_axp2101_power_key_candidate_since_us = now_us;
    } else if (candidate != s_axp2101_power_key_pressed &&
               now_us - s_axp2101_power_key_candidate_since_us >=
                   H2_AMOLED_AXP2101_POWER_KEY_DEBOUNCE_US) {
        s_axp2101_power_key_pressed = candidate;
    }
    *out_pressed = s_axp2101_power_key_pressed;
    return H2_PAL_OK;
}

static h2_pal_result_t board_power_get_capabilities(
    void *user,
    h2_pal_power_capabilities_t *out_capabilities) {
    h2_pal_result_t rc = s_platform_power->vtable->get_capabilities(
        user, out_capabilities);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = axp2101_configure_power_key();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    out_capabilities->flags |= H2_PAL_POWER_CAPABILITY_SHUTDOWN;
    return H2_PAL_OK;
}

static h2_pal_result_t board_power_shutdown(void *user, uint32_t reason) {
    (void)user;
    h2_pal_result_t rc = axp2101_validate_chip();
    if (rc != H2_PAL_OK) {
        return rc;
    }

    uint8_t config = 0u;
    rc = axp2101_read_register(
        H2_AMOLED_AXP2101_COMMON_CONFIG_REG, &config);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_esp_platform_power_before_reboot(reason);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return axp2101_write_register(
        H2_AMOLED_AXP2101_COMMON_CONFIG_REG,
        config | H2_AMOLED_AXP2101_SOFT_OFF_MASK);
}

const h2_pal_power_api_t *h2_esp_board_power_api(void) {
    if (s_board_power_api.vtable == NULL) {
        s_platform_power = h2_esp_platform_power_api();
        if (s_platform_power == NULL || s_platform_power->vtable == NULL ||
            s_platform_power->vtable->get_capabilities == NULL) {
            return s_platform_power;
        }
        s_board_power_vtable = *s_platform_power->vtable;
        s_board_power_vtable.get_capabilities =
            board_power_get_capabilities;
        s_board_power_vtable.shutdown = board_power_shutdown;
        s_board_power_api = (h2_pal_power_api_t){
            .user = s_platform_power->user,
            .vtable = &s_board_power_vtable,
        };
    }
    return &s_board_power_api;
}
