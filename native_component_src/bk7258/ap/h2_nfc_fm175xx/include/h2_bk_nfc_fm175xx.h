#ifndef H2_BK_NFC_FM175XX_H
#define H2_BK_NFC_FM175XX_H

#include "h2_fm175xx.h"
#include "hal/h2_pal_input.h"

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
  uint32_t register_trace[64];
  unsigned register_trace_count;
  unsigned scan_count;
  int last_scan_error;
  unsigned fifo_verified_cases;
  int io_disabled;
  int io_poisoned;
  uint8_t io_rx_buffer[32];
  uint8_t io_tx_buffer[33];
  uint8_t rf_config;
  uint8_t rx_threshold;
  int rf_tuning;
  unsigned activation_attempts;
  uint8_t rf_error_bits;
  int preserve_tx_drive;
} h2_bk_nfc_fm175xx_t;

/* Adapter storage outlives its API and native transfers. If io_poisoned remains
 * set after a failed controller abort, keep the storage alive and do not
 * reinitialize it until the hardware controller has been reset. */

h2_pal_result_t h2_bk_nfc_fm175xx_init(
    h2_bk_nfc_fm175xx_t* adapter, const h2_bk_nfc_fm175xx_config_t* config);

/* Same ownership as init(), with board-selected receiver settings. A scan
 * retries RF protocol failures up to three complete selections; only a fully
 * validated selection succeeds. Transport failures are not retried, and an
 * invalid frame followed only by silence remains an error. */
h2_pal_result_t h2_bk_nfc_fm175xx_init_with_rf(
    h2_bk_nfc_fm175xx_t* adapter, const h2_bk_nfc_fm175xx_config_t* config,
    uint8_t rf_config, uint8_t rx_threshold);

/* Copies profile values; neither config nor profile is retained by pointer. */
h2_pal_result_t h2_bk_nfc_fm175xx_init_with_profile(
    h2_bk_nfc_fm175xx_t* adapter, const h2_bk_nfc_fm175xx_config_t* config,
    const h2_fm175xx_type_a_profile_t* profile);

/* Copies the complete RF profile. Existing initializers keep their defaults. */
h2_pal_result_t h2_bk_nfc_fm175xx_init_with_profile(
    h2_bk_nfc_fm175xx_t* adapter, const h2_bk_nfc_fm175xx_config_t* config,
    const h2_fm175xx_type_a_profile_t* profile);

const h2_pal_nfc_api_t* h2_bk_nfc_fm175xx_api(h2_bk_nfc_fm175xx_t* adapter);

#ifdef __cplusplus
}
#endif

#endif
