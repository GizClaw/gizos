#ifndef H2_GARDEN_GUARD_RENDER_H
#define H2_GARDEN_GUARD_RENDER_H

/* Software RGB565 renderer for the game model.
 *
 * The picture has three layers: a background image (the courtyard in
 * perspective: lawn, patio, house wall, fence, front wall and the toolbar
 * plank), a static layer with independent parts that change only when their
 * key changes, and the moving scene. Each part is drawn through the same code
 * twice: a measure pass records which horizontal bands the primitives cover,
 * then a paint pass draws them. The run loop restores only bands covered in
 * this or the previous frame and pushes only those rows.
 *
 * The moving scene paints plants, zombies and projectiles by lane, far to
 * near. A per-pixel map keeps nearer parked carts over the moving scene. */

#include "h2_garden_guard.h"
#include "h2_garden_guard_game.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_GARDEN_GUARD_BAND_ROWS 16
/** One bit per band: supports heights up to 64 * 16 = 1024 rows. */
#define H2_GARDEN_GUARD_MAX_BANDS 64

#ifdef __EMSCRIPTEN__
#define H2_GARDEN_GUARD_MAX_MENU_LABELS 28u

typedef struct h2_garden_guard_menu_label {
  float x, y, width, height;
  uint32_t rgb565, opacity, centered;
  char text[80];
} h2_garden_guard_menu_label_t;

typedef struct h2_garden_guard_menu_labels {
  unsigned count, foreground;
  float card[4], shade;
  h2_garden_guard_menu_label_t labels[H2_GARDEN_GUARD_MAX_MENU_LABELS];
} h2_garden_guard_menu_labels_t;

void h2_garden_guard_collect_menu_labels(h2_garden_guard_menu_labels_t* labels,
                                         const h2_garden_guard_game_t* game);
void h2_garden_guard_web_present_menu_labels(
    const h2_garden_guard_menu_labels_t* labels);
#endif

/* Optional scene cache for a fixed layout. A plant reuses its RGB565 result
 * only when the exact pose, incoming pixels and static depth still agree. */
#define H2_GARDEN_GUARD_PLANT_CACHE_PIXELS 3072u

typedef struct h2_garden_guard_cached_plant {
  uint64_t bands;
  int16_t band_x0[H2_GARDEN_GUARD_MAX_BANDS],
      band_x1[H2_GARDEN_GUARD_MAX_BANDS];
  uint64_t idle_tick;
  uint32_t depth_revision;
  uint16_t growth, action, impact;
  int16_t x0, x1, y0, y1;
  uint8_t kind, stage;
  bool pixels_valid;
  uint16_t before[H2_GARDEN_GUARD_PLANT_CACHE_PIXELS];
  uint16_t after[H2_GARDEN_GUARD_PLANT_CACHE_PIXELS];
} h2_garden_guard_cached_plant_t;

typedef struct h2_garden_guard_plant_cache {
  uint32_t depth_revision;
  h2_garden_guard_cached_plant_t cells[H2_GARDEN_GUARD_LANES]
                                      [H2_GARDEN_GUARD_COLS];
} h2_garden_guard_plant_cache_t;

typedef struct h2_garden_guard_canvas {
  uint16_t* pixels; /**< Borrowed RGB565 rows; NULL while measuring only. */
  int32_t width, height;
  size_t stride_pixels;
  /** True records coverage without writing pixels. */
  bool measure;
  bool external_cover_labels;
  h2_garden_guard_plant_cache_t* plant_cache;
  const h2_garden_guard_config_t* config;
#ifdef __EMSCRIPTEN__
  bool external_menu_labels;
  h2_garden_guard_menu_labels_t* menu_labels;
#endif
  /** Covered bands, and per covered band its half-open column range. */
  uint64_t bands;
  int16_t band_x0[H2_GARDEN_GUARD_MAX_BANDS];
  int16_t band_x1[H2_GARDEN_GUARD_MAX_BANDS];
  /** Vertical clip rows [clip_y0, clip_y1), managed by the renderer while it
   * draws; zero clip_y1 selects the full height. */
  int32_t clip_y0, clip_y1;
  /** Optional borrowed occlusion map: one byte per pixel with stride_pixels,
   * 0 for open ground and lane + 1 below a static figure. The static field
   * part writes it and the scene reads it; NULL disables occlusion. The
   * owner clears it wherever it restores static pixels. */
  uint8_t* depth;
  /** Occlusion mode and lane, managed by the renderer while it draws. */
  uint8_t depth_mode, depth_layer;
} h2_garden_guard_canvas_t;

/** Independently cached parts of the static layer. */
typedef enum h2_garden_guard_part {
  /** In paint order: sun counter, each card, then the wave and menu. */
  H2_GARDEN_GUARD_PART_SUN = 0,
  H2_GARDEN_GUARD_PART_CARD_FIRST,
  H2_GARDEN_GUARD_PART_STATUS =
      H2_GARDEN_GUARD_PART_CARD_FIRST + H2_GARDEN_GUARD_CARD_COUNT,
  /** Parked carts and the title or result panel. */
  H2_GARDEN_GUARD_PART_FIELD,
  H2_GARDEN_GUARD_PART_COUNT,
} h2_garden_guard_part_t;

/** Fields of the game a static part depends on. All-integer, so two keys
 * compare equal with memcmp after zero initialization. */
typedef struct h2_garden_guard_static_key {
  uint32_t phase;
  uint32_t values[16];
} h2_garden_guard_static_key_t;

/** Bands spanning rows [0, height). */
uint64_t h2_garden_guard_all_bands(int32_t height);

/** Fill a background image of the canvas size for the game's layout. */
void h2_garden_guard_render_background(h2_garden_guard_canvas_t* canvas,
                                       const h2_garden_guard_game_t* game);

/** Current key of one static part. */
void h2_garden_guard_static_key(const h2_garden_guard_game_t* game,
                                h2_garden_guard_part_t part,
                                h2_garden_guard_static_key_t* out);

/** Draw or measure one static part. */
void h2_garden_guard_render_static(h2_garden_guard_canvas_t* canvas,
                                   const h2_garden_guard_game_t* game,
                                   h2_garden_guard_part_t part);

/** Draw or measure the moving scene above the static layer. */
void h2_garden_guard_render_scene(h2_garden_guard_canvas_t* canvas,
                                  const h2_garden_guard_game_t* game);

/** Draw or measure a neutral zombie at a given foot point and lane size. */
void h2_garden_guard_render_zombie_portrait(h2_garden_guard_canvas_t* canvas,
                                            h2_garden_guard_pest_kind_t kind,
                                            float x, float ground_y,
                                            float size);

/** Pixel width of text drawn at an integer scale. */
int32_t h2_garden_guard_text_width(const char* text, int32_t scale);

#ifdef __cplusplus
}
#endif

#endif
