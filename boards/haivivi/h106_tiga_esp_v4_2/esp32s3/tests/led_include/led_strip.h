#ifndef H2_TEST_LED_STRIP_H
#define H2_TEST_LED_STRIP_H

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define LED_MODEL_WS2812 0
#define LED_STRIP_COLOR_COMPONENT_FMT_GRB 1u

typedef struct h2_test_led_strip* led_strip_handle_t;

typedef struct {
  int strip_gpio_num;
  uint32_t max_leds;
  int led_model;
  uint32_t color_component_format;
} led_strip_config_t;

typedef struct {
  uint32_t resolution_hz;
  struct {
    bool with_dma;
  } flags;
} led_strip_rmt_config_t;

esp_err_t led_strip_new_rmt_device(const led_strip_config_t* strip,
                                   const led_strip_rmt_config_t* rmt,
                                   led_strip_handle_t* out);
esp_err_t led_strip_set_pixel(led_strip_handle_t strip, uint32_t index,
                              uint32_t red, uint32_t green, uint32_t blue);
esp_err_t led_strip_refresh(led_strip_handle_t strip);
esp_err_t led_strip_clear(led_strip_handle_t strip);

#endif
