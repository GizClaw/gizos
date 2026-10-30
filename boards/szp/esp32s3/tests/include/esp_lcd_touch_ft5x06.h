#ifndef H2_SZP_TEST_FT5X06_H
#define H2_SZP_TEST_FT5X06_H
#include "esp_lcd_panel_io.h"
#include <stdbool.h>

#define GPIO_NUM_NC (-1)
#define ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG() {0}
typedef void* esp_lcd_touch_handle_t;
typedef struct {
  uint16_t x_max, y_max;
  int rst_gpio_num, int_gpio_num;
  struct {
    unsigned reset, interrupt;
  } levels;
  struct {
    bool swap_xy, mirror_x, mirror_y;
  } flags;
} esp_lcd_touch_config_t;
typedef struct {
  uint16_t x, y;
} esp_lcd_touch_point_data_t;
esp_err_t esp_lcd_touch_new_i2c_ft5x06(esp_lcd_panel_io_handle_t io,
                                       const esp_lcd_touch_config_t* config,
                                       esp_lcd_touch_handle_t* out);
esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t handle);
esp_err_t esp_lcd_touch_get_data(esp_lcd_touch_handle_t handle,
                                 esp_lcd_touch_point_data_t* point,
                                 uint8_t* count, uint8_t max_points);
esp_err_t esp_lcd_touch_del(esp_lcd_touch_handle_t handle);
#endif
