#ifndef H2_G99_BRAND_H
#define H2_G99_BRAND_H

/* The GizPocket brand screen shared by the Loader and Main's boot first
 * frame: a centred smiling-face icon over the pixel "GizPocket" wordmark on
 * black, and the Loader's status line and progress bar below it. Both images
 * place the brand with this one layout so their frames match pixel for
 * pixel. Every value derives from the screen size; nothing here draws. */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The wordmark is 42 x 7 pixel cells. */
#define H2_G99_WORDMARK_COLS 42
#define H2_G99_WORDMARK_ROWS 7

typedef struct h2_g99_brand_layout {
  int32_t width;
  int32_t height;
  /** Wordmark cell, max(1, floor(min(W, H) / 96)), and its shadow offset. */
  int32_t cell;
  int32_t shadow;
  /** Square icon body and the image that also holds its shadow, as rendered
   * by h2_g99_art_brand_icon(). */
  int32_t icon_w;
  int32_t icon_h;
  int32_t icon_image_w;
  int32_t icon_image_h;
  int32_t icon_x;
  /** Top of the icon, which is the top of the brand group. */
  int32_t group_y;
  int32_t gap;
  /** Wordmark position and the image that also holds its shadow, as rendered
   * by h2_g99_art_wordmark(). */
  int32_t wordmark_x;
  int32_t wordmark_y;
  int32_t wordmark_image_w;
  int32_t wordmark_image_h;
  /** Loader status text: left edge, pixel size, baseline and the top of the
   * line box. The brand group always ends above the line. */
  int32_t status_x;
  int32_t status_font;
  int32_t status_baseline;
  int32_t status_top;
  /** Full-width progress bar flush with the bottom edge. */
  int32_t bar_y;
  int32_t bar_h;
} h2_g99_brand_layout_t;

/** Fills `out` for a width x height screen; false when either is too small
 * to hold the brand and the status line. */
bool h2_g99_brand_layout(int32_t width, int32_t height,
                         h2_g99_brand_layout_t* out);

#ifdef __cplusplus
}
#endif

#endif
