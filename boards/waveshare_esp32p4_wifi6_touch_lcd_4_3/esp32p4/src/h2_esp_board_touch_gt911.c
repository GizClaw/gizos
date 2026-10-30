#include "h2_esp_board_private.h"
#include "h2_esp_p4_board_internal.h"

#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>

static struct {
  esp_lcd_panel_io_handle_t io;
  esp_lcd_touch_handle_t handle;
  int32_t x, y;
  bool pressed;
} s_touch;

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
  i2c_master_bus_handle_t bus = h2_esp_p4_board_i2c_bus();
  if (bus == NULL) return H2_PAL_ERR_UNAVAILABLE;
  /* Reset before probing: with INT unconnected the GT911 can change address
   * during reset. Keep the driver from resetting it after address selection. */
  esp_err_t err = gpio_set_direction(23, GPIO_MODE_OUTPUT);
  if (err == ESP_OK) err = gpio_set_level(23, 0);
  if (err != ESP_OK) return touch_result(err);
  vTaskDelay(pdMS_TO_TICKS(10));
  err = gpio_set_level(23, 1);
  if (err != ESP_OK) return touch_result(err);
  vTaskDelay(pdMS_TO_TICKS(50));
  esp_lcd_panel_io_i2c_config_t io = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
  err = i2c_master_probe(bus, io.dev_addr, 20);
  if (err == ESP_ERR_NOT_FOUND) {
    io.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
    err = i2c_master_probe(bus, io.dev_addr, 20);
  }
  if (err != ESP_OK) return touch_result(err);
  io.scl_speed_hz = 100000;
  err = esp_lcd_new_panel_io_i2c(bus, &io, &s_touch.io);
  if (err != ESP_OK) return touch_result(err);
  /* Match the display's native 480x800 scan. */
  const esp_lcd_touch_config_t config = {
      .x_max = 480,
      .y_max = 800,
      .rst_gpio_num = GPIO_NUM_NC,
      .int_gpio_num = GPIO_NUM_NC,
      .levels = {.reset = 0, .interrupt = 0},
      .flags = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
  };
  err = esp_lcd_touch_new_i2c_gt911(s_touch.io, &config, &s_touch.handle);
  if (err != ESP_OK) {
    s_touch.handle = NULL;
    if (esp_lcd_panel_io_del(s_touch.io) == ESP_OK) s_touch.io = NULL;
    return touch_result(err);
  }
  s_touch.pressed = false;
  ESP_LOGI("h2_p4_touch", "GT911 ready address=0x%02x viewport=480x800",
           (unsigned)io.dev_addr);
  return H2_PAL_OK;
}

static h2_pal_result_t touch_get_info(void* user, h2_pal_touch_info_t* out) {
  (void)user;
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (s_touch.handle == NULL) return H2_PAL_ERR_INVALID_STATE;
  *out = (h2_pal_touch_info_t){.width = 480, .height = 800};
  return H2_PAL_OK;
}

static h2_pal_result_t touch_poll_event(void* user, h2_pal_touch_event_t* out) {
  (void)user;
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
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
    *out = (h2_pal_touch_event_t){
        .kind = H2_PAL_TOUCH_EVENT_UP, .x = s_touch.x, .y = s_touch.y};
    return H2_PAL_OK;
  }
  int32_t x = point.x < 480 ? point.x : 479;
  int32_t y = point.y < 800 ? point.y : 799;
  if (s_touch.pressed && x == s_touch.x && y == s_touch.y)
    return H2_PAL_ERR_WOULD_BLOCK;
  *out =
      (h2_pal_touch_event_t){.kind = s_touch.pressed ? H2_PAL_TOUCH_EVENT_MOVE
                                                     : H2_PAL_TOUCH_EVENT_DOWN,
                             .x = x,
                             .y = y};
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
  static const h2_pal_touch_vtable_t ops = {.open = touch_open,
                                            .get_info = touch_get_info,
                                            .poll_event = touch_poll_event,
                                            .close = touch_close};
  static const h2_pal_touch_api_t api = {.vtable = &ops};
  return &api;
}
