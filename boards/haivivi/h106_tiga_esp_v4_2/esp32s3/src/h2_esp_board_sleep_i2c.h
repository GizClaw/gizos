#ifndef H2_ESP_BOARD_SLEEP_I2C_H
#define H2_ESP_BOARD_SLEEP_I2C_H

#include "h2_esp_board_config.h"
#include "driver/i2c_master.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Board NFC terminal policy. Consumers must already be stopped.
 * Keep the borrowed I2C bus alive until writes finish.
 * Never call from the supervisor commit: device add/remove can wait on an
 * SDK mutex owned by the stalled worker, despite transmit's 20 ms timeout. */
static inline esp_err_t h2_tiga_sleep_i2c_write_sequence(
    i2c_master_bus_handle_t bus, uint16_t address, const uint8_t (*sequence)[2],
    size_t count) {
  const i2c_device_config_t config = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = address,
      .scl_speed_hz = H2_TIGA_ESP_PERIPHERAL_I2C_SPEED_HZ,
  };
  i2c_master_dev_handle_t device = NULL;
  esp_err_t first = i2c_master_bus_add_device(bus, &config, &device);
  if (first != ESP_OK) {
    printf("H2_BOARD_DEEP_SLEEP stage=i2c_add addr=0x%02x rc=%d\n",
           (unsigned int)address, (int)first);
    return first;
  }
  for (size_t i = 0u; i < count; ++i) {
    const esp_err_t rc = i2c_master_transmit(device, sequence[i], 2u, 20);
    if (rc != ESP_OK) {
      printf(
          "H2_BOARD_DEEP_SLEEP stage=i2c_write addr=0x%02x reg=0x%02x rc=%d\n",
          (unsigned int)address, (unsigned int)sequence[i][0], (int)rc);
      if (first == ESP_OK) first = rc;
    }
  }
  const esp_err_t rc = i2c_master_bus_rm_device(device);
  if (rc != ESP_OK) {
    printf("H2_BOARD_DEEP_SLEEP stage=i2c_remove addr=0x%02x rc=%d\n",
           (unsigned int)address, (int)rc);
    if (first == ESP_OK) first = rc;
  }
  return first;
}

static inline esp_err_t h2_tiga_sleep_nfc_prepare(i2c_master_bus_handle_t bus) {
  /* FM175xx: cancel command / receiver off, disable TX1/TX2 RF using the
   * board driver's TxControl base 0x80, then Idle + RcvOff + PowerDown.
   * No NPD/reset/power GPIO is wired. Also run when NFC scanning is disabled.
   * Fudan FM17550 manual sections 3.2.1.2, 3.2.2.5 and 11.2/11.3. */
  static const uint8_t nfc_off[][2] = {
      {0x01, 0x20},
      {0x14, 0x80},
      {0x01, 0x30},
  };
  if (bus == NULL) {
    printf("H2_BOARD_DEEP_SLEEP stage=i2c_power_down skip=no_bus\n");
    return ESP_ERR_INVALID_STATE;
  }
  const esp_err_t nfc_rc =
      h2_tiga_sleep_i2c_write_sequence(bus, H2_TIGA_ESP_NFC_I2C_ADDR, nfc_off,
                                       sizeof(nfc_off) / sizeof(nfc_off[0]));
  printf("H2_BOARD_DEEP_SLEEP stage=nfc_power_down rc=%d\n", (int)nfc_rc);
  return nfc_rc;
}

/* Consumers are stopped. Codec suspend uses the public Audio System owner;
 * no private codec register sequence and no supervisor-context I2C waits. */
static inline void h2_tiga_sleep_codec_prepare(int (*power_down)(void)) {
  const int rc = power_down();
  printf("H2_BOARD_DEEP_SLEEP stage=codec_power_down rc=%d\n", rc);
  /* Best effort: the terminal GPIO/sleep sequence must still run on error. */
}

#endif
