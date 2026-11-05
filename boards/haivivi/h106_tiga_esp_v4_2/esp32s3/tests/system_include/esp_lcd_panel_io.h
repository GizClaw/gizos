#ifndef H2_TEST_SHADOW_ESP_LCD_PANEL_IO_H
#define H2_TEST_SHADOW_ESP_LCD_PANEL_IO_H
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct esp_lcd_panel_io* esp_lcd_panel_io_handle_t;
typedef void* esp_lcd_spi_bus_handle_t;
typedef struct esp_lcd_panel_io_event_data esp_lcd_panel_io_event_data_t;
typedef bool (*esp_lcd_panel_io_color_trans_done_cb_t)(
    esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t* event,
    void* user_ctx);
typedef struct {
  int dc_gpio_num;
  int cs_gpio_num;
  unsigned pclk_hz;
  int lcd_cmd_bits;
  int lcd_param_bits;
  int spi_mode;
  int trans_queue_depth;
  esp_lcd_panel_io_color_trans_done_cb_t on_color_trans_done;
} esp_lcd_panel_io_spi_config_t;
esp_err_t esp_lcd_new_panel_io_spi(
    esp_lcd_spi_bus_handle_t bus,
    const esp_lcd_panel_io_spi_config_t* io_config,
    esp_lcd_panel_io_handle_t* ret_io);
esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t io);
esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io, int lcd_cmd,
                                    const void* param, size_t param_size);
esp_err_t esp_lcd_panel_io_tx_color(esp_lcd_panel_io_handle_t io, int lcd_cmd,
                                    const void* color, size_t color_size);
#endif
