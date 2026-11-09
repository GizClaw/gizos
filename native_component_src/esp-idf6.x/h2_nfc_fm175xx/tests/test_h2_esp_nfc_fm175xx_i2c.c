#include "h2_esp_nfc_fm175xx_internal.h"
#include "freertos/task.h"

#include <assert.h>
#include <string.h>

struct h2_nfc_test_i2c_bus {
  int unused;
};
struct h2_nfc_test_i2c_device {
  int unused;
};
static struct h2_nfc_test_i2c_bus bus;
static struct h2_nfc_test_i2c_device device;
static esp_err_t result;
static int calls;
static int timeout_ms;
static uint8_t sent[33];
static size_t sent_size;
static size_t received_size;
static TickType_t delay_ticks;

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t actual_bus,
                                    const i2c_device_config_t* config,
                                    i2c_master_dev_handle_t* out_device) {
  assert(actual_bus == &bus);
  assert(config->dev_addr_length == I2C_ADDR_BIT_LEN_7);
  assert(config->device_address == 0x28u);
  assert(config->scl_speed_hz == 400000u);
  if (result == ESP_OK) *out_device = &device;
  return result;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t actual_device) {
  assert(actual_device == &device);
  ++calls;
  return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t actual_device,
                              const uint8_t* data, size_t size, int timeout) {
  assert(actual_device == &device);
  assert(size <= sizeof(sent));
  ++calls;
  timeout_ms = timeout;
  sent_size = size;
  memcpy(sent, data, size);
  return result;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t actual_device,
                                      const uint8_t* data, size_t size,
                                      uint8_t* out, size_t out_size,
                                      int timeout) {
  assert(actual_device == &device);
  assert(size == 1u);
  ++calls;
  timeout_ms = timeout;
  sent[0] = data[0];
  sent_size = size;
  received_size = out_size;
  if (out_size > 0u && result == ESP_OK) memset(out, 0xa5, out_size);
  return result;
}

void vTaskDelay(TickType_t ticks) { delay_ticks = ticks; }

int main(void) {
  h2_esp_nfc_fm175xx_t adapter = {
      .config = {.bus = &bus, .i2c_address = 0x28u, .i2c_speed_hz = 400000u},
  };
  h2_fm175xx_transport_t transport = {0};
  uint8_t data[33] = {1, 2, 3};
  uint8_t out[3] = {0};

  result = ESP_FAIL;
  assert(h2_esp_nfc_fm175xx_i2c_open(&adapter, &transport) ==
         H2_FM175XX_ERR_IO);
  result = ESP_OK;
  assert(h2_esp_nfc_fm175xx_i2c_open(&adapter, &transport) == H2_FM175XX_OK);
  assert(transport.user == &device);

  assert(transport.write_reg(transport.user, 0x11, 0x22) == H2_FM175XX_OK);
  assert(timeout_ms == 20 && sent_size == 2u);
  assert(sent[0] == 0x11 && sent[1] == 0x22);
  assert(transport.write_regs(transport.user, 0x33, data, 3) == H2_FM175XX_OK);
  assert(timeout_ms == 20 && sent_size == 4u && sent[0] == 0x33);
  assert(memcmp(sent + 1, data, 3) == 0);
  assert(transport.read_reg(transport.user, 0x44, out) == H2_FM175XX_OK);
  assert(timeout_ms == 20 && sent[0] == 0x44);
  assert(received_size == 1u && out[0] == 0xa5);
  assert(transport.read_regs(transport.user, 0x55, out, sizeof(out)) ==
         H2_FM175XX_OK);
  assert(timeout_ms == 20 && sent[0] == 0x55);
  assert(received_size == sizeof(out) && out[2] == 0xa5);

  /* Preserve the transport's SDK-failure-to-IO mapping, including timeouts. */
  const esp_err_t failures[] = {ESP_ERR_TIMEOUT, ESP_FAIL};
  for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
    result = failures[i];
    assert(transport.write_reg(transport.user, 0, 0) == H2_FM175XX_ERR_IO);
    assert(timeout_ms == 20);
    assert(transport.write_regs(transport.user, 0, data, 3) ==
           H2_FM175XX_ERR_IO);
    assert(timeout_ms == 20);
    assert(transport.read_reg(transport.user, 0, out) == H2_FM175XX_ERR_IO);
    assert(timeout_ms == 20);
    assert(transport.read_regs(transport.user, 0, out, 3) == H2_FM175XX_ERR_IO);
    assert(timeout_ms == 20);
  }
  int previous_calls = calls;
  assert(transport.write_regs(transport.user, 0, NULL, 1) ==
         H2_FM175XX_ERR_INVALID_ARG);
  assert(transport.write_regs(transport.user, 0, data, 33) ==
         H2_FM175XX_ERR_INVALID_ARG);
  assert(transport.read_reg(transport.user, 0, NULL) ==
         H2_FM175XX_ERR_INVALID_ARG);
  assert(transport.read_regs(transport.user, 0, NULL, 1) ==
         H2_FM175XX_ERR_INVALID_ARG);
  assert(calls == previous_calls);

  transport.sleep_ms(transport.user, 0);
  assert(delay_ticks == 0);
  transport.sleep_ms(transport.user, 1);
  assert(delay_ticks == 1);
  transport.sleep_ms(transport.user, 2);
  assert(delay_ticks == 1);
  transport.sleep_ms(transport.user, 10);
  assert(delay_ticks == 1);
  transport.sleep_ms(transport.user, 11);
  assert(delay_ticks == 2);
  transport.sleep_ms(transport.user, 21);
  assert(delay_ticks == 3);
  transport.sleep_ms(transport.user, UINT32_MAX);
  assert(delay_ticks == 429496730u);
  h2_esp_nfc_fm175xx_i2c_close(&adapter);
  assert(adapter.dev == NULL && calls == previous_calls + 1);
  return 0;
}
