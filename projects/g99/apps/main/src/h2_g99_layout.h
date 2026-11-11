#ifndef H2_G99_LAYOUT_H
#define H2_G99_LAYOUT_H

/* Resolution-independent G99 layout. Every position derives from the screen
 * size through k = min(W, H) / 240; nothing here touches LVGL. */

#include "h2_g99.h"
#include "h2_g99_brand.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_G99_BOOT_MS 900u
#define H2_G99_PAGE_TRANSITION_MS 600u
#define H2_G99_ROTATE_MS 220u

typedef enum h2_g99_wheel_kind {
  H2_G99_WHEEL_ICONS = 0,
  H2_G99_WHEEL_THUMBS,
} h2_g99_wheel_kind_t;

typedef struct h2_g99_layout {
  int32_t width;
  int32_t height;
  float k;
  int32_t pad;
  int32_t sel_x;
  int32_t cy;
  int32_t radius;
  int32_t cx;
  int32_t panel_x;
  int32_t panel_y;
  int32_t panel_w;
  int32_t panel_h;
  /** Inner text inset inside the info area. */
  int32_t panel_inset;
  int32_t font_hero;
  int32_t font_title;
  int32_t font_body;
  int32_t font_small;
  int32_t row_h;
  /** Brand on the boot first frame, placed exactly as the Loader places it. */
  h2_g99_brand_layout_t brand;
} h2_g99_layout_t;

/** One drawn wheel slot. */
typedef struct h2_g99_slot {
  float x;
  float y;
  /** Edge length in pixels; zero hides the slot. */
  float size;
  float opacity;
  /** Tangential tilt in degrees, positive clockwise. */
  float tilt_deg;
  /** Selection glow strength in [0, 1]. */
  float glow;
} h2_g99_slot_t;

/** Layer parameters at one page-transition progress. */
typedef struct h2_g99_page_frame {
  float old_wheel_dx;
  float old_wheel_opacity;
  float old_info_opacity;
  float new_wheel_dx;
  float new_wheel_opacity;
  float new_info_opacity;
} h2_g99_page_frame_t;

/** Element parameters at one boot-animation time. */
typedef struct h2_g99_boot_frame {
  float brand_opacity;
  float background_opacity;
  float unfold;
  float info_opacity;
} h2_g99_boot_frame_t;

/** Returns false for a non-positive size. */
bool h2_g99_layout_compute(int32_t width, int32_t height, h2_g99_layout_t* out);

/** Base edge (in k units) and opacity of slot tier 0..2. */
float h2_g99_slot_base_size(h2_g99_wheel_kind_t kind, int tier);
float h2_g99_slot_base_opacity(int tier);

/** Angular step in radians between adjacent slots. The outermost visible
 * slot touches the vertical padding. */
float h2_g99_wheel_step(const h2_g99_layout_t* layout,
                        h2_g99_wheel_kind_t kind);

/** Slot at fractional distance d from the selection, |d| < 3 visible. */
void h2_g99_wheel_slot(const h2_g99_layout_t* layout, h2_g99_wheel_kind_t kind,
                       float d, h2_g99_slot_t* out);

/** Slot during the boot unfold of tier d in [-2, 2] at unfold u in [0, 1]. */
void h2_g99_boot_slot(const h2_g99_layout_t* layout, int d, float unfold,
                      h2_g99_slot_t* out);

/** Page transition layers at progress p in [0, 1] over 600 ms. */
void h2_g99_page_frame(const h2_g99_layout_t* layout, float p,
                       h2_g99_page_frame_t* out);

/** Boot animation at elapsed milliseconds. */
void h2_g99_boot_frame(uint32_t elapsed_ms, h2_g99_boot_frame_t* out);

/** Union of the wheel slots shown at rest for a list of count items: each
 * slot at full size, enlarged to cover its tilt. Empty for count zero. */
void h2_g99_wheel_bounds(const h2_g99_layout_t* layout,
                         h2_g99_wheel_kind_t kind, uint32_t count,
                         h2_g99_rect_t* out);

/** Rows of height row_h fitting in [top, bottom); at least one. */
int32_t h2_g99_layout_rows(const h2_g99_layout_t* layout, int32_t top,
                           int32_t bottom);

/** Visible wheel offsets for a cyclic list of count items. A list shorter
 * than five items shows each item once. Writes the lowest and highest
 * offset relative to the selection. */
void h2_g99_wheel_span(uint32_t count, int32_t* out_lo, int32_t* out_hi);

float h2_g99_clamp01(float value);
float h2_g99_ease_in(float t);
float h2_g99_ease_out(float t);

#ifdef __cplusplus
}
#endif

#endif
