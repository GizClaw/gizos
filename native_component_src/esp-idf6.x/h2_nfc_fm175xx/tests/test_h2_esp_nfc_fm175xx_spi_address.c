#include "h2_esp_nfc_fm175xx_spi_address.h"

#include <assert.h>
#include <stdint.h>

int main(void) {
    uint8_t read_clocks[4] = {0xffu, 0xffu, 0xffu, 0xffu};

    assert(h2_esp_nfc_fm175xx_spi_write_address(0x00u) == 0x00u);
    assert(h2_esp_nfc_fm175xx_spi_read_address(0x00u) == 0x80u);
    assert(h2_esp_nfc_fm175xx_spi_write_address(0x09u) == 0x12u);
    assert(h2_esp_nfc_fm175xx_spi_read_address(0x09u) == 0x92u);
    assert(h2_esp_nfc_fm175xx_spi_write_address(0x3fu) == 0x7eu);
    assert(h2_esp_nfc_fm175xx_spi_read_address(0x3fu) == 0xfeu);
    assert(h2_esp_nfc_fm175xx_spi_write_address(0xffu) == 0x7eu);
    assert(h2_esp_nfc_fm175xx_spi_read_address(0xffu) == 0xfeu);
    assert(h2_esp_nfc_fm175xx_spi_transfer_bits(4u, 0u) == 32u);
    assert(h2_esp_nfc_fm175xx_spi_transfer_bits(0u, 4u) == 32u);
    assert(h2_esp_nfc_fm175xx_spi_transfer_bits(2u, 4u) == 32u);
    h2_esp_nfc_fm175xx_spi_fill_read_clocks(0x09u, read_clocks, 1u);
    assert(read_clocks[0] == 0x00u);
    h2_esp_nfc_fm175xx_spi_fill_read_clocks(0x09u, read_clocks, 4u);
    assert(read_clocks[0] == 0x92u);
    assert(read_clocks[1] == 0x92u);
    assert(read_clocks[2] == 0x92u);
    assert(read_clocks[3] == 0x00u);
    return 0;
}
