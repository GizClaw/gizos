#include "h2_esp_board_private.h"

#include "h2_esp_board_config.h"

#include "driver/ledc.h"

static int s_vibration_configured;
static uint16_t s_vibration_duty_x100;

static h2_pal_result_t ensure_vibration_configured(void) {
  if (s_vibration_configured != 0) {
    return H2_PAL_OK;
  }
  const ledc_timer_config_t timer = {
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .duty_resolution = LEDC_TIMER_13_BIT,
      .timer_num = (ledc_timer_t)H2_TIGA_ESP_VIBRATION_LEDC_TIMER,
      .freq_hz = H2_TIGA_ESP_VIBRATION_FREQUENCY_HZ,
      .clk_cfg = LEDC_AUTO_CLK,
  };
  const ledc_channel_config_t channel = {
      .gpio_num = H2_TIGA_ESP_VIBRATION_GPIO,
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .channel = (ledc_channel_t)H2_TIGA_ESP_VIBRATION_LEDC_CHANNEL,
      .intr_type = LEDC_INTR_DISABLE,
      .timer_sel = (ledc_timer_t)H2_TIGA_ESP_VIBRATION_LEDC_TIMER,
      .duty = 0u,
      .hpoint = 0u,
      .flags.output_invert = 0u,
  };
  if (ledc_timer_config(&timer) != ESP_OK ||
      ledc_channel_config(&channel) != ESP_OK) {
    return H2_PAL_ERR_IO;
  }
  s_vibration_configured = 1;
  return H2_PAL_OK;
}

static h2_pal_result_t set_duty(void* user, h2_pal_periph_id_t id,
                                uint16_t duty_x100) {
  (void)user;
  if (id != H2_TIGA_ESP_VIBRATION_ID) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  if (duty_x100 > H2_PAL_PWM_SWITCH_DUTY_MAX_X100) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_pal_result_t rc = ensure_vibration_configured();
  if (rc != H2_PAL_OK) {
    return rc;
  }
  const uint32_t max_duty =
      (1u << H2_TIGA_ESP_VIBRATION_DUTY_RESOLUTION_BITS) - 1u;
  const uint32_t duty = ((uint32_t)duty_x100 * max_duty + 5000u) / 10000u;
  if (ledc_set_duty(LEDC_LOW_SPEED_MODE,
                    (ledc_channel_t)H2_TIGA_ESP_VIBRATION_LEDC_CHANNEL,
                    duty) != ESP_OK ||
      ledc_update_duty(LEDC_LOW_SPEED_MODE,
                       (ledc_channel_t)H2_TIGA_ESP_VIBRATION_LEDC_CHANNEL) !=
          ESP_OK) {
    return H2_PAL_ERR_IO;
  }
  s_vibration_duty_x100 = duty_x100;
  return H2_PAL_OK;
}

static h2_pal_result_t get_duty(void* user, h2_pal_periph_id_t id,
                                uint16_t* out_duty_x100) {
  (void)user;
  if (out_duty_x100 == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (id != H2_TIGA_ESP_VIBRATION_ID) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  *out_duty_x100 = s_vibration_duty_x100;
  return H2_PAL_OK;
}

h2_pal_pwm_switch_api_t* h2_esp_board_pwm_switch_api(void) {
  static const h2_pal_pwm_switch_vtable_t vtable = {
      .set_duty = set_duty,
      .get_duty = get_duty,
  };
  static h2_pal_pwm_switch_api_t api = {
      .user = NULL,
      .vtable = &vtable,
  };
  return &api;
}
