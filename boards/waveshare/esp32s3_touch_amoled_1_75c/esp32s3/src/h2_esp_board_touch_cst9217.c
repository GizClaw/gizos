#include "h2_esp_board_private.h"
#include "h2_esp_board_config.h"
#include "h2_esp_board_internal.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_cst9217.h"

#include <stdbool.h>

typedef struct touch_state {
  esp_lcd_panel_io_handle_t io;
  esp_lcd_touch_handle_t handle;
  int32_t x;
  int32_t y;
  bool pressed;
} touch_state_t;

static touch_state_t s_touch;

static h2_pal_result_t touch_result(esp_err_t err) {
  if (err == ESP_OK) {
    return H2_PAL_OK;
  }
  if (err == ESP_ERR_NO_MEM) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  if (err == ESP_ERR_TIMEOUT) {
    return H2_PAL_ERR_TIMEOUT;
  }
  if (err == ESP_ERR_INVALID_ARG) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return H2_PAL_ERR_IO;
}

static h2_pal_result_t touch_open(void* user) {
  (void)user;
  if (s_touch.handle != NULL) {
    return H2_PAL_OK;
  }
  i2c_master_bus_handle_t bus = h2_ws175c_board_i2c_bus();
  if (bus == NULL) {
    return H2_PAL_ERR_UNAVAILABLE;
  }
  esp_lcd_panel_io_i2c_config_t io_config =
      ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
  io_config.scl_speed_hz = 400000u;
  esp_err_t err = esp_lcd_new_panel_io_i2c(bus, &io_config, &s_touch.io);
  if (err != ESP_OK) {
    return touch_result(err);
  }
  const esp_lcd_touch_config_t config = {
      .x_max = H2_WS175C_LCD_WIDTH,
      .y_max = H2_WS175C_LCD_HEIGHT,
      .rst_gpio_num = H2_WS175C_TOUCH_RESET_GPIO,
      .int_gpio_num = H2_WS175C_TOUCH_INTERRUPT_GPIO,
      .levels = {.reset = 0, .interrupt = 0},
      /* Match Waveshare's panel orientation. */
      .flags = {.swap_xy = false, .mirror_x = true, .mirror_y = true},
  };
  err = esp_lcd_touch_new_i2c_cst9217(s_touch.io, &config, &s_touch.handle);
  if (err != ESP_OK) {
    (void)esp_lcd_panel_io_del(s_touch.io);
    s_touch.io = NULL;
    s_touch.handle = NULL;
  }
  s_touch.pressed = false;
  return touch_result(err);
}

static h2_pal_result_t touch_get_info(void* user, h2_pal_touch_info_t* info) {
  (void)user;
  if (info == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (s_touch.handle == NULL) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  *info = (h2_pal_touch_info_t){
      .width = H2_WS175C_LCD_WIDTH,
      .height = H2_WS175C_LCD_HEIGHT,
  };
  return H2_PAL_OK;
}

static h2_pal_result_t touch_poll_event(void* user,
                                        h2_pal_touch_event_t* event) {
  (void)user;
  if (event == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (s_touch.handle == NULL) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  esp_err_t err = esp_lcd_touch_read_data(s_touch.handle);
  if (err == ESP_ERR_INVALID_RESPONSE) {
    /* No fresh controller frame: preserve the current contact. */
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  if (err != ESP_OK) {
    return touch_result(err);
  }
  esp_lcd_touch_point_data_t point = {0};
  uint8_t count = 0u;
  err = esp_lcd_touch_get_data(s_touch.handle, &point, &count, 1u);
  if (err != ESP_OK) {
    return touch_result(err);
  }
  if (count == 0u) {
    if (!s_touch.pressed) {
      return H2_PAL_ERR_WOULD_BLOCK;
    }
    s_touch.pressed = false;
    *event = (h2_pal_touch_event_t){
        .kind = H2_PAL_TOUCH_EVENT_UP,
        .x = s_touch.x,
        .y = s_touch.y,
    };
    return H2_PAL_OK;
  }
  uint16_t x = point.x;
  uint16_t y = point.y;
  if (x >= H2_WS175C_LCD_WIDTH) {
    x = H2_WS175C_LCD_WIDTH - 1u;
  }
  if (y >= H2_WS175C_LCD_HEIGHT) {
    y = H2_WS175C_LCD_HEIGHT - 1u;
  }
  if (s_touch.pressed && x == s_touch.x && y == s_touch.y) {
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  *event = (h2_pal_touch_event_t){
      .kind =
          s_touch.pressed ? H2_PAL_TOUCH_EVENT_MOVE : H2_PAL_TOUCH_EVENT_DOWN,
      .x = x,
      .y = y,
  };
  s_touch.x = x;
  s_touch.y = y;
  s_touch.pressed = true;
  return H2_PAL_OK;
}

static h2_pal_result_t touch_close(void* user) {
  (void)user;
  if (s_touch.handle == NULL) {
    return H2_PAL_OK;
  }
  esp_err_t err = esp_lcd_touch_del(s_touch.handle);
  if (err != ESP_OK) {
    return touch_result(err);
  }
  s_touch.handle = NULL;
  s_touch.pressed = false;
  err = esp_lcd_panel_io_del(s_touch.io);
  s_touch.io = NULL;
  return touch_result(err);
}

const h2_pal_touch_api_t* h2_esp_board_touch_api(void) {
  static const h2_pal_touch_vtable_t vtable = {
      .open = touch_open,
      .get_info = touch_get_info,
      .poll_event = touch_poll_event,
      .close = touch_close,
  };
  static const h2_pal_touch_api_t api = {.vtable = &vtable};
  return &api;
}
