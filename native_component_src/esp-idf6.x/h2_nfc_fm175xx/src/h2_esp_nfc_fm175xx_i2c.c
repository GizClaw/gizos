#include "h2_esp_nfc_fm175xx_internal.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static int i2c_write_reg(void *user, uint8_t reg, uint8_t value) {
    i2c_master_dev_handle_t dev = (i2c_master_dev_handle_t)user;
    const uint8_t data[2] = {reg, value};
    return i2c_master_transmit(dev, data, sizeof(data), pdMS_TO_TICKS(20)) == ESP_OK
        ? H2_FM175XX_OK
        : H2_FM175XX_ERR_IO;
}

static int i2c_write_regs(void *user, uint8_t reg, const uint8_t *data, size_t len) {
    if (data == NULL && len > 0u) {
        return H2_FM175XX_ERR_INVALID_ARG;
    }
    uint8_t buf[33];
    if (len + 1u > sizeof(buf)) {
        return H2_FM175XX_ERR_INVALID_ARG;
    }
    buf[0] = reg;
    if (len > 0u) {
        memcpy(buf + 1, data, len);
    }
    return i2c_master_transmit(
               (i2c_master_dev_handle_t)user,
               buf,
               len + 1u,
               pdMS_TO_TICKS(20)) == ESP_OK
        ? H2_FM175XX_OK
        : H2_FM175XX_ERR_IO;
}

static int i2c_read_reg(void *user, uint8_t reg, uint8_t *out_value) {
    if (out_value == NULL) {
        return H2_FM175XX_ERR_INVALID_ARG;
    }
    return i2c_master_transmit_receive(
               (i2c_master_dev_handle_t)user,
               &reg,
               1u,
               out_value,
               1u,
               pdMS_TO_TICKS(20)) == ESP_OK
        ? H2_FM175XX_OK
        : H2_FM175XX_ERR_IO;
}

static int i2c_read_regs(void *user, uint8_t reg, uint8_t *out_data, size_t len) {
    if (out_data == NULL && len > 0u) {
        return H2_FM175XX_ERR_INVALID_ARG;
    }
    return i2c_master_transmit_receive(
               (i2c_master_dev_handle_t)user,
               &reg,
               1u,
               out_data,
               len,
               pdMS_TO_TICKS(20)) == ESP_OK
        ? H2_FM175XX_OK
        : H2_FM175XX_ERR_IO;
}

static void i2c_sleep_ms(void *user, uint32_t ms) {
    (void)user;
    vTaskDelay(pdMS_TO_TICKS(ms));
}

int h2_esp_nfc_fm175xx_i2c_open(
    h2_esp_nfc_fm175xx_t *adapter,
    h2_fm175xx_transport_t *out_transport) {
    if (adapter == NULL || out_transport == NULL ||
        adapter->config.bus == NULL || adapter->config.i2c_address == 0u) {
        return H2_FM175XX_ERR_INVALID_ARG;
    }
    const i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = adapter->config.i2c_address,
        .scl_speed_hz = adapter->config.i2c_speed_hz,
    };
    if (i2c_master_bus_add_device(
            adapter->config.bus, &dev_config, &adapter->dev) != ESP_OK) {
        return H2_FM175XX_ERR_IO;
    }
    *out_transport = (h2_fm175xx_transport_t){
        .user = adapter->dev,
        .write_reg = i2c_write_reg,
        .write_regs = i2c_write_regs,
        .read_reg = i2c_read_reg,
        .read_regs = i2c_read_regs,
        .sleep_ms = i2c_sleep_ms,
    };
    return H2_FM175XX_OK;
}

void h2_esp_nfc_fm175xx_i2c_close(h2_esp_nfc_fm175xx_t *adapter) {
    if (adapter != NULL && adapter->dev != NULL) {
        (void)i2c_master_bus_rm_device(adapter->dev);
        adapter->dev = NULL;
    }
}
