#include "h2_esp_nfc_fm175xx_internal.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

#define H2_ESP_NFC_FM175XX_SPI_BURST_MAX 32u

static int spi_transmit(
    spi_device_handle_t dev,
    uint8_t address,
    const void *tx,
    size_t tx_len,
    void *rx,
    size_t rx_len) {
    spi_transaction_t transaction = {
        .cmd = address,
        .length = h2_esp_nfc_fm175xx_spi_transfer_bits(tx_len, rx_len),
        .rxlength = rx_len * 8u,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    return spi_device_polling_transmit(dev, &transaction) == ESP_OK
        ? H2_FM175XX_OK
        : H2_FM175XX_ERR_IO;
}

static int spi_write_regs(void *user, uint8_t reg, const uint8_t *data, size_t len) {
    if (data == NULL && len != 0u) {
        return H2_FM175XX_ERR_INVALID_ARG;
    }
    if (len == 0u) {
        return H2_FM175XX_OK;
    }
    return spi_transmit(
        (spi_device_handle_t)user,
        h2_esp_nfc_fm175xx_spi_write_address(reg),
        data,
        len,
        NULL,
        0u);
}

static int spi_write_reg(void *user, uint8_t reg, uint8_t value) {
    return spi_write_regs(user, reg, &value, 1u);
}

static int spi_read_regs(void *user, uint8_t reg, uint8_t *out_data, size_t len) {
    if (out_data == NULL && len != 0u) {
        return H2_FM175XX_ERR_INVALID_ARG;
    }
    if (len == 0u) {
        return H2_FM175XX_OK;
    }
    if (len > H2_ESP_NFC_FM175XX_SPI_BURST_MAX) {
        return H2_FM175XX_ERR_INVALID_ARG;
    }
    uint8_t clocks[H2_ESP_NFC_FM175XX_SPI_BURST_MAX];
    h2_esp_nfc_fm175xx_spi_fill_read_clocks(reg, clocks, len);
    return spi_transmit(
        (spi_device_handle_t)user,
        h2_esp_nfc_fm175xx_spi_read_address(reg),
        clocks,
        len,
        out_data,
        len);
}

static int spi_read_reg(void *user, uint8_t reg, uint8_t *out_value) {
    return spi_read_regs(user, reg, out_value, 1u);
}

static void spi_sleep_ms(void *user, uint32_t ms) {
    (void)user;
    vTaskDelay(pdMS_TO_TICKS(ms));
}

int h2_esp_nfc_fm175xx_spi_open(
    h2_esp_nfc_fm175xx_t *adapter,
    h2_fm175xx_transport_t *out_transport) {
    if (adapter == NULL || out_transport == NULL) {
        return H2_FM175XX_ERR_INVALID_ARG;
    }
    const h2_esp_nfc_fm175xx_spi_config_t *config = &adapter->spi_config;
    const gpio_config_t npd_config = {
        .pin_bit_mask = 1ULL << (uint32_t)config->npd_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&npd_config) != ESP_OK) {
        return H2_FM175XX_ERR_IO;
    }
    if (gpio_set_level((gpio_num_t)config->npd_gpio, 1) != ESP_OK) {
        (void)gpio_set_level((gpio_num_t)config->npd_gpio, 0);
        return H2_FM175XX_ERR_IO;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    const spi_bus_config_t bus_config = {
        .mosi_io_num = config->mosi_gpio,
        .miso_io_num = config->miso_gpio,
        .sclk_io_num = config->sclk_gpio,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 512,
    };
    esp_err_t err = spi_bus_initialize(config->host, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        (void)gpio_set_level((gpio_num_t)config->npd_gpio, 0);
        return H2_FM175XX_ERR_IO;
    }
    adapter->owns_spi_bus = 1;

    const spi_device_interface_config_t device_config = {
        .clock_speed_hz = (int)config->clock_speed_hz,
        .mode = 0,
        .command_bits = 8,
        .spics_io_num = config->cs_gpio,
        .queue_size = 1,
        .cs_ena_pretrans = 1,
        .cs_ena_posttrans = 1,
    };
    if (spi_bus_add_device(config->host, &device_config, &adapter->spi_dev) != ESP_OK) {
        h2_esp_nfc_fm175xx_spi_close(adapter);
        return H2_FM175XX_ERR_IO;
    }
    *out_transport = (h2_fm175xx_transport_t){
        .user = adapter->spi_dev,
        .write_reg = spi_write_reg,
        .write_regs = spi_write_regs,
        .read_reg = spi_read_reg,
        .read_regs = spi_read_regs,
        .sleep_ms = spi_sleep_ms,
    };
    return H2_FM175XX_OK;
}

void h2_esp_nfc_fm175xx_spi_close(h2_esp_nfc_fm175xx_t *adapter) {
    if (adapter == NULL) {
        return;
    }
    if (adapter->spi_dev != NULL) {
        (void)spi_bus_remove_device(adapter->spi_dev);
        adapter->spi_dev = NULL;
    }
    if (adapter->owns_spi_bus) {
        (void)spi_bus_free(adapter->spi_config.host);
        adapter->owns_spi_bus = 0;
    }
    if (adapter->spi_config.npd_gpio >= 0) {
        (void)gpio_set_level((gpio_num_t)adapter->spi_config.npd_gpio, 0);
    }
}

h2_pal_result_t h2_esp_nfc_fm175xx_spi_init(
    h2_esp_nfc_fm175xx_t *adapter,
    const h2_esp_nfc_fm175xx_spi_config_t *config) {
    if (adapter == NULL || config == NULL || config->id == 0u ||
        config->host <= SPI1_HOST || config->host >= SPI_HOST_MAX ||
        config->cs_gpio < 0 || config->sclk_gpio < 0 ||
        config->mosi_gpio < 0 || config->miso_gpio < 0 || config->npd_gpio < 0 ||
        config->clock_speed_hz == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(adapter, 0, sizeof(*adapter));
    adapter->spi_config = *config;
    adapter->config.id = config->id;
    adapter->config.read_capacity = config->read_capacity;
    adapter->transport_kind = H2_ESP_NFC_FM175XX_TRANSPORT_SPI;
    adapter->api.user = adapter;
    adapter->api.vtable = &h2_esp_nfc_fm175xx_nfc_vtable;
    return H2_PAL_OK;
}
