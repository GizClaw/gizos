#ifndef H2_FRUIT_SLICE_RENDER_H
#define H2_FRUIT_SLICE_RENDER_H

/* Software RGB565 renderer for the game model.
 *
 * The picture has three layers: a background image, a static layer of text
 * and panels that changes only when its key changes, and the moving scene.
 * Each layer is drawn through the same code twice: a measure pass records
 * which horizontal bands the primitives cover, then a paint pass draws them.
 * The run loop restores only bands covered in this or the previous frame and
 * pushes only those rows to the Display. */

#include "h2_fruit_slice_game.h"
#include "os/h2_pal_mem.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_FRUIT_SLICE_BAND_ROWS 16
/** One bit per band: supports heights up to 64 * 16 = 1024 rows. */
#define H2_FRUIT_SLICE_MAX_BANDS 64
/** Cached fruit textures; more distinct sizes fall back to flat drawing. */
#define H2_FRUIT_SLICE_SPRITE_CAPACITY 32

/** Cache of per-kind, per-radius fruit textures in a fruit-local frame.
 * Whole-fruit textures hold unlit color, lit per pixel in screen space; half
 * textures hold the cut face. Textures are built on first use. */
typedef struct h2_fruit_slice_sprites h2_fruit_slice_sprites_t;

typedef struct h2_fruit_slice_canvas {
  uint16_t* pixels; /**< Borrowed RGB565 rows; NULL while measuring only. */
  int32_t width, height;
  size_t stride_pixels;
  /** True records coverage without writing pixels. */
  bool measure;
  /** Covered bands, and per covered band its half-open column range. */
  uint64_t bands;
  int16_t band_x0[H2_FRUIT_SLICE_MAX_BANDS];
  int16_t band_x1[H2_FRUIT_SLICE_MAX_BANDS];
  /** Borrowed texture cache; NULL draws fruit as flat color. */
  h2_fruit_slice_sprites_t* sprites;
} h2_fruit_slice_canvas_t;

/** Fields of the game the static layer depends on. All-integer, so two keys
 * compare equal with memcmp after zero initialization. */
typedef struct h2_fruit_slice_static_key {
  uint32_t phase, score, best, lives, new_best;
  /** Menu fruit center row, or -1 without a menu fruit. */
  int32_t menu_y;
} h2_fruit_slice_static_key_t;

/** Create an empty cache. The allocator is borrowed for the cache lifetime.
 * Returns NULL on allocation failure. */
h2_fruit_slice_sprites_t* h2_fruit_slice_sprites_create(
    const h2_pal_mem_api_t* allocator);

/** Build every texture the game's fixed sizes use, so frames do not allocate.
 * Returns false when an allocation failed or the cache is full. */
bool h2_fruit_slice_sprites_prepare(h2_fruit_slice_sprites_t* sprites,
                                    const h2_fruit_slice_game_t* game);

/** Release every texture and the cache; NULL is a no-op. */
void h2_fruit_slice_sprites_destroy(h2_fruit_slice_sprites_t* sprites);

/** Bands spanning rows [0, height). */
uint64_t h2_fruit_slice_all_bands(int32_t height);

/** Fill a background image of the canvas size. */
void h2_fruit_slice_render_background(h2_fruit_slice_canvas_t* canvas);

/** Current static-layer key of the game. */
void h2_fruit_slice_static_key(const h2_fruit_slice_game_t* game,
                               h2_fruit_slice_static_key_t* out);

/** Draw or measure the static layer: title, HUD and game-over panel. */
void h2_fruit_slice_render_static(h2_fruit_slice_canvas_t* canvas,
                                  const h2_fruit_slice_game_t* game);

/** Draw or measure the moving scene above the static layer. */
void h2_fruit_slice_render_scene(h2_fruit_slice_canvas_t* canvas,
                                 const h2_fruit_slice_game_t* game);

/** Pixel width of text drawn at an integer scale. */
int32_t h2_fruit_slice_text_width(const char* text, int32_t scale);

#ifdef __cplusplus
}
#endif

#endif
