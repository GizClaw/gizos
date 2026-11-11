#include "h2_mosaico_display.h"

#include "bsp/display.h"
#include "bsp/esp_mosaico.h"

#include <assert.h>

static int irq_level = 1;
static esp_err_t read_result = ESP_OK;
static unsigned int read_count;
static uint8_t point_count;
static uint16_t point_x;
static uint16_t point_y;

int gpio_get_level(int gpio) {
  assert(gpio == BSP_LCD_TOUCH_INT);
  return irq_level;
}

esp_err_t bsp_touch_new(int rotation, esp_lcd_touch_handle_t* out_touch) {
  assert(rotation == BSP_LCD_ROTATION_DEFAULT);
  *out_touch = &point_count;
  return ESP_OK;
}

esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t touch) {
  assert(touch == &point_count);
  ++read_count;
  return read_result;
}

bool esp_lcd_touch_get_coordinates(esp_lcd_touch_handle_t touch, uint16_t* x,
                                   uint16_t* y, uint16_t* strength,
                                   uint8_t* count, uint8_t capacity) {
  assert(touch == &point_count && capacity == 1u);
  (void)strength;
  *x = point_x;
  *y = point_y;
  *count = point_count;
  return point_count != 0u;
}

esp_err_t bsp_display_new(const bsp_display_config_t* config,
                          esp_lcd_panel_handle_t* out_panel) {
  (void)config;
  *out_panel = &point_x;
  return ESP_OK;
}
void* bsp_display_get_panel_io(void) { return &point_x; }
esp_err_t bsp_display_on(void) { return ESP_OK; }
esp_err_t bsp_display_off(void) { return ESP_OK; }
esp_err_t bsp_display_brightness_set(int percent) {
  (void)percent;
  return ESP_OK;
}
esp_err_t esp_lcd_panel_io_tx_param(void* panel, int command, const void* data,
                                    size_t size) {
  (void)panel;
  (void)command;
  (void)data;
  (void)size;
  return ESP_OK;
}
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel, int x0,
                                    int y0, int x1, int y1,
                                    const void* pixels) {
  (void)panel;
  (void)x0;
  (void)y0;
  (void)x1;
  (void)y1;
  (void)pixels;
  return ESP_OK;
}

int main(void) {
  assert(h2_mosaico_display_init() == H2_PAL_OK);
  const h2_pal_touch_api_t* touch = h2_mosaico_touch();
  h2_pal_touch_event_t event;
  assert(h2_pal_touch_poll_event(touch, &event) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_pal_touch_open(touch) == H2_PAL_OK);
  assert(h2_pal_touch_poll_event(touch, &event) == H2_PAL_ERR_WOULD_BLOCK);
  assert(read_count == 0u);
  irq_level = 0;
  point_count = 1u;
  point_x = 10u;
  point_y = 20u;
  assert(h2_pal_touch_poll_event(touch, &event) == H2_PAL_OK);
  assert(event.kind == H2_PAL_TOUCH_EVENT_DOWN);
  irq_level = 1;
  point_count = 0u;
  assert(h2_pal_touch_poll_event(touch, &event) == H2_PAL_ERR_WOULD_BLOCK);
  assert(read_count == 1u);
  irq_level = 0;
  read_result = ESP_ERR_NOT_FINISHED;
  assert(h2_pal_touch_poll_event(touch, &event) == H2_PAL_ERR_WOULD_BLOCK);
  read_result = ESP_FAIL;
  assert(h2_pal_touch_poll_event(touch, &event) == H2_PAL_ERR_IO);
  read_result = ESP_OK;
  point_count = 1u;
  point_x = 11u;
  assert(h2_pal_touch_poll_event(touch, &event) == H2_PAL_OK);
  assert(event.kind == H2_PAL_TOUCH_EVENT_MOVE);
  point_x = 480u;
  assert(h2_pal_touch_poll_event(touch, &event) == H2_PAL_ERR_IO);
  point_count = 0u;
  assert(h2_pal_touch_poll_event(touch, &event) == H2_PAL_OK);
  assert(event.kind == H2_PAL_TOUCH_EVENT_UP && event.x == 11 && event.y == 20);
  assert(h2_pal_touch_poll_event(touch, &event) == H2_PAL_ERR_WOULD_BLOCK);
  assert(h2_pal_touch_close(touch) == H2_PAL_OK);
  assert(h2_pal_touch_open(touch) == H2_PAL_OK);
  point_count = 1u;
  point_x = 11u;
  assert(h2_pal_touch_poll_event(touch, &event) == H2_PAL_OK);
  assert(event.kind == H2_PAL_TOUCH_EVENT_DOWN);
  assert(h2_mosaico_display_deinit() == H2_PAL_OK);
  return 0;
}
