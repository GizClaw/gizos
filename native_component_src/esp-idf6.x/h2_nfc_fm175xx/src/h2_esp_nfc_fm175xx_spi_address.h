#ifndef H2_ESP_NFC_FM175XX_SPI_ADDRESS_H
#define H2_ESP_NFC_FM175XX_SPI_ADDRESS_H

#include <stddef.h>
#include <stdint.h>

uint8_t h2_esp_nfc_fm175xx_spi_write_address(uint8_t reg);
uint8_t h2_esp_nfc_fm175xx_spi_read_address(uint8_t reg);
size_t h2_esp_nfc_fm175xx_spi_transfer_bits(size_t tx_len, size_t rx_len);
void h2_esp_nfc_fm175xx_spi_fill_read_clocks(
    uint8_t reg,
    uint8_t *out_clocks,
    size_t len);

#endif
