#include "h2_esp_nfc_fm175xx_spi_address.h"

uint8_t h2_esp_nfc_fm175xx_spi_write_address(uint8_t reg) {
    return (uint8_t)((reg << 1u) & 0x7eu);
}

uint8_t h2_esp_nfc_fm175xx_spi_read_address(uint8_t reg) {
    return (uint8_t)((reg << 1u) | 0x80u);
}

size_t h2_esp_nfc_fm175xx_spi_transfer_bits(size_t tx_len, size_t rx_len) {
    return (tx_len > rx_len ? tx_len : rx_len) * 8u;
}

void h2_esp_nfc_fm175xx_spi_fill_read_clocks(
    uint8_t reg,
    uint8_t *out_clocks,
    size_t len) {
    if (out_clocks == NULL) {
        return;
    }
    const uint8_t address = h2_esp_nfc_fm175xx_spi_read_address(reg);
    for (size_t i = 0u; i < len; ++i) {
        out_clocks[i] = i + 1u < len ? address : 0u;
    }
}
