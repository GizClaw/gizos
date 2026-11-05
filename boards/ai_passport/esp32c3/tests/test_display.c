#include "h2_esp_board_private.h"
#include "h2_passport_display_test_sdk.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static int panel_token, io_token;
static unsigned allocations, bitmap_calls, flush_calls;
static int bitmap_error;
static h2_display_rect_t expected_area;
static const uint8_t* expected_source;
static size_t expected_stride;

void* heap_caps_malloc(size_t size, unsigned caps) {
  assert(size == 240u * 4u * sizeof(uint16_t));
  assert(caps == (unsigned)(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  void* result = malloc(size);
  if (result != NULL) ++allocations;
  return result;
}
void heap_caps_free(void* pointer) {
  if (pointer != NULL) {
    assert(allocations != 0u);
    --allocations;
  }
  free(pointer);
}
esp_err_t spi_bus_initialize(int host, const spi_bus_config_t* config,
                             int dma) {
  assert(host == SPI2_HOST && dma == SPI_DMA_CH_AUTO);
  assert(config->mosi_io_num == 9 && config->sclk_io_num == 8);
  assert(config->max_transfer_sz == 1920);
  return ESP_OK;
}
esp_err_t spi_bus_free(int host) {
  assert(host == SPI2_HOST);
  return ESP_OK;
}
esp_err_t esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t bus,
                                   const esp_lcd_panel_io_spi_config_t* config,
                                   esp_lcd_panel_io_handle_t* out) {
  assert(bus == (esp_lcd_spi_bus_handle_t)SPI2_HOST);
  assert(config->cs_gpio_num == 1 && config->dc_gpio_num == 20);
  *out = &io_token;
  return ESP_OK;
}
esp_err_t esp_lcd_new_panel_st7789(esp_lcd_panel_io_handle_t io,
                                   const esp_lcd_panel_dev_config_t* config,
                                   esp_lcd_panel_handle_t* out) {
  assert(io == &io_token && config->bits_per_pixel == 16);
  *out = &panel_token;
  return ESP_OK;
}
esp_err_t esp_lcd_panel_del(esp_lcd_panel_handle_t handle) {
  assert(handle == &panel_token);
  return ESP_OK;
}
esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t handle) {
  assert(handle == &io_token);
  return ESP_OK;
}
esp_err_t esp_lcd_panel_reset(esp_lcd_panel_handle_t handle) {
  assert(handle == &panel_token);
  return ESP_OK;
}
esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t handle) {
  assert(handle == &panel_token);
  return ESP_OK;
}
esp_err_t esp_lcd_panel_invert_color(esp_lcd_panel_handle_t handle,
                                     bool enabled) {
  assert(handle == &panel_token && enabled);
  return ESP_OK;
}
esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t handle,
                                    bool enabled) {
  assert(handle == &panel_token && enabled);
  return ESP_OK;
}
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t handle, int x1,
                                    int y1, int x2, int y2,
                                    const void* pixels) {
  assert(handle == &panel_token && expected_source != NULL);
  const int rows = y2 - y1;
  const int first = y1 - expected_area.y;
  assert(x1 == expected_area.x && x2 == x1 + expected_area.width);
  assert(rows > 0 && rows <= 4 && first >= 0);
  assert(first + rows <= expected_area.height);
  const size_t row_bytes = (size_t)expected_area.width * sizeof(uint16_t);
  for (int row = 0; row < rows; ++row) {
    assert(memcmp((const uint8_t*)pixels + (size_t)row * row_bytes,
                  expected_source + (size_t)(first + row) * expected_stride,
                  row_bytes) == 0);
  }
  ++bitmap_calls;
  return bitmap_error ? ESP_FAIL : ESP_OK;
}
esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t handle,
                                    int command, const void* data,
                                    size_t size) {
  assert(handle == &io_token && command == -1 && data == NULL && size == 0u);
  ++flush_calls;
  return ESP_OK;
}
esp_err_t ledc_timer_config(const ledc_timer_config_t* config) {
  assert(config->freq_hz == 5000);
  return ESP_OK;
}
esp_err_t ledc_channel_config(const ledc_channel_config_t* config) {
  assert(config->gpio_num == 21 && config->duty == 1023u);
  return ESP_OK;
}
esp_err_t ledc_timer_pause(int mode, int timer) {
  assert(mode == LEDC_LOW_SPEED_MODE && timer == LEDC_TIMER_0);
  return ESP_OK;
}
esp_err_t ledc_stop(int mode, int channel, unsigned idle) {
  assert(mode == LEDC_LOW_SPEED_MODE && channel == LEDC_CHANNEL_0 &&
         idle == 0u);
  return ESP_OK;
}
esp_err_t ledc_set_duty(int mode, int channel, uint32_t duty) {
  assert(mode == LEDC_LOW_SPEED_MODE && channel == LEDC_CHANNEL_0 &&
         duty <= 1023u);
  return ESP_OK;
}
esp_err_t ledc_update_duty(int mode, int channel) {
  assert(mode == LEDC_LOW_SPEED_MODE && channel == LEDC_CHANNEL_0);
  return ESP_OK;
}

static void test_overflow_spans(const h2_pal_display_api_t* display) {
  const uint16_t pixels[4] = {0};
  h2_display_rect_t area = {0, 0, 2, 2};
  const size_t strides[] = {SIZE_MAX, SIZE_MAX - 2u};
  for (size_t i = 0; i < sizeof(strides) / sizeof(strides[0]); ++i) {
    assert(h2_pal_display_draw_bitmap(display, &area, pixels, strides[i],
                                      H2_DISPLAY_PIXEL_RGB565) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(bitmap_calls == 0u && flush_calls == 0u);
  }
  area.height = 3;
  assert(h2_pal_display_draw_bitmap(display, &area, pixels, SIZE_MAX / 2u + 1u,
                                    H2_DISPLAY_PIXEL_RGB565) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(bitmap_calls == 0u && flush_calls == 0u);
  assert(h2_pal_display_draw_bitmap(display, &area, pixels, 0u,
                                    H2_DISPLAY_PIXEL_RGB565) ==
         H2_PAL_ERR_INVALID_ARG);
}

static void test_padded_strips(const h2_pal_display_api_t* display) {
  uint16_t pixels[7][5];
  for (unsigned row = 0; row < 7u; ++row)
    for (unsigned col = 0; col < 5u; ++col)
      pixels[row][col] = (uint16_t)(row * 16u + col);
  expected_area = (h2_display_rect_t){237, 313, 3, 7};
  expected_source = (const uint8_t*)pixels;
  expected_stride = sizeof(pixels[0]);
  assert(h2_pal_display_draw_bitmap(display, &expected_area, pixels,
                                    expected_stride,
                                    H2_DISPLAY_PIXEL_RGB565) == H2_PAL_OK);
  assert(bitmap_calls == 2u && flush_calls == 2u);
  bitmap_error = 1;
  assert(h2_pal_display_draw_bitmap(display, &expected_area, pixels,
                                    expected_stride,
                                    H2_DISPLAY_PIXEL_RGB565) == H2_PAL_ERR_IO);
  assert(bitmap_calls == 3u && flush_calls == 2u);
  bitmap_error = 0;
}

static void test_single_row_stride(const h2_pal_display_api_t* display) {
  const uint16_t pixels[2] = {0x1234, 0x5678};
  expected_area = (h2_display_rect_t){0, 0, 2, 1};
  expected_source = (const uint8_t*)pixels;
  expected_stride = SIZE_MAX;
  assert(h2_pal_display_draw_bitmap(display, &expected_area, pixels,
                                    expected_stride,
                                    H2_DISPLAY_PIXEL_RGB565) == H2_PAL_OK);
  assert(bitmap_calls == 4u && flush_calls == 3u);
}

int main(void) {
  const h2_pal_display_api_t* display = h2_passport_display();
  assert(h2_pal_display_open(display) == H2_PAL_OK);
  assert(h2_pal_display_open(display) == H2_PAL_OK && allocations == 1u);
  test_overflow_spans(display);
  test_padded_strips(display);
  test_single_row_stride(display);
  assert(h2_pal_display_close(display) == H2_PAL_OK && allocations == 0u);
  assert(h2_pal_display_close(display) == H2_PAL_OK);
  return 0;
}
