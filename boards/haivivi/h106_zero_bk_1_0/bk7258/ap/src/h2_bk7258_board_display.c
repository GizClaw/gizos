#include "h2_zero_bk_display_capture.h"
#include "h2_zero_bk_lcd.h"
#ifdef H2_ZERO_BK_DISPLAY_HOST_TEST
#include "h2_zero_bk_display_test_sdk.h"
#else
#include "driver/gpio.h"
#include "driver/pwm.h"
#include "gpio_driver.h"
#include "h2_bk7258_board_private.h"
#endif
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define H2_ZERO_BK_BACKLIGHT_PIN GPIO_19
#define H2_ZERO_BK_BACKLIGHT_PWM PWM_CH_1
#define H2_ZERO_BK_BACKLIGHT_PERIOD 26000u
#define H2_ZERO_BK_DISPLAY_PIXELS (H2_ZERO_BK_LCD_WIDTH * H2_ZERO_BK_LCD_HEIGHT)
#ifdef H2_ZERO_BK_DISPLAY_HOST_TEST
_Alignas(64)
#else
__attribute__((section(".psram.bss"), aligned(64)))
#endif
    uint16_t g_h2_zero_bk_display_shadow[H2_ZERO_BK_DISPLAY_PIXELS];

typedef struct h2_zero_bk_display_state {
  uint16_t* shadow;
  bool initialized;
  bool pwm_owned;
  bool pwm_running;
  uint32_t brightness;
} h2_zero_bk_display_state_t;
static h2_zero_bk_display_state_t s_display_state = {
    .shadow = g_h2_zero_bk_display_shadow,
};

static uint16_t rgb888_to_rgb565(const uint8_t* p) {
  return (uint16_t)(((p[0] & 0xf8u) << 8) | ((p[1] & 0xfcu) << 3) |
                    (p[2] >> 3));
}
static uint16_t rgb444_to_rgb565(uint16_t pixel) {
  uint16_t r = (pixel >> 8) & 15u, g = (pixel >> 4) & 15u, b = pixel & 15u;
  return (uint16_t)((((r << 1) | (r >> 3)) << 11) |
                    (((g << 2) | (g >> 2)) << 5) | ((b << 1) | (b >> 3)));
}
static uint16_t rgb565_to_wire_order(uint16_t p) {
  return (uint16_t)((p << 8) | (p >> 8));
}
static int backlight_release(h2_zero_bk_display_state_t* s) {
  if (s->pwm_owned) {
    if (bk_pwm_deinit(H2_ZERO_BK_BACKLIGHT_PWM) != BK_OK) {
      s->pwm_running = false;
      return H2_DISPLAY_ERR_IO;
    }
    s->pwm_owned = false;
    s->pwm_running = false;
  }
  return H2_DISPLAY_OK;
}
static int backlight_set(h2_zero_bk_display_state_t* s, uint32_t percent) {
  if (percent == 0u || percent == 100u) {
    int rc = backlight_release(s);
    if (rc) return rc;
    const gpio_config_t config = {.io_mode = GPIO_OUTPUT_ENABLE,
                                  .pull_mode = GPIO_PULL_DISABLE,
                                  .func_mode = GPIO_SECOND_FUNC_DISABLE};
    if (gpio_dev_unmap(H2_ZERO_BK_BACKLIGHT_PIN) != BK_OK ||
        bk_gpio_set_config(H2_ZERO_BK_BACKLIGHT_PIN, &config) != BK_OK)
      return H2_DISPLAY_ERR_IO;
    rc = percent ? bk_gpio_set_output_high(H2_ZERO_BK_BACKLIGHT_PIN)
                 : bk_gpio_set_output_low(H2_ZERO_BK_BACKLIGHT_PIN);
    if (rc != BK_OK) return H2_DISPLAY_ERR_IO;
  } else {
    if (s->pwm_owned && !s->pwm_running) {
      int rc = backlight_release(s);
      if (rc) return rc;
    }
    if (!s->pwm_owned) {
      if (bk_pwm_driver_init() != BK_OK) return H2_DISPLAY_ERR_IO;
      const pwm_init_config_t config = {
          .period_cycle = H2_ZERO_BK_BACKLIGHT_PERIOD,
          .duty_cycle = H2_ZERO_BK_BACKLIGHT_PERIOD * percent / 100u};
      /* Init/start may fail after acquisition; retain cleanup ownership. */
      s->pwm_owned = true;
      if (bk_pwm_init(H2_ZERO_BK_BACKLIGHT_PWM, &config) != BK_OK ||
          bk_pwm_start(H2_ZERO_BK_BACKLIGHT_PWM) != BK_OK) {
        (void)backlight_release(s);
        return H2_DISPLAY_ERR_IO;
      }
      s->pwm_running = true;
    } else {
      pwm_period_duty_config_t config = {
          .period_cycle = H2_ZERO_BK_BACKLIGHT_PERIOD,
          .duty_cycle = H2_ZERO_BK_BACKLIGHT_PERIOD * percent / 100u};
      if (bk_pwm_set_period_duty(H2_ZERO_BK_BACKLIGHT_PWM, &config) != BK_OK)
        return H2_DISPLAY_ERR_IO;
    }
  }
  s->brightness = percent;
  return H2_DISPLAY_OK;
}
static int display_close(void* user) {
  h2_zero_bk_display_state_t* s = user;
  if (!s) return H2_DISPLAY_ERR_INVALID_ARG;
  if (!s->initialized && !s->pwm_owned)
    return h2_zero_bk_lcd_deinit() == 0 ? 0 : H2_DISPLAY_ERR_IO;
  int rc = backlight_set(s, 0u);
  if (rc) return rc;
  if (h2_zero_bk_lcd_deinit() != 0) return H2_DISPLAY_ERR_IO;
  s->initialized = false;
  return 0;
}
static int display_open(void* user) {
  h2_zero_bk_display_state_t* s = user;
  if (!s) return H2_DISPLAY_ERR_INVALID_ARG;
  if (s->initialized) return 0;
  int rc = display_close(s);
  if (rc) return rc;
  memset(s->shadow, 0, sizeof(g_h2_zero_bk_display_shadow));
  if (h2_zero_bk_lcd_init() != 0) return H2_DISPLAY_ERR_IO;
  rc = backlight_set(s, 100u);
  if (rc) {
    (void)backlight_release(s);
    (void)h2_zero_bk_lcd_deinit();
    return rc;
  }
  s->initialized = true;
  return 0;
}
static int display_get_info(void* user, h2_display_info_t* info) {
  h2_zero_bk_display_state_t* s = user;
  if (!s || !info) return H2_DISPLAY_ERR_INVALID_ARG;
  if (!s->initialized) return H2_DISPLAY_ERR_INVALID_STATE;
  *info = (h2_display_info_t){H2_ZERO_BK_LCD_WIDTH, H2_ZERO_BK_LCD_HEIGHT,
                              H2_DISPLAY_PIXEL_RGB565};
  return 0;
}
static int display_draw_bitmap(void* user, const h2_display_rect_t* rect,
                               const void* pixels, size_t stride,
                               h2_display_pixel_format_t format) {
  h2_zero_bk_display_state_t* s = user;
  if (!s || !rect || !pixels) return H2_DISPLAY_ERR_INVALID_ARG;
  if (!s->initialized) return H2_DISPLAY_ERR_INVALID_STATE;
  if (rect->width <= 0 || rect->height <= 0) return H2_DISPLAY_ERR_INVALID_ARG;
  if (format != H2_DISPLAY_PIXEL_RGB565 && format != H2_DISPLAY_PIXEL_RGB444 &&
      format != H2_DISPLAY_PIXEL_RGB888)
    return H2_DISPLAY_ERR_UNSUPPORTED;
  const size_t pixel_size = format == H2_DISPLAY_PIXEL_RGB888 ? 3u : 2u;
  if ((size_t)rect->width > SIZE_MAX / pixel_size)
    return H2_DISPLAY_ERR_INVALID_ARG;
  const size_t row_bytes = (size_t)rect->width * pixel_size;
  if (stride < row_bytes ||
      (rect->height > 1 &&
       stride > (SIZE_MAX - row_bytes) / (size_t)(rect->height - 1)))
    return H2_DISPLAY_ERR_INVALID_ARG;
  const int64_t right = (int64_t)rect->x + rect->width;
  const int64_t bottom = (int64_t)rect->y + rect->height;
  if (rect->x >= H2_ZERO_BK_LCD_WIDTH || rect->y >= H2_ZERO_BK_LCD_HEIGHT ||
      right <= 0 || bottom <= 0)
    return H2_DISPLAY_ERR_INVALID_ARG;
  const int x0 = rect->x < 0 ? 0 : rect->x, y0 = rect->y < 0 ? 0 : rect->y;
  const int x1 =
      right > H2_ZERO_BK_LCD_WIDTH ? H2_ZERO_BK_LCD_WIDTH : (int)right;
  const int y1 =
      bottom > H2_ZERO_BK_LCD_HEIGHT ? H2_ZERO_BK_LCD_HEIGHT : (int)bottom;
  for (int y = y0; y < y1; ++y) {
    const uint8_t* src = (const uint8_t*)pixels +
                         (size_t)((int64_t)y - rect->y) * stride +
                         (size_t)((int64_t)x0 - rect->x) * pixel_size;
    uint16_t* dst = s->shadow + (size_t)y * H2_ZERO_BK_LCD_WIDTH + x0;
    for (int x = x0; x < x1; ++x) {
      uint16_t pixel;
      if (format == H2_DISPLAY_PIXEL_RGB888)
        pixel = rgb888_to_rgb565(src);
      else {
        memcpy(&pixel, src, sizeof(pixel));
        if (format == H2_DISPLAY_PIXEL_RGB444) pixel = rgb444_to_rgb565(pixel);
      }
      *dst++ = rgb565_to_wire_order(pixel);
      src += pixel_size;
    }
  }
  return 0;
}
static int display_present(void* user) {
  h2_zero_bk_display_state_t* s = user;
  if (!s) return H2_DISPLAY_ERR_INVALID_ARG;
  if (!s->initialized) return H2_DISPLAY_ERR_INVALID_STATE;
  return h2_zero_bk_lcd_blit_rgb565_fullscreen(
             (uint8_t*)s->shadow, sizeof(g_h2_zero_bk_display_shadow), 0) == 0
             ? 0
             : H2_DISPLAY_ERR_IO;
}
static int display_set_brightness(void* user, uint32_t percent) {
  h2_zero_bk_display_state_t* s = user;
  if (!s || percent > 100u) return H2_DISPLAY_ERR_INVALID_ARG;
  if (!s->initialized) return H2_DISPLAY_ERR_INVALID_STATE;
  return backlight_set(s, percent);
}
uint32_t h2_zero_bk_display_backlight_percent(void) {
  return s_display_state.brightness;
}
h2_pal_display_t* h2_bk7258_board_display(void) {
  static const h2_pal_display_vtable_t vtable = {
      .open = display_open,
      .get_info = display_get_info,
      .draw_bitmap = display_draw_bitmap,
      .present = display_present,
      .set_brightness_percent = display_set_brightness,
      .close = display_close};
  static h2_pal_display_t display = {.user = &s_display_state,
                                     .vtable = &vtable};
  return &display;
}
int h2_bk7258_board_display_black(void) {
  h2_pal_display_t* display = h2_bk7258_board_display();
  int rc = h2_pal_display_open(display);
  if (rc) return rc;
  memset(s_display_state.shadow, 0, sizeof(g_h2_zero_bk_display_shadow));
  return h2_pal_display_present(display);
}
