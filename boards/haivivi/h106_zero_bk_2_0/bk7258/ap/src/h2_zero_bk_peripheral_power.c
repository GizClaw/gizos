#include "h2_zero_bk_peripheral_power.h"

#ifdef H2_ZERO_BK_PERIPHERAL_POWER_HOST_TEST
#include "h2_zero_bk_peripheral_power_test_sdk.h"
#else
#include "h2_zero_bk_board_config.h"
#include <driver/gpio.h>
#include <os/os.h>
#include "gpio_driver.h"
typedef __typeof__(((gpio_hw_t*)0)->gpio_num[0].cfg)
    h2_zero_bk_gpio_config_register_t;
#endif

#include <stddef.h>
#include <stdint.h>

h2_pal_result_t h2_zero_bk_peripheral_power_enable(void) {
  const gpio_id_t gpio = (gpio_id_t)H2_ZERO_BK_PERIPHERAL_ENABLE_GPIO;
  /* The BSP exclusively owns this GPIO. gpio_dev_unmap() disables output
   * even when it is already a GPIO, momentarily releasing the Flash supply.
   * The SDK's ordinary output setter rejects a disabled pin. Use its raw
   * configuration API to publish the high latch and output-enable together,
   * preserving unrelated fields. The SDK bitfield defines the register layout.
   */
  const uint32_t flags = rtos_enter_critical();
  h2_zero_bk_gpio_config_register_t config;
  config.v = bk_gpio_get_value(gpio);
  config.gpio_output = 1u;
  config.gpio_input_en = 0u;
  config.gpio_output_en = 0u;
  config.gpio_pull_mode = 0u;
  config.gpio_pull_mode_en = 0u;
  config.gpio_2_func_en = 0u;
  const int rc = bk_gpio_set_value(gpio, config.v);
  rtos_exit_critical(flags);
  return rc == BK_OK ? H2_PAL_OK : H2_PAL_ERR_IO;
}

h2_pal_result_t h2_bk7258_board_audio_power_set(int enabled) {
  h2_pal_result_t rc = h2_zero_bk_peripheral_power_enable();
  if (rc != H2_PAL_OK) return rc;
  const gpio_id_t pins[] = {
      (gpio_id_t)H2_ZERO_BK_AUDIO_SELECT_GPIO,
      (gpio_id_t)H2_ZERO_BK_AUDIO_PA_ENABLE_GPIO,
  };
  const gpio_config_t output = {
      .io_mode = GPIO_OUTPUT_ENABLE,
      .pull_mode = GPIO_PULL_DISABLE,
      .func_mode = GPIO_SECOND_FUNC_DISABLE,
  };
  for (size_t i = 0u; i < sizeof(pins) / sizeof(pins[0]); ++i) {
    if (gpio_dev_unmap(pins[i]) != BK_OK ||
        bk_gpio_set_config(pins[i], &output) != BK_OK)
      return H2_PAL_ERR_IO;
  }
  if (bk_gpio_set_output_low(pins[0]) != BK_OK) return H2_PAL_ERR_IO;
  const int power_rc = enabled ? bk_gpio_set_output_high(pins[1])
                               : bk_gpio_set_output_low(pins[1]);
  return power_rc == BK_OK ? H2_PAL_OK : H2_PAL_ERR_IO;
}
