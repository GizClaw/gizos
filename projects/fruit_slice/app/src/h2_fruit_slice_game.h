#ifndef H2_FRUIT_SLICE_GAME_H
#define H2_FRUIT_SLICE_GAME_H

/* Deterministic game model. It owns no PAL resources: the run loop feeds it
 * elapsed time and pointer edges, and the renderer reads it. Coordinates are
 * display pixels with the origin at the top-left corner. */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Host-endian RGB565 from 8-bit channels. */
#define H2_FRUIT_SLICE_RGB(r, g, b)            \
  ((uint16_t)((((unsigned)(r) & 0xF8u) << 8) | \
              (((unsigned)(g) & 0xFCu) << 3) | ((unsigned)(b) >> 3)))

#define H2_FRUIT_SLICE_PI 3.14159265f

#define H2_FRUIT_SLICE_MAX_FRUITS 16
#define H2_FRUIT_SLICE_MAX_HALVES 32
#define H2_FRUIT_SLICE_MAX_PARTICLES 128
#define H2_FRUIT_SLICE_MAX_SPLATS 12
#define H2_FRUIT_SLICE_MAX_POPUPS 6
#define H2_FRUIT_SLICE_MAX_MISSES 6
#define H2_FRUIT_SLICE_TRAIL_MAX 24
#define H2_FRUIT_SLICE_POPUP_TEXT_MAX 24

#define H2_FRUIT_SLICE_LIVES 3u
#define H2_FRUIT_SLICE_TRAIL_LIFE_MS 160u
#define H2_FRUIT_SLICE_COMBO_WINDOW_MS 300u
#define H2_FRUIT_SLICE_COMBO_MIN 3u
#define H2_FRUIT_SLICE_RESTART_DELAY_MS 900u
#define H2_FRUIT_SLICE_FLASH_MS 450u
/** Menu fruits are drawn this much larger than in play. */
#define H2_FRUIT_SLICE_MENU_SCALE 1.6f

typedef enum h2_fruit_slice_kind {
  H2_FRUIT_SLICE_WATERMELON = 0,
  H2_FRUIT_SLICE_ORANGE,
  H2_FRUIT_SLICE_APPLE,
  H2_FRUIT_SLICE_KIWI,
  H2_FRUIT_SLICE_LEMON,
  H2_FRUIT_SLICE_BOMB,
  H2_FRUIT_SLICE_KIND_COUNT,
} h2_fruit_slice_kind_t;

#define H2_FRUIT_SLICE_FRUIT_KINDS ((unsigned)H2_FRUIT_SLICE_BOMB)

typedef enum h2_fruit_slice_phase {
  H2_FRUIT_SLICE_PHASE_TITLE = 0,
  H2_FRUIT_SLICE_PHASE_PLAYING,
  H2_FRUIT_SLICE_PHASE_GAME_OVER,
} h2_fruit_slice_phase_t;

typedef enum h2_fruit_slice_pointer {
  H2_FRUIT_SLICE_POINTER_DOWN = 1,
  H2_FRUIT_SLICE_POINTER_MOVE,
  H2_FRUIT_SLICE_POINTER_UP,
  /** Abandon the stroke without a final point, e.g. after a read failure. */
  H2_FRUIT_SLICE_POINTER_CANCEL,
} h2_fruit_slice_pointer_t;

typedef struct h2_fruit_slice_body {
  float x, y;   /**< Center, pixels. */
  float vx, vy; /**< Velocity, pixels per second. */
  float angle;  /**< Orientation, radians. */
  float spin;   /**< Angular velocity, radians per second. */
} h2_fruit_slice_body_t;

typedef struct h2_fruit_slice_fruit {
  bool active;
  /** Menu fruits hover in place and select the next phase when sliced. */
  bool menu;
  uint8_t kind;
  float radius;
  h2_fruit_slice_body_t body;
} h2_fruit_slice_fruit_t;

typedef struct h2_fruit_slice_half {
  bool active;
  uint8_t kind;
  float radius;
  /** body.angle is the outward normal of the cut face's opposite side: the
   * half covers points p with dot(p - center, normal) >= 0. */
  h2_fruit_slice_body_t body;
} h2_fruit_slice_half_t;

typedef struct h2_fruit_slice_particle {
  bool active;
  float x, y, vx, vy, size;
  uint16_t color; /**< RGB565. */
  uint16_t age_ms, life_ms;
} h2_fruit_slice_particle_t;

typedef struct h2_fruit_slice_splat {
  bool active;
  float x, y, radius;
  uint16_t color;
  uint16_t age_ms, life_ms;
} h2_fruit_slice_splat_t;

typedef struct h2_fruit_slice_popup {
  bool active;
  float x, y;
  uint16_t color;
  uint16_t age_ms, life_ms;
  char text[H2_FRUIT_SLICE_POPUP_TEXT_MAX];
} h2_fruit_slice_popup_t;

/** A fruit that fell unsliced, shown as a red cross at the bottom edge. */
typedef struct h2_fruit_slice_miss {
  bool active;
  float x;
  uint16_t age_ms;
} h2_fruit_slice_miss_t;

typedef struct h2_fruit_slice_trail_point {
  float x, y;
  uint64_t time_ms;
} h2_fruit_slice_trail_point_t;

typedef struct h2_fruit_slice_game {
  int32_t width, height;
  float unit;    /**< Scale relative to a 480-pixel short side. */
  float gravity; /**< Pixels per second squared. */
  uint32_t rng;
  uint64_t time_ms;
  h2_fruit_slice_phase_t phase;
  uint64_t phase_started_ms;

  uint32_t score, best, lives, waves;
  uint32_t sliced_total, missed_total;
  /** One-shot effect request: best rose during this run and needs saving. */
  bool best_changed;
  bool new_best;

  uint64_t next_wave_ms, next_spawn_ms;
  uint32_t pending_spawns;
  bool wave_has_bomb;

  bool pointer_down, has_last;
  float last_x, last_y;
  uint32_t stroke_slices;
  uint64_t stroke_last_slice_ms;
  float stroke_last_x, stroke_last_y;

  uint32_t flash_ms;      /**< Remaining localized bomb flash time. */
  float flash_x, flash_y; /**< Explosion center, in display pixels. */

  h2_fruit_slice_fruit_t fruits[H2_FRUIT_SLICE_MAX_FRUITS];
  h2_fruit_slice_half_t halves[H2_FRUIT_SLICE_MAX_HALVES];
  h2_fruit_slice_particle_t particles[H2_FRUIT_SLICE_MAX_PARTICLES];
  h2_fruit_slice_splat_t splats[H2_FRUIT_SLICE_MAX_SPLATS];
  h2_fruit_slice_popup_t popups[H2_FRUIT_SLICE_MAX_POPUPS];
  h2_fruit_slice_miss_t misses[H2_FRUIT_SLICE_MAX_MISSES];
  h2_fruit_slice_trail_point_t trail[H2_FRUIT_SLICE_TRAIL_MAX];
  uint32_t trail_count;
} h2_fruit_slice_game_t;

/** Reset to the title screen. width/height must be positive. */
void h2_fruit_slice_game_init(h2_fruit_slice_game_t* game, int32_t width,
                              int32_t height, uint32_t seed, uint32_t best);

/** Feed one pointer edge at the current game time; may slice objects. */
void h2_fruit_slice_game_pointer(h2_fruit_slice_game_t* game,
                                 h2_fruit_slice_pointer_t kind, float x,
                                 float y);

/** Advance the simulation; large steps are split internally. */
void h2_fruit_slice_game_step(h2_fruit_slice_game_t* game, uint32_t dt_ms);

/** Take the one-shot best-score save request; false when none is pending. */
bool h2_fruit_slice_game_take_best(h2_fruit_slice_game_t* game,
                                   uint32_t* out_best);

/** Radius of a kind in pixels for this game's scale. */
float h2_fruit_slice_game_radius(const h2_fruit_slice_game_t* game,
                                 h2_fruit_slice_kind_t kind);

/** Juice color of a kind, RGB565. */
uint16_t h2_fruit_slice_game_juice(h2_fruit_slice_kind_t kind);

#ifdef __cplusplus
}
#endif

#endif
