#ifndef H2_MOSAICO_TOUCH_TEST_DISPLAY_H
#define H2_MOSAICO_TOUCH_TEST_DISPLAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
typedef void* esp_lcd_panel_handle_t;
typedef void* esp_lcd_touch_handle_t;
typedef int bsp_display_config_t;

#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_TIMEOUT 2
#define ESP_ERR_NOT_FINISHED 3
#define BSP_LCD_H_RES 480
#define BSP_LCD_V_RES 480
#define BSP_LCD_ROTATION_DEFAULT 0
#define BSP_DISPLAY_DEFAULT_CONFIG() 0

esp_err_t bsp_display_new(const bsp_display_config_t* config,
                          esp_lcd_panel_handle_t* out_panel);
void* bsp_display_get_panel_io(void);
esp_err_t bsp_display_on(void);
esp_err_t bsp_display_off(void);
esp_err_t bsp_display_brightness_set(int percent);
esp_err_t esp_lcd_panel_io_tx_param(void* panel, int command, const void* data,
                                    size_t size);
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel, int x0,
                                    int y0, int x1, int y1, const void* pixels);
esp_err_t bsp_touch_new(int rotation, esp_lcd_touch_handle_t* out_touch);
esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t touch);
bool esp_lcd_touch_get_coordinates(esp_lcd_touch_handle_t touch, uint16_t* x,
                                   uint16_t* y, uint16_t* strength,
                                   uint8_t* count, uint8_t capacity);

#endif
