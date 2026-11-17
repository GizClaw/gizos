#include "h2_bk7258_display_buffer.h"
#include <assert.h>
#include <stdlib.h>

enum { WIDTH = 800, HEIGHT = 480, PIXELS = WIDTH * HEIGHT };

static void paint(uint16_t* pixels, const h2_display_rect_t* rect,
                  uint16_t color) {
  for (int32_t y = rect->y; y < rect->y + rect->height; ++y)
    for (int32_t x = rect->x; x < rect->x + rect->width; ++x)
      pixels[(size_t)y * WIDTH + (size_t)x] = color;
}

int main(void) {
  uint16_t* shadow = calloc(PIXELS, sizeof(uint16_t));
  uint16_t* pixels[2] = {malloc(PIXELS * sizeof(uint16_t)),
                         malloc(PIXELS * sizeof(uint16_t))};
  assert(shadow && pixels[0] && pixels[1]);
  h2_atomic_bool_t available[2] = {0};
  h2_bk7258_display_buffer_t buffers[2] = {0};
  for (unsigned i = 0u; i < 2u; ++i) {
    assert(h2_atomic_bool_init(&available[i], false) == H2_ATOMIC_OK);
    memset(pixels[i], 0xa5, PIXELS * sizeof(uint16_t));
    h2_bk7258_display_buffer_init(&buffers[i], pixels[i], &available[i], WIDTH,
                                  HEIGHT);
    assert(h2_bk7258_display_buffer_claim(&buffers[i]));
    assert(h2_bk7258_display_buffer_copy(&buffers[i], shadow, WIDTH) ==
           PIXELS * sizeof(uint16_t));
    assert(memcmp(pixels[i], shadow, PIXELS * sizeof(uint16_t)) == 0);
  }
  /* Both submitted buffers are immutable until the SDK returns one. */
  assert(!h2_bk7258_display_buffer_claim(&buffers[0]));
  assert(!h2_bk7258_display_buffer_claim(&buffers[1]));
  const h2_display_rect_t canvas = {280, 120, 240, 240};
  paint(shadow, &canvas, 0xf800u);
  for (unsigned i = 0u; i < 2u; ++i)
    h2_bk7258_display_buffer_dirty(&buffers[i], &canvas);
  assert(pixels[0][120 * WIDTH + 280] == 0u);
  assert(pixels[1][120 * WIDTH + 280] == 0u);
  h2_bk7258_display_buffer_release(&buffers[0]);
  assert(h2_bk7258_display_buffer_claim(&buffers[0]));
  assert(h2_bk7258_display_buffer_copy(&buffers[0], shadow, WIDTH) ==
         240u * 240u * sizeof(uint16_t));
  assert(memcmp(pixels[0], shadow, PIXELS * sizeof(uint16_t)) == 0);
  assert(pixels[1][120 * WIDTH + 280] == 0u);

  /* A slot that missed several updates must catch up with the complete
   * current image, while the front slot keeps its preceding snapshot.
   * Distant changes copy only their own bands, not their bounding box. */
  const h2_display_rect_t left = {0, 0, 8, 8};
  const h2_display_rect_t right = {792, 472, 8, 8};
  paint(shadow, &left, 0x07e0u);
  paint(shadow, &right, 0x001fu);
  for (unsigned i = 0u; i < 2u; ++i) {
    h2_bk7258_display_buffer_dirty(&buffers[i], &left);
    h2_bk7258_display_buffer_dirty(&buffers[i], &right);
  }
  assert(pixels[0][0] == 0u && pixels[0][PIXELS - 1] == 0u);
  h2_bk7258_display_buffer_release(&buffers[1]);
  assert(h2_bk7258_display_buffer_claim(&buffers[1]));
  assert(h2_bk7258_display_buffer_copy(&buffers[1], shadow, WIDTH) ==
         (240u * 240u + 2u * 8u * 8u) * sizeof(uint16_t));
  assert(memcmp(pixels[1], shadow, PIXELS * sizeof(uint16_t)) == 0);
  h2_bk7258_display_buffer_release(&buffers[0]);
  assert(h2_bk7258_display_buffer_claim(&buffers[0]));
  assert(h2_bk7258_display_buffer_copy(&buffers[0], shadow, WIDTH) ==
         2u * 8u * 8u * sizeof(uint16_t));
  assert(memcmp(pixels[0], shadow, PIXELS * sizeof(uint16_t)) == 0);
  h2_bk7258_display_buffer_release(&buffers[0]);
  assert(h2_bk7258_display_buffer_claim(&buffers[0]));
  assert(h2_bk7258_display_buffer_copy(&buffers[0], shadow, WIDTH) == 0u);

  /* Changes inside one band merge only their own rows and columns. */
  const h2_display_rect_t first = {0, 40, 8, 4};
  const h2_display_rect_t second = {100, 44, 8, 4};
  paint(shadow, &first, 0xffffu);
  paint(shadow, &second, 0xffe0u);
  h2_bk7258_display_buffer_dirty(&buffers[0], &first);
  h2_bk7258_display_buffer_dirty(&buffers[0], &second);
  assert(h2_bk7258_display_buffer_copy(&buffers[0], shadow, WIDTH) ==
         108u * 8u * sizeof(uint16_t));
  assert(memcmp(pixels[0], shadow, PIXELS * sizeof(uint16_t)) == 0);
  for (unsigned i = 0u; i < 2u; ++i) {
    h2_bk7258_display_buffer_release(&buffers[i]);
    h2_atomic_bool_destroy(&available[i]);
    free(pixels[i]);
  }
  free(shadow);

  /* Rows past the last band fold into it and are still copied. */
  enum { TALL_WIDTH = 4, TALL_HEIGHT = 1100, TALL = TALL_WIDTH * TALL_HEIGHT };
  uint16_t* tall_shadow = calloc(TALL, sizeof(uint16_t));
  uint16_t* tall = calloc(TALL, sizeof(uint16_t));
  assert(tall_shadow && tall);
  h2_atomic_bool_t tall_available = {0};
  h2_bk7258_display_buffer_t tall_buffer = {0};
  assert(h2_atomic_bool_init(&tall_available, false) == H2_ATOMIC_OK);
  h2_bk7258_display_buffer_init(&tall_buffer, tall, &tall_available, TALL_WIDTH,
                                TALL_HEIGHT);
  for (size_t i = 0u; i < TALL; ++i) tall_shadow[i] = (uint16_t)(i + 1u);
  assert(h2_bk7258_display_buffer_claim(&tall_buffer));
  assert(h2_bk7258_display_buffer_copy(&tall_buffer, tall_shadow, TALL_WIDTH) ==
         TALL * sizeof(uint16_t));
  assert(memcmp(tall, tall_shadow, TALL * sizeof(uint16_t)) == 0);
  h2_bk7258_display_buffer_release(&tall_buffer);
  h2_atomic_bool_destroy(&tall_available);
  free(tall);
  free(tall_shadow);
  return 0;
}
