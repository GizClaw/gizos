#ifndef H2_SZP_TEST_PANEL_IO_H
#define H2_SZP_TEST_PANEL_IO_H
#include "driver/i2c_master.h"
typedef void* esp_lcd_panel_io_handle_t;
typedef struct {
  unsigned scl_speed_hz;
} esp_lcd_panel_io_i2c_config_t;
enum {
  ESP_OK,
  ESP_ERR_NO_MEM,
  ESP_ERR_TIMEOUT,
  ESP_ERR_NOT_FOUND,
  ESP_ERR_INVALID_ARG,
  ESP_ERR_INVALID_RESPONSE,
  ESP_FAIL,
};
esp_err_t esp_lcd_new_panel_io_i2c(i2c_master_bus_handle_t bus,
                                   const esp_lcd_panel_io_i2c_config_t* config,
                                   esp_lcd_panel_io_handle_t* out);
esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t io);
#endif
