#ifndef H2_XIANXIA_RPG_RENDER_H
#define H2_XIANXIA_RPG_RENDER_H

/* Software RGB565 renderer for the game model.
 *
 * The picture has three layers: a background (map tiles, battle backdrop or
 * title painting) rebuilt only when its key changes, a static layer of HUD,
 * dialogue and panels rebuilt when its key changes, and the moving scene of
 * characters and effects. Each layer is drawn through the same code twice: a
 * measure pass records which horizontal bands the primitives cover, then a
 * paint pass draws them. The run loop restores only bands covered in this or
 * the previous frame and pushes only those rows to the Display. All art is
 * procedural; text uses fonts generated from Noto Sans SC at build time. */

#include "h2_xianxia_rpg_game.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_XIANXIA_RPG_BAND_ROWS 16
/** One bit per band: supports heights up to 64 * 16 = 1024 rows. */
#define H2_XIANXIA_RPG_MAX_BANDS 64
#define H2_XIANXIA_RPG_KEY_FIELDS 24

typedef struct h2_xianxia_rpg_canvas {
  uint16_t* pixels; /**< Borrowed RGB565 rows; NULL while measuring only. */
  int32_t width, height;
  size_t stride_pixels;
  /** True records coverage without writing pixels. */
  bool measure;
  /** Covered bands, and per covered band its half-open column range. */
  uint64_t bands;
  int16_t band_x0[H2_XIANXIA_RPG_MAX_BANDS];
  int16_t band_x1[H2_XIANXIA_RPG_MAX_BANDS];
  /** Set by the renderer: limits drawing to a half-open rectangle. */
  bool clipped;
  int32_t clip_x0, clip_y0, clip_x1, clip_y1;
} h2_xianxia_rpg_canvas_t;

/** Fields a layer depends on. All-integer, so two keys compare equal with
 * memcmp after zero initialization. */
typedef struct h2_xianxia_rpg_layer_key {
  int32_t fields[H2_XIANXIA_RPG_KEY_FIELDS];
} h2_xianxia_rpg_layer_key_t;

/** Bands spanning rows [0, height). */
uint64_t h2_xianxia_rpg_all_bands(int32_t height);

void h2_xianxia_rpg_background_key(const h2_xianxia_rpg_game_t* game,
                                   h2_xianxia_rpg_layer_key_t* out);
void h2_xianxia_rpg_static_key(const h2_xianxia_rpg_game_t* game,
                               h2_xianxia_rpg_layer_key_t* out);

/** Draw the whole background of the current screen. */
void h2_xianxia_rpg_render_background(h2_xianxia_rpg_canvas_t* canvas,
                                      const h2_xianxia_rpg_game_t* game);
/** Draw or measure the static layer above the background. */
void h2_xianxia_rpg_render_static(h2_xianxia_rpg_canvas_t* canvas,
                                  const h2_xianxia_rpg_game_t* game);
/** Draw or measure the moving scene above the static layer. */
void h2_xianxia_rpg_render_scene(h2_xianxia_rpg_canvas_t* canvas,
                                 const h2_xianxia_rpg_game_t* game);

/** Whether the body (title false) or title font has a glyph. */
bool h2_xianxia_rpg_has_glyph(bool title, uint32_t codepoint);
/** Pixel advance of a UTF-8 string in the body or title font. */
int32_t h2_xianxia_rpg_text_width(bool title, const char* text);

#ifdef __cplusplus
}
#endif

#endif
