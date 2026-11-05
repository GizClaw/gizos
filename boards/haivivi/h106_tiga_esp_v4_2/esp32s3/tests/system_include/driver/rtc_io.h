#ifndef H2_TEST_RTC_IO_H
#define H2_TEST_RTC_IO_H
#include "driver/gpio.h"
typedef enum {
  RTC_GPIO_MODE_INPUT_ONLY,
  RTC_GPIO_MODE_OUTPUT_ONLY,
  RTC_GPIO_MODE_INPUT_OUTPUT
} rtc_gpio_mode_t;
esp_err_t rtc_gpio_deinit(gpio_num_t pin);
esp_err_t rtc_gpio_set_level(gpio_num_t pin, uint32_t level);
esp_err_t rtc_gpio_pullup_dis(gpio_num_t pin);
esp_err_t rtc_gpio_pulldown_dis(gpio_num_t pin);
esp_err_t rtc_gpio_hold_en(gpio_num_t pin);
esp_err_t rtc_gpio_isolate(gpio_num_t pin);
esp_err_t rtc_gpio_hold_dis(gpio_num_t pin);
esp_err_t rtc_gpio_init(gpio_num_t pin);
esp_err_t rtc_gpio_set_direction(gpio_num_t pin, rtc_gpio_mode_t mode);
#endif
