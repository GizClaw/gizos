#ifndef H2_P4_TEST_GPIO_H
#define H2_P4_TEST_GPIO_H
#include "esp_err.h"
#include <stdint.h>
enum {
  GPIO_NUM_0 = 0,
  GPIO_NUM_35 = 35,
  GPIO_MODE_INPUT = 0,
  GPIO_MODE_OUTPUT = 1,
  GPIO_PULLUP_ENABLE = 1,
  GPIO_PULLDOWN_DISABLE = 0,
  GPIO_INTR_DISABLE = 0,
};
typedef struct {
  uint64_t pin_bit_mask;
  int mode, pull_up_en, pull_down_en, intr_type;
} gpio_config_t;
esp_err_t gpio_config(const gpio_config_t* config);
int gpio_get_level(int gpio);
esp_err_t gpio_set_direction(int gpio, int mode);
esp_err_t gpio_set_level(int gpio, unsigned level);
#endif
