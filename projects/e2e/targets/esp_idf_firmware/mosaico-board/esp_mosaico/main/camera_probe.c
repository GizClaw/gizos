#include "diagnostics.h"
#include "bsp/subboard.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

/* ID-only probe for a camera installed in the LEFT slot. No DVP streaming,
 * EEPROM writes, sensor format programming, or right-slot GPIO loopback.
 * Register protocols and power levels follow esp_cam_sensor OV3640/SC101IOT. */
static esp_err_t read_id(i2c_master_bus_handle_t bus, bool sc, uint16_t *id) {
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = sc ? 0x68 : 0x3c,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t rc = i2c_master_bus_add_device(bus, &cfg, &dev);
    if (rc != ESP_OK) return rc;
    uint8_t bytes[2] = {0};
    for (unsigned i = 0; i < 2 && rc == ESP_OK; ++i) {
        if (sc) {
            const uint8_t page[] = {0xf0, 0x31};
            const uint8_t reg = 0xf7 + i;
            rc = i2c_master_transmit(dev, page, sizeof(page), 100);
            if (rc == ESP_OK) rc = i2c_master_transmit_receive(dev, &reg, 1, &bytes[i], 1, 100);
        } else {
            const uint8_t reg[] = {0x30, 0x0a + i};
            rc = i2c_master_transmit_receive(dev, reg, sizeof(reg), &bytes[i], 1, 100);
        }
    }
    const esp_err_t cleanup = i2c_master_bus_rm_device(dev);
    if (rc == ESP_OK) rc = cleanup;
    if (rc == ESP_OK) *id = ((uint16_t)bytes[0] << 8) | bytes[1];
    return rc;
}

void mosaico_camera_probe(void) {
    esp_err_t rc = bsp_subboard_init();
    if (rc != ESP_OK) {
        snprintf(mosaico_diagnostics.camera, sizeof(mosaico_diagnostics.camera), "CAM BUS ERROR:%d", rc);
        printf("H2_MOSAICO_CAMERA init_rc=%d\n", rc);
        return;
    }
    i2c_master_bus_handle_t bus = bsp_subboard_get_i2c_bus();
    for (unsigned addr = 0x50; addr <= 0x51; ++addr) {
        const esp_err_t probe = i2c_master_probe(bus, addr, 100);
        printf("H2_MOSAICO_SLOT side=%s addr=0x%02x probe_rc=%d\n", addr == 0x50 ? "left" : "right", addr, probe);
        if (probe != ESP_OK) continue;
        const i2c_device_config_t cfg = {.dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr, .scl_speed_hz = 100000};
        i2c_master_dev_handle_t dev = NULL;
        esp_err_t er = i2c_master_bus_add_device(bus, &cfg, &dev);
        uint8_t offset = 0, bytes[16] = {0};
        if (er == ESP_OK) er = i2c_master_transmit_receive(dev, &offset, 1, bytes, sizeof(bytes), 100);
        printf("H2_MOSAICO_EEPROM addr=0x%02x read_rc=%d data=", addr, er);
        if (er == ESP_OK) for (unsigned i = 0; i < sizeof(bytes); ++i) printf("%02x", bytes[i]);
        printf("\n");
        if (dev) printf("H2_MOSAICO_EEPROM cleanup_rc=%d\n", i2c_master_bus_rm_device(dev));
    }
    const gpio_config_t pins = {.pin_bit_mask = (1ULL << 48) | (1ULL << 53), .mode = GPIO_MODE_OUTPUT};
    rc = gpio_config(&pins);
    if (rc == ESP_OK) rc = gpio_set_level(GPIO_NUM_53, 1);
    bool identified = false;
    for (unsigned candidate = 0; candidate < 2 && rc == ESP_OK; ++candidate) {
        const bool sc = candidate == 1;
        rc = gpio_set_level(GPIO_NUM_48, sc ? 1 : 0);
        if (rc != ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(30));
        uint16_t first = 0, second = 0;
        esp_err_t read_rc = read_id(bus, sc, &first);
        if (read_rc == ESP_OK) read_rc = read_id(bus, sc, &second);
        const bool match = read_rc == ESP_OK && first == second &&
            (sc ? first == 0xda4a : (first == 0x364c || first == 0x3641));
        printf("H2_MOSAICO_CAMERA candidate=%s addr=0x%02x rc=%d pid=0x%04x repeat=0x%04x matched=%d\n",
               sc ? "SC101IOT" : "OV3640", sc ? 0x68 : 0x3c, read_rc, first, second, match);
        if (match) {
            snprintf(mosaico_diagnostics.camera, sizeof(mosaico_diagnostics.camera),
                     "CAM:%s ID:%04X", sc ? "SC101IOT" : "OV3640", first);
            identified = true;
            /* Sensor-specific inactive level; release GPIOs below. */
            rc = gpio_set_level(GPIO_NUM_48, sc ? 0 : 1);
            break;
        }
    }
    if (!identified) snprintf(mosaico_diagnostics.camera, sizeof(mosaico_diagnostics.camera), "CAM:UNIDENTIFIED RC:%d", rc);
    const esp_err_t reset_cleanup = gpio_reset_pin(GPIO_NUM_53);
    const esp_err_t pwdn_cleanup = gpio_reset_pin(GPIO_NUM_48);
    printf("H2_MOSAICO_CAMERA_SUMMARY %s gpio_rc=%d cleanup=%d/%d capture=NOT_TESTED\n",
           mosaico_diagnostics.camera, rc, reset_cleanup, pwdn_cleanup);
}
