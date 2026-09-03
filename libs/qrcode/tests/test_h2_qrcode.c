#include "h2_qrcode.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TEST_MAX_VERSION 10
#define TEST_BUFFER_LEN H2_QRCODE_BUFFER_LEN_FOR_VERSION(TEST_MAX_VERSION)

static uint8_t s_modules[TEST_BUFFER_LEN];
static uint8_t s_scratch[TEST_BUFFER_LEN];

static void encode_hello(h2_qrcode_t *out_qrcode) {
  assert(h2_qrcode_encode_text("HELLO GIZOS", H2_QRCODE_ECC_MEDIUM,
                               TEST_MAX_VERSION, s_modules, sizeof(s_modules),
                               s_scratch, sizeof(s_scratch),
                               out_qrcode) == H2_PAL_OK);
  /* Version 1 through 10 map to 21 through 57 modules per side. */
  assert(out_qrcode->size >= 21 && out_qrcode->size <= 57);
  assert(out_qrcode->size % 4 == 1);
  assert(out_qrcode->modules == s_modules);
}

static void test_encode_rejects_invalid_arguments(void) {
  h2_qrcode_t qrcode;

  assert(h2_qrcode_encode_text("x", H2_QRCODE_ECC_LOW, TEST_MAX_VERSION,
                               s_modules, sizeof(s_modules), s_scratch,
                               sizeof(s_scratch), NULL) ==
         H2_PAL_ERR_INVALID_ARG);

  memset(&qrcode, 0xAA, sizeof(qrcode));
  assert(h2_qrcode_encode_text(NULL, H2_QRCODE_ECC_LOW, TEST_MAX_VERSION,
                               s_modules, sizeof(s_modules), s_scratch,
                               sizeof(s_scratch),
                               &qrcode) == H2_PAL_ERR_INVALID_ARG);
  assert(qrcode.modules == NULL && qrcode.size == 0);

  assert(h2_qrcode_encode_text("x", H2_QRCODE_ECC_LOW, 0, s_modules,
                               sizeof(s_modules), s_scratch, sizeof(s_scratch),
                               &qrcode) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_qrcode_encode_text("x", H2_QRCODE_ECC_LOW,
                               H2_QRCODE_VERSION_MAX + 1, s_modules,
                               sizeof(s_modules), s_scratch, sizeof(s_scratch),
                               &qrcode) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_qrcode_encode_text("x", (h2_qrcode_ecc_t)7, TEST_MAX_VERSION,
                               s_modules, sizeof(s_modules), s_scratch,
                               sizeof(s_scratch),
                               &qrcode) == H2_PAL_ERR_INVALID_ARG);
}

static void test_encode_reports_short_buffers(void) {
  h2_qrcode_t qrcode;

  assert(h2_qrcode_encode_text("x", H2_QRCODE_ECC_LOW, TEST_MAX_VERSION,
                               s_modules, TEST_BUFFER_LEN - 1u, s_scratch,
                               sizeof(s_scratch),
                               &qrcode) == H2_PAL_ERR_NO_SPACE);
  assert(h2_qrcode_encode_text("x", H2_QRCODE_ECC_LOW, TEST_MAX_VERSION,
                               s_modules, sizeof(s_modules), s_scratch,
                               TEST_BUFFER_LEN - 1u,
                               &qrcode) == H2_PAL_ERR_NO_SPACE);
}

static void test_encode_reports_oversized_payload(void) {
  static char payload[512];
  h2_qrcode_t qrcode;

  memset(payload, 'A', sizeof(payload) - 1u);
  payload[sizeof(payload) - 1u] = '\0';
  /* Version 1 at high ECC holds far less than 511 alphanumeric characters. */
  assert(h2_qrcode_encode_text(payload, H2_QRCODE_ECC_HIGH, 1, s_modules,
                               sizeof(s_modules), s_scratch, sizeof(s_scratch),
                               &qrcode) == H2_PAL_ERR_FULL);
  assert(qrcode.modules == NULL && qrcode.size == 0);
}

static void test_finder_patterns_are_dark(void) {
  h2_qrcode_t qrcode;

  encode_hello(&qrcode);
  /* Every symbol carries a dark 7x7 finder ring in three corners. */
  assert(h2_qrcode_module_is_dark(&qrcode, 0, 0));
  assert(h2_qrcode_module_is_dark(&qrcode, 6, 0));
  assert(h2_qrcode_module_is_dark(&qrcode, 0, 6));
  assert(h2_qrcode_module_is_dark(&qrcode, qrcode.size - 1, 0));
  assert(h2_qrcode_module_is_dark(&qrcode, 0, qrcode.size - 1));
  /* The separator ring around a finder pattern is light. */
  assert(!h2_qrcode_module_is_dark(&qrcode, 7, 7));
  /* Coordinates outside the symbol read as light. */
  assert(!h2_qrcode_module_is_dark(&qrcode, -1, 0));
  assert(!h2_qrcode_module_is_dark(&qrcode, qrcode.size, 0));
  assert(!h2_qrcode_module_is_dark(NULL, 0, 0));
}

static void test_layout_centers_symbol(void) {
  h2_qrcode_t qrcode;
  h2_qrcode_layout_t layout;
  int span_modules = 0;

  encode_hello(&qrcode);
  span_modules = qrcode.size + 2 * H2_QRCODE_QUIET_MODULES_MIN;

  assert(h2_qrcode_layout_center(&qrcode, 368, 448,
                                 H2_QRCODE_QUIET_MODULES_MIN,
                                 &layout) == H2_PAL_OK);
  assert(layout.scale == 368 / span_modules);
  assert(layout.quiet_modules == H2_QRCODE_QUIET_MODULES_MIN);
  assert(layout.origin_x == (368 - span_modules * layout.scale) / 2);
  assert(layout.origin_y == (448 - span_modules * layout.scale) / 2);

  /* The short side selects the scale. */
  assert(h2_qrcode_layout_center(&qrcode, 448, 368,
                                 H2_QRCODE_QUIET_MODULES_MIN,
                                 &layout) == H2_PAL_OK);
  assert(layout.scale == 368 / span_modules);

  memset(&layout, 0xAA, sizeof(layout));
  assert(h2_qrcode_layout_center(&qrcode, span_modules - 1, span_modules - 1,
                                 H2_QRCODE_QUIET_MODULES_MIN,
                                 &layout) == H2_PAL_ERR_NO_SPACE);
  assert(layout.scale == 0);
  assert(h2_qrcode_layout_center(&qrcode, 368, 448,
                                 H2_QRCODE_QUIET_MODULES_MIN - 1,
                                 &layout) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_qrcode_layout_center(&qrcode, 0, 448, H2_QRCODE_QUIET_MODULES_MIN,
                                 &layout) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_qrcode_layout_center(NULL, 368, 448, H2_QRCODE_QUIET_MODULES_MIN,
                                 &layout) == H2_PAL_ERR_INVALID_ARG);
}

static void test_render_band_matches_modules(void) {
  enum { kWidth = 96, kHeight = 96, kBandRows = 8 };
  static uint16_t pixels[kWidth * kBandRows];
  const uint16_t dark = 0x0000u;
  const uint16_t light = 0xFFFFu;
  const uint16_t background = 0xF800u;
  h2_qrcode_t qrcode;
  h2_qrcode_layout_t layout;
  int span_pixels = 0;

  encode_hello(&qrcode);
  assert(h2_qrcode_layout_center(&qrcode, kWidth, kHeight,
                                 H2_QRCODE_QUIET_MODULES_MIN,
                                 &layout) == H2_PAL_OK);
  span_pixels = (qrcode.size + 2 * layout.quiet_modules) * layout.scale;

  for (int band_y = 0; band_y < kHeight; band_y += kBandRows) {
    assert(h2_qrcode_render_rgb565_band(&qrcode, &layout, dark, light,
                                        background, band_y, kWidth, kBandRows,
                                        pixels,
                                        sizeof(pixels) / sizeof(pixels[0])) ==
           H2_PAL_OK);
    for (int row = 0; row < kBandRows; ++row) {
      for (int column = 0; column < kWidth; ++column) {
        const uint16_t actual = pixels[row * kWidth + column];
        const int local_x = column - layout.origin_x;
        const int local_y = (band_y + row) - layout.origin_y;
        uint16_t expected = background;

        if (local_x >= 0 && local_x < span_pixels && local_y >= 0 &&
            local_y < span_pixels) {
          const int module_x =
              (local_x / layout.scale) - layout.quiet_modules;
          const int module_y =
              (local_y / layout.scale) - layout.quiet_modules;
          expected = h2_qrcode_module_is_dark(&qrcode, module_x, module_y)
                         ? dark
                         : light;
        }
        assert(actual == expected);
      }
    }
  }
}

static void test_render_band_rejects_invalid_arguments(void) {
  static uint16_t pixels[16];
  h2_qrcode_t qrcode;
  h2_qrcode_layout_t layout;

  encode_hello(&qrcode);
  assert(h2_qrcode_layout_center(&qrcode, 96, 96, H2_QRCODE_QUIET_MODULES_MIN,
                                 &layout) == H2_PAL_OK);

  assert(h2_qrcode_render_rgb565_band(&qrcode, &layout, 0u, 0u, 0u, 0, 4, 4,
                                      pixels, 15u) == H2_PAL_ERR_NO_SPACE);
  assert(h2_qrcode_render_rgb565_band(&qrcode, &layout, 0u, 0u, 0u, -1, 4, 4,
                                      pixels, 16u) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_qrcode_render_rgb565_band(&qrcode, &layout, 0u, 0u, 0u, 0, 0, 4,
                                      pixels, 16u) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_qrcode_render_rgb565_band(&qrcode, NULL, 0u, 0u, 0u, 0, 4, 4,
                                      pixels, 16u) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_qrcode_render_rgb565_band(&qrcode, &layout, 0u, 0u, 0u, 0, 4, 4,
                                      NULL, 16u) == H2_PAL_ERR_INVALID_ARG);
}

int main(void) {
  test_encode_rejects_invalid_arguments();
  test_encode_reports_short_buffers();
  test_encode_reports_oversized_payload();
  test_finder_patterns_are_dark();
  test_layout_centers_symbol();
  test_render_band_matches_modules();
  test_render_band_rejects_invalid_arguments();
  return 0;
}
