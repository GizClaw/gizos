#include "h2_bk7258_board.h"
#include "h2_h202_board_config.h"
#ifdef H2_H202_HOST_TEST
#include "h2_h202_board_test_sdk.h"
#else
#include "driver/gpio.h"
#include "gpio_driver.h"
#include "modules/wifi.h"
#endif
#include <stdio.h>
#include <string.h>

int h2_bk7258_board_init(void) {
  static const struct {
    unsigned gpio;
    int high;
  } outputs[] = {
      {H2_H202_PA_ENABLE_GPIO, 0},
      {H2_H202_MODEM_ENABLE_GPIO, 0},
      {H2_H202_MODEM_VBUS_ENABLE_GPIO, 0},
      {H2_H202_WHITE_LED_GPIO, 0},
      {H2_H202_RGB_GPIO, 0},
      {H2_H202_PERIPHERAL_ENABLE_GPIO, 1},
  };
  const gpio_config_t config = {
      .io_mode = GPIO_OUTPUT_ENABLE,
      .pull_mode = GPIO_PULL_DISABLE,
      .func_mode = GPIO_SECOND_FUNC_DISABLE,
  };
  for (size_t i = 0; i < sizeof(outputs) / sizeof(outputs[0]); ++i) {
    gpio_id_t pin = (gpio_id_t)outputs[i].gpio;
    /* The SDK rejects level writes while output is disabled. */
    if (gpio_dev_unmap(pin) != 0 || bk_gpio_set_config(pin, &config) != 0 ||
        (outputs[i].high ? bk_gpio_set_output_high(pin)
                         : bk_gpio_set_output_low(pin)) != 0)
      return H2_PAL_ERR_IO;
  }
  return H2_PAL_OK;
}

const char* h2_bk7258_board_ble_local_name(void) {
  static char name[12];
  uint8_t mac[6];
  static const uint8_t zero[6] = {0};
  if (name[0]) return name;
  if (bk_wifi_sta_get_mac(mac) != 0 || (mac[0] & 1u) ||
      memcmp(mac, zero, sizeof(mac)) == 0)
    return NULL;
  if (snprintf(name, sizeof(name), "h202-%02X%02X%02X", (unsigned)mac[3],
               (unsigned)mac[4], (unsigned)mac[5]) != 11) {
    name[0] = '\0';
    return NULL;
  }
  return name;
}
