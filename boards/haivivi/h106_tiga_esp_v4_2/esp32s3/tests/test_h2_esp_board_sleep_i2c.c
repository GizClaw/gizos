#include "h2_esp_board_sleep_i2c.h"

#include <assert.h>

static unsigned adds, writes, removes;
static int fail_write;
static esp_err_t add_rc, remove_rc;
static int bus_storage, device_storage;

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
                                    const i2c_device_config_t* config,
                                    i2c_master_dev_handle_t* device) {
  assert(bus == (i2c_master_bus_handle_t)&bus_storage);
  /* Only NFC may be addressed here, never an audio codec. */
  assert(config->device_address == 0x28u);
  assert(config->dev_addr_length == I2C_ADDR_BIT_LEN_7);
  assert(config->scl_speed_hz == H2_TIGA_ESP_PERIPHERAL_I2C_SPEED_HZ);
  ++adds;
  *device = (i2c_master_dev_handle_t)&device_storage;
  return add_rc;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device,
                              const uint8_t* data, size_t size,
                              int timeout_ms) {
  static const uint8_t expected[][2] = {
      {0x01, 0x20},
      {0x14, 0x80},
      {0x01, 0x30},
  };
  assert(device == (i2c_master_dev_handle_t)&device_storage);
  assert(size == 2u && timeout_ms == 20);
  assert(writes < 3u);
  assert(data[0] == expected[writes][0]);
  assert(data[1] == expected[writes][1]);
  ++writes;
  return (int)writes == fail_write ? ESP_FAIL : ESP_OK;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device) {
  assert(device == (i2c_master_dev_handle_t)&device_storage);
  ++removes;
  return remove_rc;
}

static int codec_calls, codec_rc;
static int codec_power_down(void) {
  ++codec_calls;
  return codec_rc;
}

int main(void) {
  h2_tiga_sleep_codec_prepare(codec_power_down);
  assert(codec_calls == 1);
  codec_rc = -1;
  h2_tiga_sleep_codec_prepare(codec_power_down);
  assert(codec_calls == 2); /* Failure returned to the terminal sequence. */
  assert(h2_tiga_sleep_nfc_prepare(NULL) == ESP_ERR_INVALID_STATE);
  assert(adds == 0u);
  assert(h2_tiga_sleep_nfc_prepare((i2c_master_bus_handle_t)&bus_storage) ==
         ESP_OK);
  assert(adds == 1u && writes == 3u && removes == 1u);
  for (fail_write = 1; fail_write <= 3; ++fail_write) {
    writes = 0u;
    assert(h2_tiga_sleep_nfc_prepare((i2c_master_bus_handle_t)&bus_storage) ==
           ESP_FAIL);
    assert(writes == 3u); /* Best effort reaches PowerDown after errors. */
  }
  add_rc = ESP_FAIL;
  writes = 0u;
  unsigned previous_removes = removes;
  assert(h2_tiga_sleep_nfc_prepare((i2c_master_bus_handle_t)&bus_storage) ==
         ESP_FAIL);
  assert(writes == 0u && removes == previous_removes);
  add_rc = ESP_OK;
  remove_rc = ESP_FAIL;
  assert(h2_tiga_sleep_nfc_prepare((i2c_master_bus_handle_t)&bus_storage) ==
         ESP_FAIL);
  assert(writes == 3u && removes == previous_removes + 1u);
  return 0;
}
