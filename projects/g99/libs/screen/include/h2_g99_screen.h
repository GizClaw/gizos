#ifndef H2_G99_SCREEN_H
#define H2_G99_SCREEN_H

/* G99 screen orientation, selected by its device entries. A 90-degree
 * counter-clockwise rotation is G99 product policy, defined separately
 * in h2_g99_screen_config.h, rather than a board default. These
 * wrappers present the rotated (logical) screen to Runtime, so Main, the
 * Loader screen and Lua apps all draw and take touch in G99 orientation:
 * a logical point (x, y) shows at panel point (y, H - 1 - x), where H is the
 * panel height. The board's own display and touch providers are unchanged.
 *
 * The board panel transfers whole rows, and once rotated a logical row band
 * covers every panel row. Drawing therefore lands in a panel-sized frame,
 * and present() sends the panel rows drawn since the previous present in one
 * board draw; every G99 drawer presents after each frame.
 * Pure C; no SDK handles. Only RGB565 bitmaps are accepted. */

#include "h2_pal.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_g99_screen_rotation {
  H2_G99_SCREEN_ROTATION_NONE = 0,
  H2_G99_SCREEN_ROTATION_CCW_90 = 90,
} h2_g99_screen_rotation_t;

typedef struct h2_g99_screen {
  const h2_pal_display_api_t* board_display;
  const h2_pal_touch_api_t* board_touch;
  const h2_pal_mem_api_t* mem;
  h2_g99_screen_rotation_t rotation;
  /* Panel size, read from the board display on open. */
  int32_t panel_width;
  int32_t panel_height;
  /* Panel-oriented RGB565 frame, allocated from `mem` on the first open. */
  uint16_t* frame;
  /* Panel rows [dirty_first, dirty_end) drawn since the last present. */
  int32_t dirty_first;
  int32_t dirty_end;
  h2_pal_display_api_t display;
  h2_pal_touch_api_t touch;
} h2_g99_screen_t;

/**
 * Prepares the rotated display and touch APIs to put in the Runtime config.
 * Either board API may be NULL, which leaves that wrapper NULL as well.
 * `screen`, the board APIs and `mem` are borrowed for the Runtime lifetime;
 * the frame is allocated from `mem` on the first rotated display open and kept
 * until deinit. NONE passes display and touch operations through without a
 * frame. Rotation must be NONE or CCW_90; other values and missing required
 * allocators return INVALID_ARG. The display wrapper is not reentrant; callers
 * serialize it.
 */
h2_pal_result_t h2_g99_screen_init(h2_g99_screen_t* screen,
                                   const h2_pal_display_api_t* board_display,
                                   const h2_pal_touch_api_t* board_touch,
                                   const h2_pal_mem_api_t* mem,
                                   h2_g99_screen_rotation_t rotation);

/** Releases the frame; the rotated display must be closed. */
void h2_g99_screen_deinit(h2_g99_screen_t* screen);

/** Rotated APIs; NULL when the board API was NULL. */
const h2_pal_display_api_t* h2_g99_screen_display(
    const h2_g99_screen_t* screen);
const h2_pal_touch_api_t* h2_g99_screen_touch(const h2_g99_screen_t* screen);

#ifdef __cplusplus
}
#endif

#endif
