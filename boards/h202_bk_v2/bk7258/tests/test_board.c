#include "h2_bk7258_board.h"
#include "h2_h202_board_config.h"
#include "h2_h202_board_test_sdk.h"
#include "usr_gpio_cfg.h"
#include <assert.h>
#include <string.h>
static int calls, fail_at;
static unsigned last_pin;
static int high[56];
static int output_enabled[56];
/* Match the SDK's lookup requirement using the production pin configuration. */
static const int gpio_defaults[][11] = GPIO_DEFAULT_DEV_CONFIG;
static int tick(unsigned pin) {
  last_pin = pin;
  return ++calls == fail_at ? -1 : 0;
}
int gpio_dev_unmap(gpio_id_t pin) {
  for (size_t i = 0; i < sizeof(gpio_defaults) / sizeof(gpio_defaults[0]);
       ++i) {
    if (gpio_defaults[i][0] == (int)pin) {
      assert(gpio_defaults[i][1] == GPIO_SECOND_FUNC_DISABLE);
      assert(gpio_defaults[i][2] == GPIO_DEV_INVALID);
      assert(gpio_defaults[i][3] == GPIO_IO_DISABLE);
      int rc = tick(pin);
      if (!rc) output_enabled[pin] = 0;
      return rc;
    }
  }
  return -1;
}
int bk_gpio_set_config(gpio_id_t pin, const gpio_config_t* cfg) {
  assert(cfg->io_mode == GPIO_OUTPUT_ENABLE);
  int rc = tick(pin);
  if (!rc) output_enabled[pin] = 1;
  return rc;
}
int bk_gpio_set_output_high(gpio_id_t pin) {
  if (!output_enabled[pin]) return -1;
  int rc = tick(pin);
  if (rc) return rc;
  high[pin] = 1;
  return 0;
}
int bk_gpio_set_output_low(gpio_id_t pin) {
  if (!output_enabled[pin]) return -1;
  int rc = tick(pin);
  if (rc) return rc;
  high[pin] = 0;
  return 0;
}
static int mac_failure = 1;
int bk_wifi_sta_get_mac(uint8_t mac[6]) {
  if (mac_failure) return H2_PAL_ERR_IO;
  memcpy(mac, (uint8_t[]){2, 1, 2, 0xab, 0xcd, 0xef}, 6);
  return 0;
}
int main(void) {
  for (int i = 1; i <= 18; ++i) {
    calls = 0;
    fail_at = i;
    assert(h2_bk7258_board_init() == H2_PAL_ERR_IO);
    assert(calls == i);
  }
  fail_at = 0;
  calls = 0;
  assert(h2_bk7258_board_init() == 0);
  assert(last_pin == 54 && high[54] == 1);
  assert(!high[7] && !high[13] && !high[53] && !high[37] && !high[4]);
  assert(h2_bk7258_board_init() == 0);
  assert(h2_bk7258_board_ble_local_name() == NULL);
  mac_failure = 0;
  assert(strcmp(h2_bk7258_board_ble_local_name(), "h202-ABCDEF") == 0);
  mac_failure = 1;
  assert(strcmp(h2_bk7258_board_ble_local_name(), "h202-ABCDEF") == 0);
  return 0;
}
