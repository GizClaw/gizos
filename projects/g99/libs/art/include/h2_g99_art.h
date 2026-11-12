#ifndef H2_G99_ART_H
#define H2_G99_ART_H

/* Procedural G99 artwork rendered at the real screen resolution: flat
 * single-colour icons on a 24x24 design grid, the library cover, the Loader
 * brand, the selection glow and the flowing background. Pure C; callers own
 * every buffer. ARGB8888 buffers are little-endian B, G, R, A bytes. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_G99_COLOR_PURPLE 0x9B5CFFu
#define H2_G99_COLOR_PURPLE_HI 0xC9A7FFu
#define H2_G99_COLOR_SHADOW 0x4A2493u
#define H2_G99_COLOR_DEEP 0x1C0F38u
#define H2_G99_COLOR_TEXT 0xF1EDF8u
#define H2_G99_COLOR_MUTED 0xA79FBCu
#define H2_G99_COLOR_DIM 0x6E6685u
#define H2_G99_COLOR_OK 0x62E3A0u
#define H2_G99_COLOR_ERROR 0xFF5C7Au
/** Eyes and smile on the brand icon face. */
#define H2_G99_COLOR_GLOW 0xD7C2FFu
/** Cheeks on the brand icon face. */
#define H2_G99_COLOR_BLUSH 0xFF5C7Au
/** Loader status text. */
#define H2_G99_COLOR_STATUS 0xE8E4F0u

typedef enum h2_g99_icon {
  H2_G99_ICON_ME = 0,
  H2_G99_ICON_STORE,
  H2_G99_ICON_LIBRARY,
  H2_G99_ICON_LAB,
  H2_G99_ICON_SETTINGS,
  H2_G99_ICON_WIFI,
  H2_G99_ICON_VOLUME,
  H2_G99_ICON_SUN,
  H2_G99_ICON_MOON,
  H2_G99_ICON_INFO,
  H2_G99_ICON_HAPTICS,
  H2_G99_ICON_PAD,
  H2_G99_ICON_DISK,
  H2_G99_ICON_UPDATE,
  H2_G99_ICON_ACCOUNT,
  H2_G99_ICON_CLOCK,
  H2_G99_ICON_LANGUAGE,
  H2_G99_ICON_RESET,
  H2_G99_ICON_LOCK,
  H2_G99_ICON_CHECK,
  H2_G99_ICON_COMMUNITY,
  H2_G99_ICON_COUNT,
} h2_g99_icon_t;

/** Renders one icon, centred, into a size x size ARGB8888 buffer. */
bool h2_g99_art_icon(h2_g99_icon_t icon, uint32_t rgb, uint8_t* argb,
                     int32_t size);

/** Renders the built-in Orbit app cover with rounded corners. */
bool h2_g99_art_cover(uint8_t* argb, int32_t size);

/** Renders the smiling-face brand icon into a width x height ARGB8888 buffer
 * (square 112-unit body plus a 4-unit drop shadow). */
bool h2_g99_art_brand_icon(uint8_t* argb, int32_t width, int32_t height);

/** Renders the "GizPocket" wordmark with its shadow into an ARGB8888 buffer
 * of (42 * cell + shadow) x (7 * cell + shadow) pixels. */
bool h2_g99_art_wordmark(uint8_t* argb, int32_t cell, int32_t shadow);

/** Wordmark cell lookup; exposed for tests. */
bool h2_g99_art_wordmark_cell(int32_t col, int32_t row);

/** Renders a soft purple glow disc into a size x size ARGB8888 buffer. */
bool h2_g99_art_glow(uint8_t* argb, int32_t size);

typedef struct h2_g99_background {
  int32_t width;
  int32_t height;
  /** Left edge of the info shade; usually panel_x - 24k. */
  int32_t shade_x;
  float k;
} h2_g99_background_t;

/** First row the waves can touch; rows above it never change with time. */
int32_t h2_g99_background_wave_top(const h2_g99_background_t* background);

/**
 * Renders rows [row_begin, row_end) of the background at flow phase
 * phase_ms into an RGB565 buffer with the given row stride in pixels.
 */
bool h2_g99_background_render(const h2_g99_background_t* background,
                              uint32_t phase_ms, uint16_t* rgb565,
                              int32_t stride, int32_t row_begin,
                              int32_t row_end);

#ifdef __cplusplus
}
#endif

#endif
