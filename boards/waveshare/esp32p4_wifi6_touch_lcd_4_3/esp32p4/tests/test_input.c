#include "../src/h2_esp_board_private.h"
#include "driver/gpio.h"
#include "driver/temperature_sensor.h"
#include <assert.h>
#include <string.h>

static int level = 1, temperature;
static unsigned configurations;
static esp_err_t gpio_error;

esp_err_t gpio_config(const gpio_config_t* config) {
  /* Schematic Key2 is GPIO35; GPIO0 is unrelated to the BOOT key. */
  assert(config->pin_bit_mask == (1ULL << 35) &&
         config->mode == GPIO_MODE_INPUT &&
         config->pull_up_en == GPIO_PULLUP_ENABLE &&
         config->pull_down_en == GPIO_PULLDOWN_DISABLE &&
         config->intr_type == GPIO_INTR_DISABLE);
  ++configurations;
  return gpio_error;
}
int gpio_get_level(int gpio) {
  assert(gpio == 35);
  return level;
}
esp_err_t temperature_sensor_install(const temperature_sensor_config_t* config,
                                     temperature_sensor_handle_t* out) {
  assert(config->low == 10 && config->high == 80);
  *out = &temperature;
  return ESP_OK;
}
esp_err_t temperature_sensor_enable(temperature_sensor_handle_t handle) {
  assert(handle == &temperature);
  return ESP_OK;
}
esp_err_t temperature_sensor_uninstall(temperature_sensor_handle_t handle) {
  assert(handle == &temperature);
  return ESP_OK;
}
esp_err_t temperature_sensor_get_celsius(temperature_sensor_handle_t handle,
                                         float* out) {
  assert(handle == &temperature);
  *out = 37.25f;
  return ESP_OK;
}
int main(void) {
  const h2_pal_button_api_t* api = h2_esp_board_button_api();
  h2_pal_single_button_reading_t reading;
  assert(h2_pal_button_read_single_button(api, 102, &reading) ==
         H2_PAL_ERR_NOT_FOUND); /* RESET is EN, not a fabricated GPIO input. */
  assert(h2_pal_button_read_single_button(api, 501, &reading) ==
         H2_PAL_ERR_INVALID_ARG);
  gpio_error = ESP_FAIL;
  assert(h2_pal_button_read_single_button(api, 101, &reading) == H2_PAL_ERR_IO);
  gpio_error = ESP_OK;
  assert(h2_pal_button_read_single_button(api, 101, &reading) == 0);
  assert(reading.id == 101 && reading.state == H2_PAL_BUTTON_STATE_RELEASED);
  level = 0;
  assert(h2_pal_button_read_single_button(api, 101, &reading) == 0);
  assert(reading.state == H2_PAL_BUTTON_STATE_PRESSED);
  level = 1;
  assert(h2_pal_button_read_single_button(api, 101, &reading) == 0);
  assert(reading.state == H2_PAL_BUTTON_STATE_RELEASED && configurations == 2);
  h2_pal_periph_info_t info;
  assert(h2_pal_periph_get(h2_esp_board_periph_api(), 101, &info) == 0);
  assert(strcmp(info.name, "boot") == 0);
  h2_pal_temperature_reading_t temp;
  assert(h2_pal_input_read_temperature(h2_esp_board_input_api(), 501, &temp) ==
         0);
  assert(temp.milli_celsius == 37250);
  return 0;
}
