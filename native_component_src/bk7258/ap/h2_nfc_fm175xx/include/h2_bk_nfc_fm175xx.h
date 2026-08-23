#ifndef H2_BK_NFC_FM175XX_H
#define H2_BK_NFC_FM175XX_H

#include "h2_fm175xx.h"
#include "h2/pal/hal/h2_pal_input.h"

#include <driver/i2c_types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_BK_NFC_FM175XX_DEFAULT_READ_CAPACITY 256u

typedef struct h2_bk_nfc_fm175xx_config {
    h2_pal_periph_id_t id;
    i2c_id_t i2c_id;
    uint16_t i2c_address;
    uint32_t i2c_speed_hz;
    size_t read_capacity;
} h2_bk_nfc_fm175xx_config_t;

typedef struct h2_bk_nfc_fm175xx {
    h2_bk_nfc_fm175xx_config_t config;
    h2_fm175xx_t reader;
    h2_pal_nfc_api_t api;
    int opened;
} h2_bk_nfc_fm175xx_t;

h2_pal_result_t h2_bk_nfc_fm175xx_init(
    h2_bk_nfc_fm175xx_t *adapter,
    const h2_bk_nfc_fm175xx_config_t *config);

const h2_pal_nfc_api_t *h2_bk_nfc_fm175xx_api(
    h2_bk_nfc_fm175xx_t *adapter);

#ifdef __cplusplus
}
#endif

#endif
