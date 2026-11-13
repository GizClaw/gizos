#ifndef H2_MOSAICO_DISPLAY_TEST_SDK_H
#define H2_MOSAICO_DISPLAY_TEST_SDK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
enum {
  ESP_OK = 0,
  ESP_ERR_NO_MEM = 1,
  ESP_ERR_TIMEOUT = 2,
  ESP_ERR_INVALID_RESPONSE = 3,
  ESP_ERR_NOT_FINISHED = 0x10c,
  BSP_LCD_H_RES = 480,
  BSP_LCD_V_RES = 480,
  BSP_LCD_ROTATION_DEFAULT = 0,
  MALLOC_CAP_DMA = 1,
  MALLOC_CAP_INTERNAL = 2,
  MALLOC_CAP_SPIRAM = 4,
  MALLOC_CAP_8BIT = 8,
  pdTRUE = 1,
};
typedef struct {
  int unused;
} StaticSemaphore_t;
typedef StaticSemaphore_t* SemaphoreHandle_t;
#define pdMS_TO_TICKS(ms) (ms)
typedef void* esp_lcd_panel_handle_t;
typedef void* esp_lcd_touch_handle_t;
typedef struct {
  int rotation;
} bsp_display_config_t;
#define BSP_DISPLAY_DEFAULT_CONFIG() {0}
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* storage);
int xSemaphoreTake(SemaphoreHandle_t mutex, unsigned timeout);
void xSemaphoreGive(SemaphoreHandle_t mutex);
void* heap_caps_malloc(size_t size, unsigned caps);
void* heap_caps_calloc(size_t count, size_t size, unsigned caps);
void heap_caps_free(void* pointer);
esp_err_t bsp_display_new(const bsp_display_config_t* config,
                          esp_lcd_panel_handle_t* panel);
esp_err_t bsp_display_on(void);
esp_err_t bsp_display_off(void);
esp_err_t bsp_display_brightness_set(int percent);
void* bsp_display_get_panel_io(void);
esp_err_t esp_lcd_panel_io_tx_param(void* io, int command, const void* data,
                                    size_t size);
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel, int x0,
                                    int y0, int x1, int y1, const void* data);
esp_err_t bsp_touch_new(int rotation, esp_lcd_touch_handle_t* touch);
esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t touch);
bool esp_lcd_touch_get_coordinates(esp_lcd_touch_handle_t touch, uint16_t* x,
                                   uint16_t* y, uint16_t* strength,
                                   uint8_t* count, uint8_t max_points);
#endif
