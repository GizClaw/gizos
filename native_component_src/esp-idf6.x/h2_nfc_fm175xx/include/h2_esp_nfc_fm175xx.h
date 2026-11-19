#ifndef H2_ESP_NFC_FM175XX_H
#define H2_ESP_NFC_FM175XX_H

#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "h2_fm175xx.h"
#include "hal/h2_pal_input.h"
#include "os/h2_pal_time.h"
#include "os/h2_pal_log.h"

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
  /** Optional borrowed monotonic clock for prepared NDEF writes. */
  const h2_pal_time_api_t* time;
  /** Optional borrowed log sink; NULL disables diagnostics. Keep the API
   * and its user context valid for the initialized adapter lifetime. */
  const h2_pal_log_api_t* log;
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
  const h2_pal_time_api_t* time;
  /** Optional borrowed log sink; NULL disables diagnostics. Keep the API
   * and its user context valid for the initialized adapter lifetime. */
  const h2_pal_log_api_t* log;
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
  unsigned scan_count;
  int last_scan_error;
  h2_fm175xx_type_a_card_t read_selection;
  int read_selection_valid;
  uintptr_t prepared_ndef_id;
  uint64_t ndef_update_started_ms;
  h2_fm175xx_ndef_update_t ndef_update;
} h2_esp_nfc_fm175xx_t;

h2_pal_result_t h2_esp_nfc_fm175xx_init(
    h2_esp_nfc_fm175xx_t* adapter, const h2_esp_nfc_fm175xx_config_t* config);

h2_pal_result_t h2_esp_nfc_fm175xx_spi_init(
    h2_esp_nfc_fm175xx_t* adapter,
    const h2_esp_nfc_fm175xx_spi_config_t* config);

void h2_esp_nfc_fm175xx_deinit(h2_esp_nfc_fm175xx_t* adapter);

/** Borrow the adapter's externally serialized NFC capability. The first write
 * after a successful read may reuse that exact selected card. A scan, another
 * read, a write attempt, reopening or deinitializing ends that reuse window.
 * Other writes select and check the requested UID before device writes.
 * Prepared NDEF updates reserve the selection for 500 ms after acquisition;
 * scan and unrelated I/O return BUSY until finish or expiration. */
const h2_pal_nfc_api_t* h2_esp_nfc_fm175xx_api(h2_esp_nfc_fm175xx_t* adapter);

#ifdef __cplusplus
}
#endif

#endif
