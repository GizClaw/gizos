#include "h2_esp_board_internal.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdint.h>

#define H2_AMOLED_I2C_PORT 0
#define H2_AMOLED_I2C_SDA_GPIO GPIO_NUM_15
#define H2_AMOLED_I2C_SCL_GPIO GPIO_NUM_14
#define H2_AMOLED_IO_EXPANDER_ADDRESS 0x20u
#define H2_AMOLED_IO_EXPANDER_SPEED_HZ 100000u
#define H2_AMOLED_IO_EXPANDER_TIMEOUT_MS 100
#define H2_AMOLED_IO_EXPANDER_INPUT_REG 0x00u
#define H2_AMOLED_IO_EXPANDER_OUTPUT_REG 0x01u
#define H2_AMOLED_IO_EXPANDER_CONFIG_REG 0x03u

static const char *TAG = "h2_amoled_i2c";
static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_io_expander;
static StaticSemaphore_t s_io_lock_storage;
static SemaphoreHandle_t s_io_lock;

i2c_master_bus_handle_t h2_esp_amoled_board_i2c_bus(void) {
    if (s_i2c_bus != NULL) {
        return s_i2c_bus;
    }

    const i2c_master_bus_config_t bus_config = {
        .i2c_port = H2_AMOLED_I2C_PORT,
        .sda_io_num = H2_AMOLED_I2C_SDA_GPIO,
        .scl_io_num = H2_AMOLED_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,
        },
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &s_i2c_bus);
    if (err == ESP_ERR_INVALID_STATE) {
        err = i2c_master_get_bus_handle(H2_AMOLED_I2C_PORT, &s_i2c_bus);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "shared i2c bus init failed: %s", esp_err_to_name(err));
        s_i2c_bus = NULL;
        return NULL;
    }
    if (s_io_lock == NULL) {
        s_io_lock = xSemaphoreCreateMutexStatic(&s_io_lock_storage);
    }
    return s_i2c_bus;
}

static esp_err_t ensure_io_expander(i2c_master_bus_handle_t bus) {
    if (s_io_expander != NULL) {
        return ESP_OK;
    }
    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = H2_AMOLED_IO_EXPANDER_ADDRESS,
        .scl_speed_hz = H2_AMOLED_IO_EXPANDER_SPEED_HZ,
    };
    s_io_expander = NULL;
    esp_err_t err = i2c_master_bus_add_device(
        bus, &device_config, &s_io_expander);
    if (err != ESP_OK) {
        s_io_expander = NULL;
    }
    return err;
}

static esp_err_t io_read_register(uint8_t reg, uint8_t *out_value) {
    return i2c_master_transmit_receive(
        s_io_expander,
        &reg,
        sizeof(reg),
        out_value,
        sizeof(*out_value),
        H2_AMOLED_IO_EXPANDER_TIMEOUT_MS);
}

static esp_err_t io_write_register(uint8_t reg, uint8_t value) {
    const uint8_t data[] = {reg, value};
    return i2c_master_transmit(
        s_io_expander,
        data,
        sizeof(data),
        H2_AMOLED_IO_EXPANDER_TIMEOUT_MS);
}

esp_err_t h2_esp_amoled_board_io_update_outputs(
    uint8_t mask,
    uint8_t value) {
    i2c_master_bus_handle_t bus = h2_esp_amoled_board_i2c_bus();
    if (bus == NULL || s_io_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_io_lock, pdMS_TO_TICKS(100u)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ensure_io_expander(bus);
    uint8_t output = 0u;
    if (err == ESP_OK) {
        err = io_read_register(
            H2_AMOLED_IO_EXPANDER_OUTPUT_REG, &output);
    }
    if (err == ESP_OK) {
        output = (uint8_t)((output & (uint8_t)~mask) | (value & mask));
        err = io_write_register(
            H2_AMOLED_IO_EXPANDER_OUTPUT_REG, output);
    }

    uint8_t config = 0u;
    if (err == ESP_OK) {
        err = io_read_register(
            H2_AMOLED_IO_EXPANDER_CONFIG_REG, &config);
    }
    if (err == ESP_OK) {
        config &= (uint8_t)~mask;
        err = io_write_register(
            H2_AMOLED_IO_EXPANDER_CONFIG_REG, config);
    }
    xSemaphoreGive(s_io_lock);
    return err;
}

esp_err_t h2_esp_amoled_board_io_read_inputs(uint8_t *out_value) {
    if (out_value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_value = 0u;
    i2c_master_bus_handle_t bus = h2_esp_amoled_board_i2c_bus();
    if (bus == NULL || s_io_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_io_lock, pdMS_TO_TICKS(100u)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ensure_io_expander(bus);
    if (err == ESP_OK) {
        err = io_read_register(
            H2_AMOLED_IO_EXPANDER_INPUT_REG, out_value);
    }
    xSemaphoreGive(s_io_lock);
    return err;
}
