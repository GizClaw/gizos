#include "h2_esp_board_config.h"
#include "h2_esp_board_display_buffer.h"

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint16_t s_shadow[H2_WS175C_LCD_WIDTH * H2_WS175C_LCD_HEIGHT];

static uint16_t at(int x, int y) {
  return s_shadow[(size_t)y * H2_WS175C_LCD_WIDTH + (size_t)x];
}

static void test_odd_pixel_preserves_neighbors(void) {
  for (size_t i = 0; i < sizeof(s_shadow) / sizeof(s_shadow[0]); ++i) {
    s_shadow[i] = 0x3412u;
  }
  const uint16_t red = 0xf800u;
  const h2_display_rect_t rect = {.x = 3, .y = 5, .width = 1, .height = 1};
  h2_display_rect_t transfer = {0};
  assert(h2_ws175c_display_buffer_update(s_shadow, &rect, &red, 2u,
                                         H2_DISPLAY_PIXEL_RGB565,
                                         &transfer) == H2_DISPLAY_OK);
  assert(transfer.x == 2 && transfer.y == 4 && transfer.width == 2 &&
         transfer.height == 2);
  assert(at(3, 5) == 0x00f8u);
  assert(at(2, 4) == 0x3412u && at(3, 4) == 0x3412u && at(2, 5) == 0x3412u);
}

static void test_clipped_rgb888_with_padding(void) {
  const uint8_t pixels[3][12] = {
      {255, 0, 0, 255, 0, 0, 255, 0, 0, 0, 0, 0},
      {255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0},
      {255, 0, 0, 255, 255, 255, 0, 0, 0, 0, 0, 0},
  };
  memset(s_shadow, 0, sizeof(s_shadow));
  const h2_display_rect_t rect = {.x = -1, .y = -1, .width = 3, .height = 3};
  h2_display_rect_t transfer = {0};
  assert(h2_ws175c_display_buffer_update(s_shadow, &rect, pixels, 12u,
                                         H2_DISPLAY_PIXEL_RGB888,
                                         &transfer) == H2_DISPLAY_OK);
  assert(transfer.x == 0 && transfer.y == 0 && transfer.width == 2 &&
         transfer.height == 2);
  assert(at(0, 0) == 0xe007u && at(1, 0) == 0x1f00u);
  assert(at(0, 1) == 0xffffu && at(1, 1) == 0u);
}

static void test_corner_rgb444(void) {
  const uint16_t white = 0x0fffu;
  const h2_display_rect_t rect = {.x = 465, .y = 465, .width = 1, .height = 1};
  h2_display_rect_t transfer = {0};
  assert(h2_ws175c_display_buffer_update(s_shadow, &rect, &white, 2u,
                                         H2_DISPLAY_PIXEL_RGB444,
                                         &transfer) == H2_DISPLAY_OK);
  assert(transfer.x == 464 && transfer.y == 464 && transfer.width == 2 &&
         transfer.height == 2);
  assert(at(465, 465) == 0xffffu);
}

static void test_rejects_bad_input_before_writing(void) {
  const uint16_t pixel = 0u;
  h2_display_rect_t rect = {.width = 2, .height = 2};
  h2_display_rect_t transfer = {0};
  s_shadow[0] = 0xaaaau;
  assert(h2_ws175c_display_buffer_update(s_shadow, NULL, &pixel, 4u,
                                         H2_DISPLAY_PIXEL_RGB565, &transfer) ==
         H2_DISPLAY_ERR_INVALID_ARG);
  assert(h2_ws175c_display_buffer_update(s_shadow, &rect, &pixel, 2u,
                                         H2_DISPLAY_PIXEL_RGB565, &transfer) ==
         H2_DISPLAY_ERR_INVALID_ARG);
  assert(h2_ws175c_display_buffer_update(s_shadow, &rect, &pixel, SIZE_MAX,
                                         H2_DISPLAY_PIXEL_RGB565, &transfer) ==
         H2_DISPLAY_ERR_INVALID_ARG);
  assert(h2_ws175c_display_buffer_update(
             s_shadow, &rect, &pixel, 4u, (h2_display_pixel_format_t)999,
             &transfer) == H2_DISPLAY_ERR_UNSUPPORTED);
  rect.x = INT32_MAX;
  assert(h2_ws175c_display_buffer_update(s_shadow, &rect, &pixel, 4u,
                                         H2_DISPLAY_PIXEL_RGB565, &transfer) ==
         H2_DISPLAY_ERR_INVALID_ARG);
  assert(s_shadow[0] == 0xaaaau);
}

int main(void) {
  test_odd_pixel_preserves_neighbors();
  test_clipped_rgb888_with_padding();
  test_corner_rgb444();
  test_rejects_bad_input_before_writing();
  puts("Waveshare 1.75C display buffer tests passed");
  return 0;
}
