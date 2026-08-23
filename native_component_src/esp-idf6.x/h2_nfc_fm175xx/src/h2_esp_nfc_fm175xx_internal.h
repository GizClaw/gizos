#ifndef H2_ESP_NFC_FM175XX_INTERNAL_H
#define H2_ESP_NFC_FM175XX_INTERNAL_H

#include "h2_esp_nfc_fm175xx.h"
#include "h2_esp_nfc_fm175xx_spi_address.h"

int h2_esp_nfc_fm175xx_spi_open(
    h2_esp_nfc_fm175xx_t *adapter,
    h2_fm175xx_transport_t *out_transport);
void h2_esp_nfc_fm175xx_spi_close(h2_esp_nfc_fm175xx_t *adapter);
int h2_esp_nfc_fm175xx_i2c_open(
    h2_esp_nfc_fm175xx_t *adapter,
    h2_fm175xx_transport_t *out_transport);
void h2_esp_nfc_fm175xx_i2c_close(h2_esp_nfc_fm175xx_t *adapter);
extern const h2_pal_nfc_vtable_t h2_esp_nfc_fm175xx_nfc_vtable;

#endif
