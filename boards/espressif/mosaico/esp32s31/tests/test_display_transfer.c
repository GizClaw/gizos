#include "h2_mosaico_display.h"
#include "h2_mosaico_display_capture.h"
#include "bsp/display.h"
#include "esp_heap_caps.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static uint16_t panel[480 * 480], source[480 * 480];
static const uint16_t* pending;
static h2_display_rect_t pending_rect;
static unsigned captured_pixels;
static size_t dma_limit = SIZE_MAX;
static unsigned rejected_allocations;

void* h2_mosaico_test_dma_alloc(size_t size, unsigned int caps) {
  assert(caps == (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  if (size > dma_limit) {
    ++rejected_allocations;
    return NULL;
  }
  return malloc(size);
}
int gpio_get_level(int gpio) {
  (void)gpio;
  return 1;
}
esp_err_t bsp_touch_new(int rotation, esp_lcd_touch_handle_t* out) {
  (void)rotation;
  *out = panel;
  return ESP_OK;
}
esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t touch) {
  (void)touch;
  return ESP_OK;
}
bool esp_lcd_touch_get_coordinates(esp_lcd_touch_handle_t touch, uint16_t* x,
                                   uint16_t* y, uint16_t* strength,
                                   uint8_t* count, uint8_t capacity) {
  (void)touch;
  (void)x;
  (void)y;
  (void)strength;
  (void)capacity;
  *count = 0;
  return false;
}
esp_err_t bsp_display_new(const bsp_display_config_t* config,
                          esp_lcd_panel_handle_t* out) {
  (void)config;
  *out = panel;
  return ESP_OK;
}
void* bsp_display_get_panel_io(void) { return panel; }
esp_err_t bsp_display_on(void) { return ESP_OK; }
esp_err_t bsp_display_off(void) { return ESP_OK; }
esp_err_t bsp_display_brightness_set(int percent) {
  (void)percent;
  return ESP_OK;
}
esp_err_t esp_lcd_panel_io_tx_param(void* io, int command, const void* data,
                                    size_t size) {
  assert(io == panel && command == -1 && data == NULL && size == 0u);
  if (pending != NULL) {
    for (int y = 0; y < pending_rect.height; ++y)
      memcpy(panel + (pending_rect.y + y) * 480 + pending_rect.x,
             pending + y * pending_rect.width,
             (size_t)pending_rect.width * sizeof(uint16_t));
    pending = NULL;
  }
  return ESP_OK;
}
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t handle, int x0,
                                    int y0, int x1, int y1,
                                    const void* pixels) {
  assert(handle == panel && pending == NULL);
  assert(x0 >= 0 && x1 <= 480 && x0 < x1 && x0 % 4 == 0 && x1 % 4 == 0);
  assert(y0 >= 0 && y1 <= 480 && y0 < y1 && y0 % 8 == 0 && y1 % 8 == 0);
  pending = pixels;
  pending_rect = (h2_display_rect_t){x0, y0, x1 - x0, y1 - y0};
  return ESP_OK;
}
static void capture(void* user, const h2_display_rect_t* rect,
                    const uint16_t* pixels) {
  (void)user;
  assert(pending == NULL);
  for (int y = 0; y < rect->height; ++y)
    assert(memcmp(panel + (rect->y + y) * 480 + rect->x,
                  pixels + y * rect->width,
                  (size_t)rect->width * sizeof(uint16_t)) == 0);
  captured_pixels += (unsigned)rect->width * (unsigned)rect->height;
}
static uint16_t wire(uint16_t value) {
  return (uint16_t)((value << 8) | (value >> 8));
}
static void run_frame_checks(void) {
  const h2_pal_display_api_t* display = h2_mosaico_display();
  assert(h2_pal_display_open(display) == H2_PAL_OK);
  for (size_t i = 0; i < 480u * 480u; ++i)
    source[i] = (uint16_t)(i * 977u + 1u);
  const h2_display_rect_t all = {0, 0, 480, 480};
  captured_pixels = 0;
  assert(h2_pal_display_draw_bitmap(display, &all, source, 960,
                                    H2_DISPLAY_PIXEL_RGB565) == H2_PAL_OK);
  assert(h2_pal_display_present(display) == H2_PAL_OK);
  assert(captured_pixels == 480u * 480u);
  for (size_t i = 0; i < 480u * 480u; ++i) assert(panel[i] == wire(source[i]));

  uint16_t patch[11 * 40];
  for (size_t i = 0; i < 11u * 40u; ++i) patch[i] = (uint16_t)(i + 777u);
  const h2_display_rect_t rect = {5, 3, 11, 40};
  captured_pixels = 0;
  assert(h2_pal_display_draw_bitmap(display, &rect, patch, 22,
                                    H2_DISPLAY_PIXEL_RGB565) == H2_PAL_OK);
  assert(captured_pixels == 12u * 48u);
  for (int y = 0; y < 480; ++y)
    for (int x = 0; x < 480; ++x) {
      const uint16_t expected = x >= 5 && x < 16 && y >= 3 && y < 43
                                    ? patch[(y - 3) * 11 + x - 5]
                                    : source[y * 480 + x];
      assert(panel[y * 480 + x] == wire(expected));
    }
  const h2_display_rect_t corner = {479, 479, 1, 1};
  const uint16_t pixel = 0x5aa5;
  captured_pixels = 0;
  assert(h2_pal_display_draw_bitmap(display, &corner, &pixel, 2,
                                    H2_DISPLAY_PIXEL_RGB565) == H2_PAL_OK);
  assert(captured_pixels == 4u * 8u && panel[480 * 480 - 1] == wire(pixel));
  assert(h2_pal_display_close(display) == H2_PAL_OK);
}
int main(void) {
  assert(h2_mosaico_display_init() == H2_PAL_OK);
  h2_mosaico_display_set_transfer_capture(capture, NULL);
  run_frame_checks();
  dma_limit = 480u * 8u * sizeof(uint16_t);
  run_frame_checks();
  assert(rejected_allocations > 0u);
  assert(h2_mosaico_display_deinit() == H2_PAL_OK);
  return 0;
}
