#ifndef H2_TEST_ESP_PRIVATE_GPIO_H
#define H2_TEST_ESP_PRIVATE_GPIO_H
#include "driver/gpio.h"
#define PIN_FUNC_GPIO 1
esp_err_t gpio_func_sel(gpio_num_t pin, uint32_t function);
#endif
