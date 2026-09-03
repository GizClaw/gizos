#ifndef H2_QRCODE_H
#define H2_QRCODE_H

/**
 * @file h2_qrcode.h
 * @brief QR Code Model 2 encoding and RGB565 rasterization without allocation.
 *
 * The encoder wraps the vendored `qrcodegen` implementation and keeps every
 * buffer caller-provided, so the library never allocates and never depends on
 * a platform. The rasterizer converts an encoded symbol into RGB565 pixel
 * bands, which lets a caller push a symbol to a Display PAL surface one band
 * at a time instead of holding a full-screen frame.
 */

#include "h2/pal/core/h2_pal_errors.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Lowest QR Code Model 2 version accepted by this library. */
#define H2_QRCODE_VERSION_MIN 1
/** Highest QR Code Model 2 version accepted by this library. */
#define H2_QRCODE_VERSION_MAX 40

/**
 * @brief Byte length of a module or scratch buffer able to hold `version`.
 *
 * Both `h2_qrcode_encode_text()` buffers must be at least this long for the
 * requested `max_version`. Requires H2_QRCODE_VERSION_MIN <= version <=
 * H2_QRCODE_VERSION_MAX; the expression is usable as an array bound.
 */
#define H2_QRCODE_BUFFER_LEN_FOR_VERSION(version)                              \
  ((((version) * 4 + 17) * ((version) * 4 + 17) + 7) / 8 + 1)

/** Byte length that holds any encodable symbol. */
#define H2_QRCODE_BUFFER_LEN_MAX                                               \
  H2_QRCODE_BUFFER_LEN_FOR_VERSION(H2_QRCODE_VERSION_MAX)

/** Quiet zone width required by the QR Code specification, in modules. */
#define H2_QRCODE_QUIET_MODULES_MIN 4

/** Error correction level, ordered by ascending protection. */
typedef enum h2_qrcode_ecc {
  H2_QRCODE_ECC_LOW = 0,
  H2_QRCODE_ECC_MEDIUM = 1,
  H2_QRCODE_ECC_QUARTILE = 2,
  H2_QRCODE_ECC_HIGH = 3,
} h2_qrcode_ecc_t;

/**
 * @brief An encoded symbol borrowing the caller's module storage.
 *
 * The struct stays valid only while the `modules` buffer passed to
 * `h2_qrcode_encode_text()` stays alive and unmodified. The library never
 * copies, retains, or frees that buffer.
 */
typedef struct h2_qrcode {
  /** Borrowed encoded module storage owned by the caller. */
  const uint8_t *modules;
  /** Symbol width and height in modules. */
  int size;
} h2_qrcode_t;

/**
 * @brief Placement of a symbol inside a pixel surface.
 *
 * `origin_x` and `origin_y` locate the top-left pixel of the quiet zone, so
 * the drawn square spans `(size + 2 * quiet_modules) * scale` pixels per side.
 */
typedef struct h2_qrcode_layout {
  /** Left pixel of the quiet zone in surface coordinates. */
  int origin_x;
  /** Top pixel of the quiet zone in surface coordinates. */
  int origin_y;
  /** Pixels per module; at least 1. */
  int scale;
  /** Quiet zone width in modules on every side. */
  int quiet_modules;
} h2_qrcode_layout_t;

/**
 * @brief Encode NUL-terminated text into a QR Code symbol.
 *
 * The call is synchronous, allocation free, and safe from any task context.
 * Both buffers are caller-provided storage of at least
 * `H2_QRCODE_BUFFER_LEN_FOR_VERSION(max_version)` bytes and must not overlap;
 * `scratch` holds no useful data afterwards. On success `out_qrcode` borrows
 * `modules`; on failure it is zeroed.
 *
 * @param text Borrowed NUL-terminated payload.
 * @param ecc Minimum error correction level; the encoder may raise it when
 *        that costs no extra version.
 * @param max_version Largest version the encoder may select, in
 *        [H2_QRCODE_VERSION_MIN, H2_QRCODE_VERSION_MAX].
 * @param modules Caller-provided module storage borrowed by `out_qrcode`.
 * @param modules_len Byte length of `modules`.
 * @param scratch Caller-provided scratch storage, released on return.
 * @param scratch_len Byte length of `scratch`.
 * @param out_qrcode Encoded symbol, zeroed on failure.
 * @return H2_PAL_OK on success, H2_PAL_ERR_INVALID_ARG for a rejected
 *         argument, H2_PAL_ERR_NO_SPACE when a buffer is too short, or
 *         H2_PAL_ERR_FULL when the payload does not fit `max_version`.
 */
h2_pal_result_t h2_qrcode_encode_text(const char *text, h2_qrcode_ecc_t ecc,
                                      int max_version, uint8_t *modules,
                                      size_t modules_len, uint8_t *scratch,
                                      size_t scratch_len,
                                      h2_qrcode_t *out_qrcode);

/**
 * @brief Report whether the module at (x, y) is dark.
 *
 * Coordinates outside the symbol read as light, which matches the quiet zone.
 */
bool h2_qrcode_module_is_dark(const h2_qrcode_t *qrcode, int x, int y);

/**
 * @brief Choose the largest integer scale that centers a symbol in a surface.
 *
 * @param qrcode Encoded symbol.
 * @param surface_width Surface width in pixels.
 * @param surface_height Surface height in pixels.
 * @param quiet_modules Quiet zone width per side, at least
 *        H2_QRCODE_QUIET_MODULES_MIN.
 * @param out_layout Centered layout, zeroed on failure.
 * @return H2_PAL_OK on success, H2_PAL_ERR_INVALID_ARG for a rejected
 *         argument, or H2_PAL_ERR_NO_SPACE when the surface cannot hold the
 *         symbol and its quiet zone at scale 1.
 */
h2_pal_result_t h2_qrcode_layout_center(const h2_qrcode_t *qrcode,
                                        int surface_width, int surface_height,
                                        int quiet_modules,
                                        h2_qrcode_layout_t *out_layout);

/**
 * @brief Rasterize one horizontal band of the surface as RGB565 pixels.
 *
 * The band is a full-width slice starting at surface row `band_y`. Pixels
 * inside the symbol take `dark_color` or `light_color`, the quiet zone takes
 * `light_color`, and everything outside the quiet zone takes
 * `background_color`. The caller owns `out_pixels`, which must hold
 * `band_width * band_height` pixels laid out row-major without padding.
 *
 * @return H2_PAL_OK on success, H2_PAL_ERR_INVALID_ARG for a rejected
 *         argument, or H2_PAL_ERR_NO_SPACE when `out_pixels_len` is too small.
 */
h2_pal_result_t h2_qrcode_render_rgb565_band(
    const h2_qrcode_t *qrcode, const h2_qrcode_layout_t *layout,
    uint16_t dark_color, uint16_t light_color, uint16_t background_color,
    int band_y, int band_width, int band_height, uint16_t *out_pixels,
    size_t out_pixels_len);

#ifdef __cplusplus
}
#endif

#endif
