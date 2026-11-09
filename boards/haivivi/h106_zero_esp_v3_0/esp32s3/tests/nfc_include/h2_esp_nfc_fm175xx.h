#ifndef H2_TEST_NFC_FM175XX_H
#define H2_TEST_NFC_FM175XX_H
#include "driver/i2c_master.h"
#include "hal/h2_pal_nfc.h"
typedef struct {
  int unused;
} h2_esp_nfc_fm175xx_t;
typedef struct {
  h2_pal_periph_id_t id;
  i2c_master_bus_handle_t bus;
  uint16_t i2c_address;
  uint32_t i2c_speed_hz;
  size_t read_capacity;
} h2_esp_nfc_fm175xx_config_t;
h2_pal_result_t h2_esp_nfc_fm175xx_init(
    h2_esp_nfc_fm175xx_t* adapter, const h2_esp_nfc_fm175xx_config_t* config);
void h2_esp_nfc_fm175xx_deinit(h2_esp_nfc_fm175xx_t* adapter);
const h2_pal_nfc_api_t* h2_esp_nfc_fm175xx_api(h2_esp_nfc_fm175xx_t* adapter);
#endif
