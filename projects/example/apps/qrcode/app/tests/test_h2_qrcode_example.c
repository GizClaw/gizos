#include "h2_qrcode_example.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* AMOLED panel geometry, so the test exercises the shipped display size. */
#define TEST_WIDTH 368
#define TEST_HEIGHT 448

typedef struct fake_display {
  uint16_t *frame;
  int open_count;
  int present_count;
  uint32_t brightness_percent;
  int brightness_calls;
  size_t drawn_pixels;
} fake_display_t;

static size_t s_live_allocations;

static void *test_alloc(void *user, size_t len) {
  (void)user;
  void *ptr = malloc(len);
  if (ptr != NULL) {
    ++s_live_allocations;
  }
  return ptr;
}

static void *test_realloc(void *user, void *ptr, size_t len) {
  (void)user;
  (void)ptr;
  (void)len;
  /* The App must not resize an allocation. */
  assert(0);
  return NULL;
}

static void test_free(void *user, void *ptr) {
  (void)user;
  if (ptr != NULL) {
    assert(s_live_allocations > 0u);
    --s_live_allocations;
  }
  free(ptr);
}

static const h2_pal_mem_vtable_t kMemVtable = {
    test_alloc,
    test_realloc,
    test_free,
};

static int fake_open(void *user) {
  fake_display_t *display = (fake_display_t *)user;
  ++display->open_count;
  return H2_DISPLAY_OK;
}

static int fake_get_info(void *user, h2_display_info_t *out_info) {
  (void)user;
  memset(out_info, 0, sizeof(*out_info));
  out_info->width = TEST_WIDTH;
  out_info->height = TEST_HEIGHT;
  return H2_DISPLAY_OK;
}

static int fake_draw_bitmap(void *user, const h2_display_rect_t *rect,
                            const void *pixels, size_t stride_bytes,
                            h2_display_pixel_format_t format) {
  fake_display_t *display = (fake_display_t *)user;
  const uint8_t *source = (const uint8_t *)pixels;

  assert(format == H2_DISPLAY_PIXEL_RGB565);
  assert(rect != NULL && pixels != NULL);
  assert(rect->x == 0 && rect->width == TEST_WIDTH);
  assert(rect->y >= 0 && rect->height > 0);
  assert(rect->y + rect->height <= TEST_HEIGHT);
  assert(stride_bytes >= (size_t)rect->width * sizeof(uint16_t));

  for (int row = 0; row < rect->height; ++row) {
    memcpy(&display->frame[(size_t)(rect->y + row) * TEST_WIDTH],
           source + (size_t)row * stride_bytes,
           (size_t)TEST_WIDTH * sizeof(uint16_t));
  }
  display->drawn_pixels += (size_t)rect->width * (size_t)rect->height;
  return H2_DISPLAY_OK;
}

static int fake_present(void *user) {
  fake_display_t *display = (fake_display_t *)user;
  ++display->present_count;
  return H2_DISPLAY_OK;
}

static int fake_set_brightness(void *user, uint32_t percent) {
  fake_display_t *display = (fake_display_t *)user;
  display->brightness_percent = percent;
  ++display->brightness_calls;
  return H2_DISPLAY_OK;
}

static int fake_close(void *user) {
  (void)user;
  return H2_DISPLAY_OK;
}

static const h2_pal_display_vtable_t kDisplayVtable = {
    fake_open, fake_get_info,  fake_draw_bitmap,
    fake_present, fake_set_brightness, fake_close,
};

static h2_pal_result_t count_ready(void *user) {
  ++*(int *)user;
  return H2_PAL_OK;
}

static void test_presents_a_centered_symbol(void) {
  static uint16_t frame[(size_t)TEST_WIDTH * TEST_HEIGHT];
  fake_display_t display = {frame, 0, 0, 0u, 0, 0u};
  const h2_pal_display_api_t display_api = {&display, &kDisplayVtable};
  const h2_pal_mem_api_t mem_api = {NULL, &kMemVtable};
  h2_runtime_t runtime;
  int ready_calls = 0;
  size_t dark_pixels = 0u;
  size_t light_pixels = 0u;

  memset(&runtime, 0, sizeof(runtime));
  runtime.display = &display_api;
  runtime.mem = &mem_api;
  memset(frame, 0xAA, sizeof(frame));

  const h2_qrcode_example_config_t config = {
      .text = "https://github.com/GizClaw/gizos",
      .ecc = H2_QRCODE_ECC_MEDIUM,
      .max_version = 0,
      .quiet_modules = 0,
      .brightness_percent = 90u,
      .on_ready_user = &ready_calls,
      .on_ready = count_ready,
  };
  assert(h2_qrcode_example_run(&runtime, &config) == H2_PAL_OK);

  assert(display.open_count == 1);
  assert(display.present_count == 1);
  assert(display.brightness_calls == 1 && display.brightness_percent == 90u);
  assert(ready_calls == 1);
  /* Every pixel of the panel is written exactly once. */
  assert(display.drawn_pixels == (size_t)TEST_WIDTH * TEST_HEIGHT);
  assert(s_live_allocations == 0u);

  for (size_t index = 0u; index < (size_t)TEST_WIDTH * TEST_HEIGHT; ++index) {
    assert(frame[index] == 0x0000u || frame[index] == 0xFFFFu);
    if (frame[index] == 0x0000u) {
      ++dark_pixels;
    } else {
      ++light_pixels;
    }
  }
  /* A real symbol has both colors, and light dominates on a padded panel. */
  assert(dark_pixels > 0u);
  assert(light_pixels > dark_pixels);
  /* Panel corners lie outside the symbol and stay light. */
  assert(frame[0] == 0xFFFFu);
  assert(frame[(size_t)TEST_WIDTH * TEST_HEIGHT - 1u] == 0xFFFFu);
}

static void test_rejects_missing_capabilities(void) {
  const h2_pal_display_api_t display_api = {NULL, &kDisplayVtable};
  const h2_pal_mem_api_t mem_api = {NULL, &kMemVtable};
  h2_runtime_t runtime;
  const h2_qrcode_example_config_t config = {
      .text = "x",
      .ecc = H2_QRCODE_ECC_LOW,
      .max_version = 0,
      .quiet_modules = 0,
      .brightness_percent = 0u,
      .on_ready_user = NULL,
      .on_ready = NULL,
  };

  memset(&runtime, 0, sizeof(runtime));
  assert(h2_qrcode_example_run(NULL, &config) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_qrcode_example_run(&runtime, NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_qrcode_example_run(&runtime, &config) == H2_PAL_ERR_UNAVAILABLE);
  runtime.display = &display_api;
  assert(h2_qrcode_example_run(&runtime, &config) == H2_PAL_ERR_UNAVAILABLE);
  runtime.mem = &mem_api;
  runtime.display = NULL;
  assert(h2_qrcode_example_run(&runtime, &config) == H2_PAL_ERR_UNAVAILABLE);
}

static void test_rejects_invalid_config(void) {
  const h2_pal_display_api_t display_api = {NULL, &kDisplayVtable};
  const h2_pal_mem_api_t mem_api = {NULL, &kMemVtable};
  h2_runtime_t runtime;
  h2_qrcode_example_config_t config = {
      .text = NULL,
      .ecc = H2_QRCODE_ECC_LOW,
      .max_version = 0,
      .quiet_modules = 0,
      .brightness_percent = 0u,
      .on_ready_user = NULL,
      .on_ready = NULL,
  };

  memset(&runtime, 0, sizeof(runtime));
  runtime.display = &display_api;
  runtime.mem = &mem_api;
  assert(h2_qrcode_example_run(&runtime, &config) == H2_PAL_ERR_INVALID_ARG);
  config.text = "x";
  config.max_version = H2_QRCODE_VERSION_MAX + 1;
  assert(h2_qrcode_example_run(&runtime, &config) == H2_PAL_ERR_INVALID_ARG);
  config.max_version = 0;
  config.quiet_modules = H2_QRCODE_QUIET_MODULES_MIN - 1;
  assert(h2_qrcode_example_run(&runtime, &config) == H2_PAL_ERR_INVALID_ARG);
}

int main(void) {
  test_presents_a_centered_symbol();
  test_rejects_missing_capabilities();
  test_rejects_invalid_config();
  return 0;
}
