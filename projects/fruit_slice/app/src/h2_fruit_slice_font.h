#ifndef H2_FRUIT_SLICE_FONT_H
#define H2_FRUIT_SLICE_FONT_H

#include "h2_fruit_slice_font_format.h"

/* One generated, uncompressed 14px/2bpp subset; no runtime font engine. */
extern const lv_font_t h2_fruit_slice_font_zh;
typedef struct h2_fruit_slice_font_asset {
  const lv_font_t* font;
  const char*
      license_notice; /**< OFL attribution travels with binary font data. */
} h2_fruit_slice_font_asset_t;
extern const h2_fruit_slice_font_asset_t h2_fruit_slice_font_asset;
const lv_font_fmt_txt_glyph_dsc_t* h2_fruit_slice_font_glyph(
    uint32_t codepoint);

#endif
