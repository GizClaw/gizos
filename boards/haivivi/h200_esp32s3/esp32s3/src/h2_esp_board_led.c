#include "h2_esp_board_private.h"

#include "h2_esp_board_config.h"

#include "driver/ledc.h"
#include "led_strip.h"

static led_strip_handle_t s_strip;
static uint8_t s_strip_brightness = 100u;
static int s_white_led_configured;
static uint16_t s_white_led_duty_x100;

static h2_pal_result_t map_esp(esp_err_t err) {
  if (err == ESP_OK) {
    return H2_PAL_OK;
  }
  if (err == ESP_ERR_INVALID_ARG) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (err == ESP_ERR_NO_MEM) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  return H2_PAL_ERR_IO;
}

static h2_pal_result_t validate_strip_id(h2_pal_led_id_t id) {
  if (id == H2_H200_LED_STRIP_ID) {
    return H2_PAL_OK;
  }
  h2_pal_periph_info_t info;
  h2_pal_result_t rc = h2_pal_periph_get(h2_esp_board_periph_api(), id, &info);
  return rc == H2_PAL_OK ? H2_PAL_ERR_INVALID_ARG : rc;
}

static h2_pal_result_t ensure_strip(void) {
  if (s_strip != NULL) {
    return H2_PAL_OK;
  }
  const led_strip_config_t config = {
      .strip_gpio_num = H2_H200_LED_STRIP_GPIO,
      .max_leds = H2_H200_LED_STRIP_PIXEL_COUNT,
      .led_model = LED_MODEL_WS2812,
      .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
  };
  const led_strip_rmt_config_t rmt = {
      .resolution_hz = H2_H200_LED_STRIP_RMT_RES_HZ,
      .flags.with_dma = 0,
  };
  return map_esp(led_strip_new_rmt_device(&config, &rmt, &s_strip));
}

static h2_pal_led_color_t scale_color(h2_pal_led_color_t color) {
  return (h2_pal_led_color_t){
      .r = (uint8_t)(((uint16_t)color.r * s_strip_brightness) / 100u),
      .g = (uint8_t)(((uint16_t)color.g * s_strip_brightness) / 100u),
      .b = (uint8_t)(((uint16_t)color.b * s_strip_brightness) / 100u),
      .w = 0u,
  };
}

static h2_pal_result_t led_get_info(void* user, h2_pal_led_id_t id,
                                    h2_pal_led_info_t* out_info) {
  (void)user;
  if (out_info == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_pal_result_t rc = validate_strip_id(id);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  *out_info = (h2_pal_led_info_t){
      .id = id,
      .pixel_count = H2_H200_LED_STRIP_PIXEL_COUNT,
      .channels_per_pixel = H2_H200_LED_STRIP_CHANNELS_PER_PIXEL,
      .supports_brightness = 1u,
      .frame_commit_is_atomic = 1u,
  };
  return H2_PAL_OK;
}

static h2_pal_result_t led_set_frame(void* user, h2_pal_led_id_t id,
                                     const h2_pal_led_color_t* pixels,
                                     size_t pixel_count) {
  (void)user;
  h2_pal_result_t rc = validate_strip_id(id);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  if (pixels == NULL || pixel_count != H2_H200_LED_STRIP_PIXEL_COUNT) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  rc = ensure_strip();
  if (rc != H2_PAL_OK) {
    return rc;
  }
  for (size_t i = 0u; i < pixel_count; ++i) {
    h2_pal_led_color_t color = scale_color(pixels[i]);
    rc = map_esp(
        led_strip_set_pixel(s_strip, (uint32_t)i, color.r, color.g, color.b));
    if (rc != H2_PAL_OK) {
      return rc;
    }
  }
  return map_esp(led_strip_refresh(s_strip));
}

static h2_pal_result_t led_set_solid(void* user, h2_pal_led_id_t id,
                                     h2_pal_led_color_t color) {
  h2_pal_led_color_t pixels[H2_H200_LED_STRIP_PIXEL_COUNT];
  for (size_t i = 0u; i < H2_H200_LED_STRIP_PIXEL_COUNT; ++i) {
    pixels[i] = color;
  }
  return led_set_frame(user, id, pixels, H2_H200_LED_STRIP_PIXEL_COUNT);
}

static h2_pal_result_t led_clear(void* user, h2_pal_led_id_t id) {
  (void)user;
  h2_pal_result_t rc = validate_strip_id(id);
  if (rc == H2_PAL_OK) {
    rc = ensure_strip();
  }
  return rc == H2_PAL_OK ? map_esp(led_strip_clear(s_strip)) : rc;
}

static h2_pal_result_t led_set_brightness(void* user, h2_pal_led_id_t id,
                                          uint8_t percent) {
  (void)user;
  h2_pal_result_t rc = validate_strip_id(id);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  if (percent > 100u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  s_strip_brightness = percent;
  return H2_PAL_OK;
}

static h2_pal_result_t ensure_white_led(void) {
  if (s_white_led_configured) {
    return H2_PAL_OK;
  }
  const ledc_timer_config_t timer = {
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .duty_resolution = LEDC_TIMER_13_BIT,
      .timer_num = (ledc_timer_t)H2_H200_WHITE_LED_LEDC_TIMER,
      .freq_hz = H2_H200_WHITE_LED_FREQUENCY_HZ,
      .clk_cfg = LEDC_AUTO_CLK,
  };
  const ledc_channel_config_t channel = {
      .gpio_num = H2_H200_WHITE_LED_GPIO,
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .channel = (ledc_channel_t)H2_H200_WHITE_LED_LEDC_CHANNEL,
      .intr_type = LEDC_INTR_DISABLE,
      .timer_sel = (ledc_timer_t)H2_H200_WHITE_LED_LEDC_TIMER,
      .duty = 0u,
      .hpoint = 0u,
  };
  if (ledc_timer_config(&timer) != ESP_OK ||
      ledc_channel_config(&channel) != ESP_OK) {
    return H2_PAL_ERR_IO;
  }
  s_white_led_configured = 1;
  return H2_PAL_OK;
}

static h2_pal_result_t white_led_set(void* user, h2_pal_periph_id_t id,
                                     uint16_t duty_x100) {
  (void)user;
  if (id != H2_H200_WHITE_LED_ID) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  if (duty_x100 > H2_PAL_PWM_SWITCH_DUTY_MAX_X100) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_pal_result_t rc = ensure_white_led();
  if (rc != H2_PAL_OK) {
    return rc;
  }
  const uint32_t max_duty = (1u << H2_H200_WHITE_LED_DUTY_RESOLUTION_BITS) - 1u;
  const uint32_t duty = ((uint32_t)duty_x100 * max_duty + 5000u) / 10000u;
  if (ledc_set_duty(LEDC_LOW_SPEED_MODE,
                    (ledc_channel_t)H2_H200_WHITE_LED_LEDC_CHANNEL,
                    duty) != ESP_OK ||
      ledc_update_duty(LEDC_LOW_SPEED_MODE,
                       (ledc_channel_t)H2_H200_WHITE_LED_LEDC_CHANNEL) !=
          ESP_OK) {
    return H2_PAL_ERR_IO;
  }
  s_white_led_duty_x100 = duty_x100;
  return H2_PAL_OK;
}

static h2_pal_result_t white_led_get(void* user, h2_pal_periph_id_t id,
                                     uint16_t* out_duty_x100) {
  (void)user;
  if (out_duty_x100 == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (id != H2_H200_WHITE_LED_ID) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  *out_duty_x100 = s_white_led_duty_x100;
  return H2_PAL_OK;
}

const h2_pal_led_api_t* h2_esp_board_led_api(void) {
  static const h2_pal_led_vtable_t vtable = {
      .get_info = led_get_info,
      .set_frame = led_set_frame,
      .set_solid = led_set_solid,
      .clear = led_clear,
      .set_brightness_percent = led_set_brightness,
  };
  static const h2_pal_led_api_t api = {.vtable = &vtable};
  return &api;
}

h2_pal_pwm_switch_api_t* h2_esp_board_pwm_switch_api(void) {
  static const h2_pal_pwm_switch_vtable_t vtable = {
      .set_duty = white_led_set,
      .get_duty = white_led_get,
  };
  static h2_pal_pwm_switch_api_t api = {.vtable = &vtable};
  return &api;
}
