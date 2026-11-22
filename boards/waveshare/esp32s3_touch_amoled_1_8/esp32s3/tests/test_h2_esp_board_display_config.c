#include "h2_esp_board.h"
#include "../src/h2_esp_board_display_config_internal.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>

static void test_landscape_touch_corners(void) {
  const int32_t corners[][4] = {
      {367, 0, 0, 0},
      {367, 447, 447, 0},
      {0, 0, 0, 367},
      {0, 447, 447, 367},
  };
  int32_t width, height;
  h2_esp_board_display_extent(H2_ESP_BOARD_DISPLAY_PORTRAIT, &width, &height);
  assert(width == 368 && height == 448);
  h2_esp_board_display_extent(H2_ESP_BOARD_DISPLAY_LANDSCAPE_CCW, &width,
                              &height);
  assert(width == 448 && height == 368);
  for (size_t i = 0; i < sizeof(corners) / sizeof(corners[0]); ++i) {
    int32_t x = corners[i][0], y = corners[i][1];
    h2_esp_board_display_touch_point(H2_ESP_BOARD_DISPLAY_PORTRAIT, &x, &y);
    assert(x == corners[i][0] && y == corners[i][1]);
    h2_esp_board_display_touch_point(H2_ESP_BOARD_DISPLAY_LANDSCAPE_CCW, &x,
                                     &y);
    assert(x == corners[i][2] && y == corners[i][3]);
  }
}

static void test_landscape_packed_chunk(void) {
  const uint16_t pixels[][6] = {
      {0, 0, 0, 0, 0, 0},
      {0, 0x1122, 0x3344, 0x5566, 0x7788, 0},
      {0, 0x99aa, 0xbbcc, 0xddee, 0x1234, 0},
  };
  const h2_display_rect_t source = {443, 365, 5, 3};
  const h2_display_rect_t chunk = {444, 366, 4, 2};
  uint16_t packed[8] = {0};
  const h2_display_rect_t native = h2_esp_board_display_pack(
      H2_ESP_BOARD_DISPLAY_LANDSCAPE_CCW, &source, &chunk, pixels,
      sizeof(pixels[0]), H2_DISPLAY_PIXEL_RGB565, sizeof(uint16_t), packed);
  assert(native.x == 0 && native.y == 444 && native.width == 2 &&
         native.height == 4);
  const uint16_t expected[] = {0xaa99, 0x2211, 0xccbb, 0x4433,
                               0xeedd, 0x6655, 0x3412, 0x8877};
  for (int32_t y = 0; y < native.height; ++y) {
    for (int32_t x = 0; x < native.width; ++x) {
      const int32_t i = y * native.width + x;
      assert(packed[i] == expected[i]);
      int32_t touch_x = native.x + x, touch_y = native.y + y;
      h2_esp_board_display_touch_point(H2_ESP_BOARD_DISPLAY_LANDSCAPE_CCW,
                                       &touch_x, &touch_y);
      assert(touch_x == chunk.x + y && touch_y == chunk.y + 1 - x);
    }
  }
}

static void test_landscape_color_formats(void) {
  const h2_display_rect_t rect = {0, 0, 2, 2};
  const uint16_t rgb444[] = {0xf00, 0x0f0, 0x00f, 0xfff};
  const uint8_t rgb888[] = {255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255};
  uint16_t from_444[4], from_888[4];
  (void)h2_esp_board_display_pack(H2_ESP_BOARD_DISPLAY_LANDSCAPE_CCW, &rect,
                                  &rect, rgb444, 4u, H2_DISPLAY_PIXEL_RGB444,
                                  2u, from_444);
  (void)h2_esp_board_display_pack(H2_ESP_BOARD_DISPLAY_LANDSCAPE_CCW, &rect,
                                  &rect, rgb888, 6u, H2_DISPLAY_PIXEL_RGB888,
                                  3u, from_888);
  const uint16_t expected[] = {0x1f00, 0x00f8, 0xffff, 0xe007};
  for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
    assert(from_444[i] == expected[i] && from_888[i] == expected[i]);
  }
}

int main(void) {
  h2_esp_board_display_config_t config = {
      .pclk_hz = 0u,
  };
  assert(h2_esp_board_display_config_is_valid(&config));
  assert(h2_esp_board_display_config_is_valid(NULL) == 0);
  assert(h2_esp_board_display_dma_rows(&config, 64u) == 64u);
  assert(h2_esp_board_display_dma_rows(NULL, 64u) == 0u);

  config.orientation = H2_ESP_BOARD_DISPLAY_LANDSCAPE_CCW;
  assert(h2_esp_board_display_config_is_valid(&config));
  config.orientation = (h2_esp_board_display_orientation_t)2;
  assert(h2_esp_board_display_config_is_valid(&config) == 0);
  config.orientation = H2_ESP_BOARD_DISPLAY_PORTRAIT;
  test_landscape_touch_corners();
  test_landscape_packed_chunk();
  test_landscape_color_formats();

  config.pclk_hz = (uint32_t)INT_MAX;
  assert(h2_esp_board_display_config_is_valid(&config));
  config.pclk_hz = (uint32_t)INT_MAX + 1u;
  assert(h2_esp_board_display_config_is_valid(&config) == 0);

  config.pclk_hz = 0u;
  const uint32_t valid_rows[] = {0u, 8u, 16u, 32u, 64u};
  for (unsigned i = 0; i < sizeof(valid_rows) / sizeof(valid_rows[0]); ++i) {
    config.dma_buffer_rows = valid_rows[i];
    assert(h2_esp_board_display_config_is_valid(&config));
    assert(h2_esp_board_display_dma_rows(&config, 64u) ==
           (valid_rows[i] == 0u ? 64u : valid_rows[i]));
  }
  const uint32_t invalid_rows[] = {1u, 7u, 9u, 63u, 65u, UINT32_MAX};
  for (unsigned i = 0; i < sizeof(invalid_rows) / sizeof(invalid_rows[0]);
       ++i) {
    config.dma_buffer_rows = invalid_rows[i];
    assert(h2_esp_board_display_config_is_valid(&config) == 0);
    assert(h2_esp_board_display_dma_rows(&config, 64u) == 0u);
  }

  assert(h2_esp_board_display_config_may_apply(0));
  assert(h2_esp_board_display_config_may_apply(1) == 0);
  config.dma_buffer_rows = 0u;
  assert(h2_esp_board_display_dma_rows(&config, 0u) == 0u);
  assert(h2_esp_board_display_dma_rows(&config, 7u) == 0u);
  assert(h2_esp_board_display_dma_rows(&config, 65u) == 0u);
  puts(
      "AMOLED_DMA_CONFIG_PASS default_rows=64 options=8,16,32,64 invalid_rows=0");
  return 0;
}
