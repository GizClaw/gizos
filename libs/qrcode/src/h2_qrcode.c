#include "h2_qrcode.h"

#include "qrcodegen.h"

#include <limits.h>
#include <string.h>

/*
 * The public buffer macro repeats the upstream formula so that consumers do
 * not need the vendored header on their include path. Both must stay equal.
 */
_Static_assert(H2_QRCODE_BUFFER_LEN_FOR_VERSION(H2_QRCODE_VERSION_MAX) ==
                   qrcodegen_BUFFER_LEN_FOR_VERSION(qrcodegen_VERSION_MAX),
               "h2_qrcode buffer length diverged from qrcodegen");
_Static_assert(H2_QRCODE_VERSION_MIN == qrcodegen_VERSION_MIN &&
                   H2_QRCODE_VERSION_MAX == qrcodegen_VERSION_MAX,
               "h2_qrcode version range diverged from qrcodegen");

static bool ecc_to_upstream(h2_qrcode_ecc_t ecc, enum qrcodegen_Ecc *out_ecc) {
  switch (ecc) {
  case H2_QRCODE_ECC_LOW:
    *out_ecc = qrcodegen_Ecc_LOW;
    return true;
  case H2_QRCODE_ECC_MEDIUM:
    *out_ecc = qrcodegen_Ecc_MEDIUM;
    return true;
  case H2_QRCODE_ECC_QUARTILE:
    *out_ecc = qrcodegen_Ecc_QUARTILE;
    return true;
  case H2_QRCODE_ECC_HIGH:
    *out_ecc = qrcodegen_Ecc_HIGH;
    return true;
  default:
    return false;
  }
}

h2_pal_result_t h2_qrcode_encode_text(const char *text, h2_qrcode_ecc_t ecc,
                                      int max_version, uint8_t *modules,
                                      size_t modules_len, uint8_t *scratch,
                                      size_t scratch_len,
                                      h2_qrcode_t *out_qrcode) {
  enum qrcodegen_Ecc upstream_ecc = qrcodegen_Ecc_LOW;
  size_t required_len = 0u;

  if (out_qrcode == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_qrcode, 0, sizeof(*out_qrcode));
  if (text == NULL || modules == NULL || scratch == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (max_version < H2_QRCODE_VERSION_MIN ||
      max_version > H2_QRCODE_VERSION_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (!ecc_to_upstream(ecc, &upstream_ecc)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  required_len = (size_t)H2_QRCODE_BUFFER_LEN_FOR_VERSION(max_version);
  if (modules_len < required_len || scratch_len < required_len) {
    return H2_PAL_ERR_NO_SPACE;
  }

  if (!qrcodegen_encodeText(text, scratch, modules, upstream_ecc,
                            qrcodegen_VERSION_MIN, max_version,
                            qrcodegen_Mask_AUTO, true)) {
    return H2_PAL_ERR_FULL;
  }
  out_qrcode->modules = modules;
  out_qrcode->size = qrcodegen_getSize(modules);
  return H2_PAL_OK;
}

bool h2_qrcode_module_is_dark(const h2_qrcode_t *qrcode, int x, int y) {
  if (qrcode == NULL || qrcode->modules == NULL) {
    return false;
  }
  return qrcodegen_getModule(qrcode->modules, x, y);
}

/*
 * The descriptor is public storage, so a caller can hand back a size that no
 * encoder would produce. Every span is derived from it, so validate the
 * specified module counts before any arithmetic.
 */
static bool qrcode_size_is_valid(const h2_qrcode_t *qrcode) {
  return qrcode != NULL && qrcode->modules != NULL &&
         qrcode->size >= H2_QRCODE_SIZE_MIN &&
         qrcode->size <= H2_QRCODE_SIZE_MAX && (qrcode->size - 17) % 4 == 0;
}

h2_pal_result_t h2_qrcode_layout_center(const h2_qrcode_t *qrcode,
                                        int surface_width, int surface_height,
                                        int quiet_modules,
                                        h2_qrcode_layout_t *out_layout) {
  int span_modules = 0;
  int scale = 0;
  int span_pixels = 0;

  if (out_layout == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_layout, 0, sizeof(*out_layout));
  if (!qrcode_size_is_valid(qrcode)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (surface_width <= 0 || surface_height <= 0 ||
      quiet_modules < H2_QRCODE_QUIET_MODULES_MIN ||
      quiet_modules > H2_QRCODE_QUIET_MODULES_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }

  span_modules = qrcode->size + 2 * quiet_modules;
  scale = surface_width / span_modules;
  if (surface_height / span_modules < scale) {
    scale = surface_height / span_modules;
  }
  if (scale < 1) {
    return H2_PAL_ERR_NO_SPACE;
  }

  span_pixels = span_modules * scale;
  out_layout->origin_x = (surface_width - span_pixels) / 2;
  out_layout->origin_y = (surface_height - span_pixels) / 2;
  out_layout->scale = scale;
  out_layout->quiet_modules = quiet_modules;
  return H2_PAL_OK;
}

h2_pal_result_t h2_qrcode_render_rgb565_band(
    const h2_qrcode_t *qrcode, const h2_qrcode_layout_t *layout,
    uint16_t dark_color, uint16_t light_color, uint16_t background_color,
    int band_y, int band_width, int band_height, uint16_t *out_pixels,
    size_t out_pixels_len) {
  int span_modules = 0;
  int span_pixels = 0;

  if (!qrcode_size_is_valid(qrcode) || layout == NULL ||
      out_pixels == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (layout->scale < 1 ||
      layout->quiet_modules < H2_QRCODE_QUIET_MODULES_MIN ||
      layout->quiet_modules > H2_QRCODE_QUIET_MODULES_MAX || band_y < 0 ||
      band_width <= 0 || band_height <= 0) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  /* The bottom band row and the drawn span must stay representable. */
  if (band_height > INT_MAX - band_y) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  span_modules = qrcode->size + 2 * layout->quiet_modules;
  if (layout->scale > INT_MAX / span_modules) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (out_pixels_len < (size_t)band_width * (size_t)band_height) {
    return H2_PAL_ERR_NO_SPACE;
  }

  span_pixels = span_modules * layout->scale;
  for (int row = 0; row < band_height; ++row) {
    uint16_t *pixels = &out_pixels[(size_t)row * (size_t)band_width];
    const int surface_y = band_y + row;
    const int local_y = surface_y - layout->origin_y;
    const bool row_outside = local_y < 0 || local_y >= span_pixels;
    /* Module row is constant across the band row, so resolve it once. */
    const int module_y =
        row_outside ? 0 : (local_y / layout->scale) - layout->quiet_modules;

    for (int column = 0; column < band_width; ++column) {
      int local_x = 0;
      int module_x = 0;

      if (row_outside) {
        pixels[column] = background_color;
        continue;
      }
      local_x = column - layout->origin_x;
      if (local_x < 0 || local_x >= span_pixels) {
        pixels[column] = background_color;
        continue;
      }
      module_x = (local_x / layout->scale) - layout->quiet_modules;
      pixels[column] = h2_qrcode_module_is_dark(qrcode, module_x, module_y)
                           ? dark_color
                           : light_color;
    }
  }
  return H2_PAL_OK;
}
