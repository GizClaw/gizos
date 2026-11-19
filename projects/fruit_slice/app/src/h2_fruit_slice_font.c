#include "h2_fruit_slice_font.h"

/* Full notice accompanies the font asset in linked binary metadata. */
static const char k_font_license[] =
    "(c) 2014-2021 Adobe (http://www.adobe.com/), with Reserved Font Name 'Source'.\n"
    "\n"
    "This Font Software is licensed under the SIL Open Font License,\n"
    "Version 1.1.\n"
    "\n"
    "This license is copied below, and is also available with a FAQ at:\n"
    "http://scripts.sil.org/OFL\n"
    "\n"
    "-----------------------------------------------------------\n"
    "SIL OPEN FONT LICENSE Version 1.1 - 26 February 2007\n"
    "-----------------------------------------------------------\n"
    "\n"
    "PREAMBLE\n"
    "The goals of the Open Font License (OFL) are to stimulate worldwide\n"
    "development of collaborative font projects, to support the font\n"
    "creation efforts of academic and linguistic communities, and to\n"
    "provide a free and open framework in which fonts may be shared and\n"
    "improved in partnership with others.\n"
    "\n"
    "The OFL allows the licensed fonts to be used, studied, modified and\n"
    "redistributed freely as long as they are not sold by themselves. The\n"
    "fonts, including any derivative works, can be bundled, embedded,\n"
    "redistributed and/or sold with any software provided that any reserved\n"
    "names are not used by derivative works. The fonts and derivatives,\n"
    "however, cannot be released under any other type of license. The\n"
    "requirement for fonts to remain under this license does not apply to\n"
    "any document created using the fonts or their derivatives.\n"
    "\n"
    "DEFINITIONS\n"
    "\"Font Software\" refers to the set of files released by the Copyright\n"
    "Holder(s) under this license and clearly marked as such. This may\n"
    "include source files, build scripts and documentation.\n"
    "\n"
    "\"Reserved Font Name\" refers to any names specified as such after the\n"
    "copyright statement(s).\n"
    "\n"
    "\"Original Version\" refers to the collection of Font Software\n"
    "components as distributed by the Copyright Holder(s).\n"
    "\n"
    "\"Modified Version\" refers to any derivative made by adding to,\n"
    "deleting, or substituting -- in part or in whole -- any of the\n"
    "components of the Original Version, by changing formats or by porting\n"
    "the Font Software to a new environment.\n"
    "\n"
    "\"Author\" refers to any designer, engineer, programmer, technical\n"
    "writer or other person who contributed to the Font Software.\n"
    "\n"
    "PERMISSION & CONDITIONS\n"
    "Permission is hereby granted, free of charge, to any person obtaining\n"
    "a copy of the Font Software, to use, study, copy, merge, embed,\n"
    "modify, redistribute, and sell modified and unmodified copies of the\n"
    "Font Software, subject to the following conditions:\n"
    "\n"
    "1) Neither the Font Software nor any of its individual components, in\n"
    "Original or Modified Versions, may be sold by itself.\n"
    "\n"
    "2) Original or Modified Versions of the Font Software may be bundled,\n"
    "redistributed and/or sold with any software, provided that each copy\n"
    "contains the above copyright notice and this license. These can be\n"
    "included either as stand-alone text files, human-readable headers or\n"
    "in the appropriate machine-readable metadata fields within text or\n"
    "binary files as long as those fields can be easily viewed by the user.\n"
    "\n"
    "3) No Modified Version of the Font Software may use the Reserved Font\n"
    "Name(s) unless explicit written permission is granted by the\n"
    "corresponding Copyright Holder. This restriction only applies to the\n"
    "primary font name as presented to the users.\n"
    "\n"
    "4) The name(s) of the Copyright Holder(s) or the Author(s) of the Font\n"
    "Software shall not be used to promote, endorse or advertise any\n"
    "Modified Version, except to acknowledge the contribution(s) of the\n"
    "Copyright Holder(s) and the Author(s) or with their explicit written\n"
    "permission.\n"
    "\n"
    "5) The Font Software, modified or unmodified, in part or in whole,\n"
    "must be distributed entirely under this license, and must not be\n"
    "distributed under any other license. The requirement for fonts to\n"
    "remain under this license does not apply to any document created using\n"
    "the Font Software.\n"
    "\n"
    "TERMINATION\n"
    "This license becomes null and void if any of the above conditions are\n"
    "not met.\n"
    "\n"
    "DISCLAIMER\n"
    "THE FONT SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND,\n"
    "EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF\n"
    "MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT\n"
    "OF COPYRIGHT, PATENT, TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL THE\n"
    "COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,\n"
    "INCLUDING ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL\n"
    "DAMAGES, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING\n"
    "FROM, OUT OF THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM\n"
    "OTHER DEALINGS IN THE FONT SOFTWARE.\n";

extern const lv_font_t h2_fruit_slice_font_zh_compact;
extern const lv_font_t h2_fruit_slice_font_zh_body;
extern const lv_font_t h2_fruit_slice_font_zh_heading;
extern const lv_font_t h2_fruit_slice_font_zh_display;

const h2_fruit_slice_font_asset_t h2_fruit_slice_font_asset = {
    .strikes = {{&h2_fruit_slice_font_zh_compact, 13, 13, 11},
                {&h2_fruit_slice_font_zh_body, 16, 17, 14},
                {&h2_fruit_slice_font_zh_heading, 21, 21, 18},
                {&h2_fruit_slice_font_zh_display, 26, 27, 23}},
    .license_notice = k_font_license,
};

const h2_fruit_slice_font_strike_t* h2_fruit_slice_font_strike(int32_t scale) {
  const unsigned count = sizeof(h2_fruit_slice_font_asset.strikes) /
                         sizeof(h2_fruit_slice_font_asset.strikes[0]);
  const unsigned index = scale <= 1 ? 0u : (unsigned)scale - 1u;
  return &h2_fruit_slice_font_asset.strikes[index < count ? index : count - 1u];
}

const lv_font_fmt_txt_glyph_dsc_t* h2_fruit_slice_font_glyph(
    const h2_fruit_slice_font_strike_t* strike, uint32_t cp) {
  const lv_font_fmt_txt_dsc_t* dsc = strike->font->dsc;
  for (unsigned i = 0; i < dsc->cmap_num; ++i) {
    const lv_font_fmt_txt_cmap_t* map = &dsc->cmaps[i];
    if (cp < map->range_start || cp - map->range_start >= map->range_length)
      continue;
    const uint32_t off = cp - map->range_start;
    uint32_t id = 0;
    if (map->type == LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY) {
      id = map->glyph_id_start + off;
    } else if (map->type == LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL) {
      id = map->glyph_id_start + ((const uint8_t*)map->glyph_id_ofs_list)[off];
    } else {
      unsigned low = 0, high = map->list_length;
      while (low < high) {
        const unsigned mid = low + (high - low) / 2u;
        if (map->unicode_list[mid] == off) {
          id = map->glyph_id_start +
               (map->type == LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
                    ? mid
                    : ((const uint16_t*)map->glyph_id_ofs_list)[mid]);
          break;
        }
        if (map->unicode_list[mid] < off)
          low = mid + 1u;
        else
          high = mid;
      }
    }
    if (id != 0u) return &dsc->glyph_dsc[id];
  }
  return NULL;
}
