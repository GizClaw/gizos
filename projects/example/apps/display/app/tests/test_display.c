#include "h2_smoke_display.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct test_display {
  unsigned open_calls;
  unsigned info_calls;
  unsigned brightness_calls;
  unsigned draw_calls;
  unsigned present_calls;
  int next_y;
} test_display_t;

static int fake_open(void *user) {
  test_display_t *display = (test_display_t *)user;
  ++display->open_calls;
  return H2_DISPLAY_OK;
}

static int fake_get_info(void *user, h2_display_info_t *info) {
  test_display_t *display = (test_display_t *)user;
  ++display->info_calls;
  info->width = 8;
  info->height = 17;
  info->native_format = H2_DISPLAY_PIXEL_RGB565;
  return H2_DISPLAY_OK;
}

static int fake_draw_bitmap(void *user, const h2_display_rect_t *rect,
                            const void *pixels, size_t stride_bytes,
                            h2_display_pixel_format_t format) {
  static const uint16_t expected_colors[] = {
      0xffffu, 0xffe0u, 0x07ffu, 0x07e0u, 0xf81fu, 0xf800u, 0x001fu, 0x0000u,
  };
  test_display_t *display = (test_display_t *)user;
  const uint16_t *row = (const uint16_t *)pixels;
  assert(rect->x == 0);
  assert(rect->y == display->next_y);
  assert(rect->width == 8);
  assert(rect->height == (display->next_y == 0 ? 16 : 1));
  assert(stride_bytes == 8u * sizeof(uint16_t));
  assert(format == H2_DISPLAY_PIXEL_RGB565);
  for (size_t index = 0u; index < 8u; ++index) {
    assert(row[index] == expected_colors[index]);
  }
  display->next_y += rect->height;
  ++display->draw_calls;
  return H2_DISPLAY_OK;
}

static int fake_present(void *user) {
  test_display_t *display = (test_display_t *)user;
  ++display->present_calls;
  return H2_DISPLAY_OK;
}

static int fake_set_brightness(void *user, uint32_t percent) {
  test_display_t *display = (test_display_t *)user;
  assert(percent == 90u);
  ++display->brightness_calls;
  return H2_DISPLAY_OK;
}

static int fake_close(void *user) {
  (void)user;
  return H2_DISPLAY_OK;
}

int main(void) {
  assert(h2_smoke_display_run(NULL) == H2_DISPLAY_ERR_INVALID_ARG);
  h2_runtime_t invalid_runtime;
  memset(&invalid_runtime, 0, sizeof(invalid_runtime));
  assert(h2_smoke_display_run(&invalid_runtime) == H2_DISPLAY_ERR_INVALID_ARG);

  test_display_t state;
  memset(&state, 0, sizeof(state));
  static const h2_pal_display_vtable_t display_vtable = {
      .open = fake_open,
      .get_info = fake_get_info,
      .draw_bitmap = fake_draw_bitmap,
      .present = fake_present,
      .set_brightness_percent = fake_set_brightness,
      .close = fake_close,
  };
  h2_pal_display_t display = {
      .user = &state,
      .vtable = &display_vtable,
  };
  h2_runtime_t runtime;
  memset(&runtime, 0, sizeof(runtime));
  runtime.display = &display;

  assert(h2_smoke_display_run(&runtime) == H2_DISPLAY_OK);
  assert(state.open_calls == 1u);
  assert(state.info_calls == 1u);
  assert(state.brightness_calls == 1u);
  assert(state.draw_calls == 2u);
  assert(state.next_y == 17);
  assert(state.present_calls == 1u);
  return 0;
}
