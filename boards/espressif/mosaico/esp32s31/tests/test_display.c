#ifdef NDEBUG
#undef NDEBUG
#endif
#include "h2_mosaico_display.h"
#include "h2_mosaico_display_test_sdk.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static uint16_t glass[480 * 480];
static unsigned transfers, pixels_sent;
static int held, draw_error;
static bool fail_next_completion;
static uint16_t touch_x, touch_y;
static bool touch_down;
static int touch_error;
static const uint16_t* pending;
static int pending_x, pending_y, pending_width;
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* storage) {
  return storage;
}
int xSemaphoreTake(SemaphoreHandle_t mutex, unsigned timeout) {
  (void)timeout;
  assert(mutex && !held);
  held = 1;
  return pdTRUE;
}
void xSemaphoreGive(SemaphoreHandle_t mutex) {
  assert(mutex && held);
  held = 0;
}
void* heap_caps_malloc(size_t size, unsigned caps) {
  assert(caps == (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  return malloc(size);
}
void* heap_caps_calloc(size_t count, size_t size, unsigned caps) {
  assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  return calloc(count, size);
}
void heap_caps_free(void* pointer) {
  assert(!pending);
  free(pointer);
}
esp_err_t bsp_display_new(const bsp_display_config_t* config,
                          esp_lcd_panel_handle_t* panel) {
  assert(config);
  *panel = glass;
  return ESP_OK;
}
esp_err_t bsp_display_on(void) { return ESP_OK; }
esp_err_t bsp_display_off(void) { return ESP_OK; }
esp_err_t bsp_display_brightness_set(int percent) {
  assert(percent >= 0 && percent <= 100);
  return ESP_OK;
}
void* bsp_display_get_panel_io(void) { return glass; }
esp_err_t esp_lcd_panel_io_tx_param(void* io, int command, const void* data,
                                    size_t size) {
  assert(io == glass && command == -1 && !data && !size);
  if (pending && fail_next_completion) {
    fail_next_completion = false;
    return ESP_ERR_TIMEOUT;
  }
  if (pending) {
    for (int y = 0; y < 8; ++y)
      memcpy(glass + (pending_y + y) * 480 + pending_x,
             pending + y * pending_width, (size_t)pending_width * 2u);
    pending = NULL;
  }
  return ESP_OK;
}
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel, int x0,
                                    int y0, int x1, int y1, const void* data) {
  assert(panel == glass && data && !pending);
  assert(x0 >= 0 && x1 <= 480 && x0 < x1 && x0 % 4 == 0 && x1 % 4 == 0);
  assert(y0 >= 0 && y1 <= 480 && y1 - y0 == 8 && y0 % 8 == 0);
  if (draw_error) return draw_error;
  pending = data;
  pending_x = x0;
  pending_y = y0;
  pending_width = x1 - x0;
  ++transfers;
  pixels_sent += (unsigned)(x1 - x0) * 8u;
  return ESP_OK;
}
esp_err_t bsp_touch_new(int rotation, esp_lcd_touch_handle_t* touch) {
  assert(rotation == 0);
  *touch = &touch_x;
  return ESP_OK;
}
esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t touch) {
  assert(touch == &touch_x);
  return touch_error;
}
bool esp_lcd_touch_get_coordinates(esp_lcd_touch_handle_t touch, uint16_t* x,
                                   uint16_t* y, uint16_t* strength,
                                   uint8_t* count, uint8_t max_points) {
  (void)strength;
  assert(touch == &touch_x && max_points == 1);
  *x = touch_x;
  *y = touch_y;
  *count = touch_down ? 1 : 0;
  return touch_down;
}
static void test_partial_updates(void) {
  const h2_pal_display_api_t* display = h2_mosaico_display();
  assert(h2_pal_display_open(display) == 0);
  uint16_t source[4] = {0xf800, 0x07e0, 0x001f, 0xffff};
  h2_display_rect_t rect = {3, 7, 2, 2};
  assert(h2_pal_display_draw_bitmap(display, &rect, source, 4,
                                    H2_DISPLAY_PIXEL_RGB565) == 0);
  /* A second draw in the same aligned bands must retain the first pixels. */
  uint16_t other = 0xffe0;
  rect = (h2_display_rect_t){11, 8, 1, 1};
  assert(h2_pal_display_draw_bitmap(display, &rect, &other, 2,
                                    H2_DISPLAY_PIXEL_RGB565) == 0);
  memset(source, 0, sizeof(source)); /* Caller can immediately reuse pixels. */
  other = 0;
  assert(h2_pal_display_present(display) == 0);
  assert(glass[7 * 480 + 3] == 0x00f8 && glass[7 * 480 + 4] == 0xe007);
  assert(glass[8 * 480 + 3] == 0x1f00 && glass[8 * 480 + 4] == 0xffff);
  assert(glass[8 * 480 + 11] == 0xe0ff);
  assert(glass[8 * 480 + 5] == 0 && glass[7 * 480 + 2] == 0);
  assert(transfers == 2 && pixels_sent == (8 + 12) * 8);
  assert(h2_pal_display_present(display) == 0 && transfers == 2);
  uint16_t corner = 0x1234;
  rect = (h2_display_rect_t){479, 479, 1, 1};
  assert(h2_pal_display_draw_bitmap(display, &rect, &corner, 2,
                                    H2_DISPLAY_PIXEL_RGB565) == 0);
  draw_error = ESP_ERR_TIMEOUT;
  assert(h2_pal_display_present(display) == H2_PAL_ERR_TIMEOUT);
  draw_error = 0;
  assert(h2_pal_display_present(display) == 0);
  assert(glass[480 * 480 - 1] == 0x3412 && glass[480 * 480 - 2] == 0);
  assert(transfers == 3 && pixels_sent == (8 + 12 + 4) * 8);
  /* An uncompleted transfer must be drained before the DMA buffer is reused. */
  rect = (h2_display_rect_t){0, 0, 1, 1};
  assert(h2_pal_display_draw_bitmap(display, &rect, &corner, 2,
                                    H2_DISPLAY_PIXEL_RGB565) == 0);
  fail_next_completion = true;
  assert(h2_pal_display_present(display) == H2_PAL_ERR_TIMEOUT);
  assert(pending);
  assert(h2_pal_display_present(display) == 0);
  assert(glass[0] == 0x3412);
  assert(h2_pal_display_close(display) == 0);
  assert(h2_pal_display_open(display) == 0);
  const unsigned before = transfers;
  assert(h2_pal_display_present(display) == 0 && transfers == before);
  assert(h2_pal_display_close(display) == 0);
}
static void test_touch_reports(void) {
  const h2_pal_touch_api_t* touch = h2_mosaico_touch();
  assert(h2_pal_touch_open(touch) == 0);
  h2_pal_touch_event_t event;
  touch_down = true;
  touch_x = 100;
  touch_y = 200;
  assert(h2_pal_touch_poll_event(touch, &event) == 0);
  assert(event.kind == H2_PAL_TOUCH_EVENT_DOWN && event.x == 100 &&
         event.y == 200);
  /* ACK-only reports have no coordinates and must retain the active stroke. */
  touch_error = ESP_ERR_NOT_FINISHED;
  touch_x = UINT16_MAX;
  touch_y = UINT16_MAX;
  assert(h2_pal_touch_poll_event(touch, &event) == H2_PAL_ERR_WOULD_BLOCK);
  touch_error = ESP_OK;
  touch_x = 110;
  touch_y = 210;
  assert(h2_pal_touch_poll_event(touch, &event) == 0);
  assert(event.kind == H2_PAL_TOUCH_EVENT_MOVE && event.x == 110 &&
         event.y == 210);
  assert(h2_pal_touch_poll_event(touch, &event) == H2_PAL_ERR_WOULD_BLOCK);
  touch_error = ESP_ERR_NOT_FINISHED;
  touch_down = false;
  assert(h2_pal_touch_poll_event(touch, &event) == H2_PAL_ERR_WOULD_BLOCK);
  touch_error = ESP_OK;
  assert(h2_pal_touch_poll_event(touch, &event) == 0);
  assert(event.kind == H2_PAL_TOUCH_EVENT_UP && event.x == 110 &&
         event.y == 210);
  touch_error = ESP_ERR_TIMEOUT;
  assert(h2_pal_touch_poll_event(touch, &event) == H2_PAL_ERR_TIMEOUT);
  touch_error = ESP_ERR_INVALID_RESPONSE;
  assert(h2_pal_touch_poll_event(touch, &event) == H2_PAL_ERR_IO);
  touch_error = ESP_OK;
  touch_down = true;
  touch_x = 480;
  assert(h2_pal_touch_poll_event(touch, &event) == H2_PAL_ERR_IO);
  assert(h2_pal_touch_close(touch) == 0);
}
int main(void) {
  assert(h2_mosaico_display_init() == 0);
  test_partial_updates();
  test_touch_reports();
  assert(h2_mosaico_display_deinit() == 0);
  return 0;
}
