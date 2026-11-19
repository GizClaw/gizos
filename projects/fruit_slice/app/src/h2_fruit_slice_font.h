#ifndef H2_FRUIT_SLICE_FONT_H
#define H2_FRUIT_SLICE_FONT_H

#include "h2_fruit_slice_font_format.h"

/* Native uncompressed bitmap strikes, selected by the existing text scale.
 * Cell dimensions include the CJK ink overshoots at each size. */
typedef struct h2_fruit_slice_font_strike {
  const lv_font_t* font;
  int32_t width, height, baseline;
} h2_fruit_slice_font_strike_t;

typedef struct h2_fruit_slice_font_asset {
  h2_fruit_slice_font_strike_t strikes[4];
  const char* license_notice;
} h2_fruit_slice_font_asset_t;

extern const h2_fruit_slice_font_asset_t h2_fruit_slice_font_asset;
const h2_fruit_slice_font_strike_t* h2_fruit_slice_font_strike(int32_t scale);
const lv_font_fmt_txt_glyph_dsc_t* h2_fruit_slice_font_glyph(
    const h2_fruit_slice_font_strike_t* strike, uint32_t codepoint);

#endif
