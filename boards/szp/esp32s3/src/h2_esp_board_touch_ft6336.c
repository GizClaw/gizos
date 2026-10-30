#include "h2_esp_board_private.h"
#include "h2_esp_szp_board_internal.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"

#include <stdbool.h>

#define H2_SZP_TOUCH_ADDRESS 0x38u
#define H2_SZP_TOUCH_WIDTH 320u
#define H2_SZP_TOUCH_HEIGHT 240u

typedef struct szp_touch_state {
  esp_lcd_panel_io_handle_t io;
  esp_lcd_touch_handle_t handle;
  int32_t x;
  int32_t y;
  bool pressed;
} szp_touch_state_t;

static szp_touch_state_t s_touch;

static h2_pal_result_t touch_result(esp_err_t err) {
  if (err == ESP_OK) return H2_PAL_OK;
  if (err == ESP_ERR_NO_MEM) return H2_PAL_ERR_NO_MEMORY;
  if (err == ESP_ERR_TIMEOUT) return H2_PAL_ERR_TIMEOUT;
  if (err == ESP_ERR_NOT_FOUND) return H2_PAL_ERR_NOT_FOUND;
  if (err == ESP_ERR_INVALID_ARG) return H2_PAL_ERR_INVALID_ARG;
  return H2_PAL_ERR_IO;
}

static h2_pal_result_t touch_open(void* user) {
  (void)user;
  if (s_touch.handle != NULL) return H2_PAL_OK;
  if (s_touch.io != NULL) {
    esp_err_t err = esp_lcd_panel_io_del(s_touch.io);
    if (err != ESP_OK) return touch_result(err);
    s_touch.io = NULL;
  }
  i2c_master_bus_handle_t bus = h2_esp_szp_board_i2c_bus();
  if (bus == NULL) return H2_PAL_ERR_UNAVAILABLE;
  esp_err_t err = i2c_master_probe(bus, H2_SZP_TOUCH_ADDRESS, 20);
  if (err != ESP_OK) return touch_result(err);
  esp_lcd_panel_io_i2c_config_t io_config =
      ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
  io_config.scl_speed_hz = 100000u;
  err = esp_lcd_new_panel_io_i2c(bus, &io_config, &s_touch.io);
  if (err != ESP_OK) return touch_result(err);
  const esp_lcd_touch_config_t config = {
      /* LCKFB's FT6336 reports portrait coordinates. Mirror native X before
       * swapping axes to match the SZP's 320x240 landscape ST7789 viewport. */
      .x_max = H2_SZP_TOUCH_HEIGHT,
      .y_max = H2_SZP_TOUCH_WIDTH,
      .rst_gpio_num = GPIO_NUM_NC,
      .int_gpio_num = GPIO_NUM_NC,
      .levels = {.reset = 0, .interrupt = 0},
      .flags = {.swap_xy = true, .mirror_x = true, .mirror_y = false},
  };
  err = esp_lcd_touch_new_i2c_ft5x06(s_touch.io, &config, &s_touch.handle);
  if (err != ESP_OK) {
    s_touch.handle = NULL;
    if (esp_lcd_panel_io_del(s_touch.io) == ESP_OK) s_touch.io = NULL;
    return touch_result(err);
  }
  s_touch.pressed = false;
  ESP_LOGI("h2_szp_touch", "FT6336 ready address=0x38 viewport=320x240");
  return H2_PAL_OK;
}

static h2_pal_result_t touch_get_info(void* user,
                                      h2_pal_touch_info_t* out_info) {
  (void)user;
  if (out_info == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (s_touch.handle == NULL) return H2_PAL_ERR_INVALID_STATE;
  *out_info = (h2_pal_touch_info_t){.width = H2_SZP_TOUCH_WIDTH,
                                    .height = H2_SZP_TOUCH_HEIGHT};
  return H2_PAL_OK;
}

static h2_pal_result_t touch_poll_event(void* user,
                                        h2_pal_touch_event_t* event) {
  (void)user;
  if (event == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (s_touch.handle == NULL) return H2_PAL_ERR_INVALID_STATE;
  esp_err_t err = esp_lcd_touch_read_data(s_touch.handle);
  if (err == ESP_ERR_INVALID_RESPONSE) return H2_PAL_ERR_WOULD_BLOCK;
  if (err != ESP_OK) return touch_result(err);
  esp_lcd_touch_point_data_t point = {0};
  uint8_t count = 0;
  err = esp_lcd_touch_get_data(s_touch.handle, &point, &count, 1);
  if (err != ESP_OK) return touch_result(err);
  if (count == 0) {
    if (!s_touch.pressed) return H2_PAL_ERR_WOULD_BLOCK;
    s_touch.pressed = false;
    *event = (h2_pal_touch_event_t){
        .kind = H2_PAL_TOUCH_EVENT_UP, .x = s_touch.x, .y = s_touch.y};
    return H2_PAL_OK;
  }
  int32_t x = point.x < H2_SZP_TOUCH_WIDTH ? point.x : H2_SZP_TOUCH_WIDTH - 1u;
  int32_t y =
      point.y < H2_SZP_TOUCH_HEIGHT ? point.y : H2_SZP_TOUCH_HEIGHT - 1u;
  if (s_touch.pressed && x == s_touch.x && y == s_touch.y)
    return H2_PAL_ERR_WOULD_BLOCK;
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
  if (s_touch.handle != NULL) {
    esp_err_t err = esp_lcd_touch_del(s_touch.handle);
    if (err != ESP_OK) return touch_result(err);
    s_touch.handle = NULL;
    s_touch.pressed = false;
  }
  if (s_touch.io != NULL) {
    esp_err_t err = esp_lcd_panel_io_del(s_touch.io);
    if (err != ESP_OK) return touch_result(err);
    s_touch.io = NULL;
  }
  return H2_PAL_OK;
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
