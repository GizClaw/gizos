#ifndef H2_ESP_NFC_FM175XX_H
#define H2_ESP_NFC_FM175XX_H

#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "h2_fm175xx.h"
#include "h2/pal/hal/h2_pal_input.h"

#ifdef __cplusplus
extern "C" {
#endif

#define H2_ESP_NFC_FM175XX_DEFAULT_READ_CAPACITY 256u

typedef struct h2_esp_nfc_fm175xx_config {
    h2_pal_periph_id_t id;
    i2c_master_bus_handle_t bus;
    uint16_t i2c_address;
    uint32_t i2c_speed_hz;
    size_t read_capacity;
} h2_esp_nfc_fm175xx_config_t;

typedef struct h2_esp_nfc_fm175xx_spi_config {
    h2_pal_periph_id_t id;
    spi_host_device_t host;
    int cs_gpio;
    int sclk_gpio;
    int mosi_gpio;
    int miso_gpio;
    int npd_gpio;
    uint32_t clock_speed_hz;
    size_t read_capacity;
} h2_esp_nfc_fm175xx_spi_config_t;

typedef enum h2_esp_nfc_fm175xx_transport_kind {
    H2_ESP_NFC_FM175XX_TRANSPORT_I2C = 0,
    H2_ESP_NFC_FM175XX_TRANSPORT_SPI = 1,
} h2_esp_nfc_fm175xx_transport_kind_t;

typedef struct h2_esp_nfc_fm175xx {
    h2_esp_nfc_fm175xx_config_t config;
    h2_esp_nfc_fm175xx_spi_config_t spi_config;
    h2_esp_nfc_fm175xx_transport_kind_t transport_kind;
    i2c_master_dev_handle_t dev;
    spi_device_handle_t spi_dev;
    int owns_spi_bus;
    h2_fm175xx_t reader;
    h2_pal_nfc_api_t api;
    int opened;
} h2_esp_nfc_fm175xx_t;

h2_pal_result_t h2_esp_nfc_fm175xx_init(
    h2_esp_nfc_fm175xx_t *adapter,
    const h2_esp_nfc_fm175xx_config_t *config);

h2_pal_result_t h2_esp_nfc_fm175xx_spi_init(
    h2_esp_nfc_fm175xx_t *adapter,
    const h2_esp_nfc_fm175xx_spi_config_t *config);

void h2_esp_nfc_fm175xx_deinit(h2_esp_nfc_fm175xx_t *adapter);

const h2_pal_nfc_api_t *h2_esp_nfc_fm175xx_api(h2_esp_nfc_fm175xx_t *adapter);

#ifdef __cplusplus
}
#endif

#endif
