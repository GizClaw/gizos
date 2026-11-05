#ifndef H2_ZERO_BK_PERIPHERAL_POWER_TEST_SDK_H
#define H2_ZERO_BK_PERIPHERAL_POWER_TEST_SDK_H

#include "h2_zero_bk_board_config.h"
#include <stdint.h>

typedef int gpio_id_t;
typedef struct {
  int io_mode;
  int pull_mode;
  int func_mode;
} gpio_config_t;

/* Register-shaped SDK test double; the native implementation uses gpio_hw_t
 * from the pinned SDK through gpio_driver.h. */
typedef union {
  struct {
    uint32_t gpio_input : 1;
    uint32_t gpio_output : 1;
    uint32_t gpio_input_en : 1;
    uint32_t gpio_output_en : 1;
    uint32_t gpio_pull_mode : 1;
    uint32_t gpio_pull_mode_en : 1;
    uint32_t gpio_2_func_en : 1;
    uint32_t preserved : 25;
  };
  uint32_t v;
} h2_zero_bk_gpio_config_register_t;

enum {
  BK_OK = 0,
  GPIO_OUTPUT_ENABLE = 1,
  GPIO_PULL_DISABLE = 0,
  GPIO_SECOND_FUNC_DISABLE = 0,
};

uint32_t rtos_enter_critical(void);
void rtos_exit_critical(uint32_t previous);
int gpio_dev_unmap(gpio_id_t gpio);
int bk_gpio_set_config(gpio_id_t gpio, const gpio_config_t* config);
int bk_gpio_set_output_high(gpio_id_t gpio);
int bk_gpio_set_output_low(gpio_id_t gpio);
uint32_t bk_gpio_get_value(gpio_id_t gpio);
int bk_gpio_set_value(gpio_id_t gpio, uint32_t value);

#endif
