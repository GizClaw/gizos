#include "h2_esp_board_private.h"

#include "h2_esp_board_config.h"
#include "h2_esp_h200_board_internal.h"
#include "h2_esp_platform_core.h"

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "freertos/FreeRTOS.h"

static int s_power_hold_enabled;

h2_pal_result_t h2_esp_platform_power_before_reboot(uint32_t reason);

static const h2_pal_power_api_t* base_power(void) {
  return h2_esp_platform_power_api();
}

static h2_pal_result_t power_set_hold(void* user, int enabled) {
  (void)user;
  const int requested = enabled ? 1 : 0;
  if (requested == s_power_hold_enabled) {
    return H2_PAL_OK;
  }
  if (requested == 0) {
    (void)h2_esp_platform_power_before_reboot(0u);
    (void)h2_esp_h200_board_set_codec_power(0);
  }
  const gpio_num_t gpio = (gpio_num_t)H2_H200_POWER_HOLD_GPIO;
  esp_err_t err = rtc_gpio_init(gpio);
  if (err == ESP_OK && requested == 0) {
    err = rtc_gpio_hold_dis(gpio);
  }
  if (err == ESP_OK) {
    err = rtc_gpio_set_direction(gpio, RTC_GPIO_MODE_OUTPUT_ONLY);
  }
  if (err == ESP_OK) {
    err = rtc_gpio_set_level(gpio, requested ? 1u : 0u);
  }
  if (err == ESP_OK && requested) {
    err = rtc_gpio_hold_en(gpio);
  }
  if (err != ESP_OK) {
    return H2_PAL_ERR_IO;
  }
  s_power_hold_enabled = requested;
  return H2_PAL_OK;
}

static h2_pal_result_t power_get_hold(void* user,
                                      h2_pal_power_hold_state_t* out_state) {
  (void)user;
  if (out_state == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  out_state->enabled = s_power_hold_enabled;
  return H2_PAL_OK;
}

h2_pal_result_t h2_esp_platform_power_before_reboot(uint32_t reason) {
  (void)reason;
  (void)h2_esp_h200_board_set_pa(0);
  return H2_PAL_OK;
}

const h2_pal_power_api_t* h2_esp_board_power_api(void) {
  static portMUX_TYPE init_lock = portMUX_INITIALIZER_UNLOCKED;
  static h2_pal_power_vtable_t vtable;
  static h2_pal_power_api_t api;
  const h2_pal_power_api_t* native = base_power();
  portENTER_CRITICAL(&init_lock);
  if (api.vtable == NULL) {
    /* Keep native boot/image operations and their borrowed callback context. */
    vtable = *native->vtable;
    vtable.set_hold = power_set_hold;
    vtable.get_hold = power_get_hold;
    api = *native;
    api.vtable = &vtable;
  }
  portEXIT_CRITICAL(&init_lock);
  return &api;
}
