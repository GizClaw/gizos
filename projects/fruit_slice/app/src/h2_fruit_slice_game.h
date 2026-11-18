#ifndef H2_FRUIT_SLICE_GAME_H
#define H2_FRUIT_SLICE_GAME_H

/* Deterministic game model. It owns no PAL resources: the run loop feeds it
 * elapsed time and pointer edges, and the renderer reads it. Coordinates are
 * display pixels with the origin at the top-left corner. */

#include "h2_fruit_slice_text.h"
#include "h2_fruit_slice_cards.h"
#include "h2_fruit_slice_sound.h"

#include <stdbool.h>
#include <stddef.h>
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
  /* Stable IDs: append fruit after the original bomb slot. */
  H2_FRUIT_SLICE_BOMB = 5,
  H2_FRUIT_SLICE_STRAWBERRY = 6,
  H2_FRUIT_SLICE_BANANA,
  H2_FRUIT_SLICE_PINEAPPLE,
  H2_FRUIT_SLICE_GRAPES,
  H2_FRUIT_SLICE_KIND_COUNT,
} h2_fruit_slice_kind_t;

#define H2_FRUIT_SLICE_FRUIT_KINDS 9u

/** Dense fruit index -> stable kind; out-of-range returns KIND_COUNT.
 * Bombs are excluded. Repeated bodies of one kind are separate slice events,
 * while a grape bunch is one body/event, including its visual berries. */
h2_fruit_slice_kind_t h2_fruit_slice_kind_at(unsigned fruit_index);
bool h2_fruit_slice_is_fruit(unsigned kind);

typedef enum h2_fruit_slice_phase {
  H2_FRUIT_SLICE_PHASE_TITLE = 0,
  H2_FRUIT_SLICE_PHASE_PLAYING,
  H2_FRUIT_SLICE_PHASE_GAME_OVER,
  H2_FRUIT_SLICE_PHASE_TASKS,
  H2_FRUIT_SLICE_PHASE_SHOP,
  H2_FRUIT_SLICE_PHASE_CARDS,
} h2_fruit_slice_phase_t;

#define H2_FRUIT_SLICE_WEAPONS 3u
#define H2_FRUIT_SLICE_TASKS 3u
#define H2_FRUIT_SLICE_PROGRESS_BYTES 32u
#define H2_FRUIT_SLICE_PROGRESS_LIMIT 999999u

/* Stable weapon IDs and ownership bits are also used by saved profiles. */
typedef struct h2_fruit_slice_weapon {
  const char* name;
  const char* detail;
  uint32_t price;
  float reach, trail_width; /**< At a 480-pixel short side. */
  uint16_t color;
} h2_fruit_slice_weapon_t;

typedef struct h2_fruit_slice_task {
  const char* name;
  uint32_t target, reward;
  bool repeatable;
} h2_fruit_slice_task_t;

typedef struct h2_fruit_slice_progress {
  uint32_t coins, fruit_credit, combo_credit;
  uint32_t score_task; /**< 0: locked, 1: ready, 2: claimed. */
  uint32_t owned, equipped;
} h2_fruit_slice_progress_t;

typedef struct h2_fruit_slice_rect {
  float x0, y0, x1, y1;
} h2_fruit_slice_rect_t;

#define H2_FRUIT_SLICE_BUTTON_TASKS 1u
#define H2_FRUIT_SLICE_BUTTON_SHOP 2u
#define H2_FRUIT_SLICE_BUTTON_BACK 3u
#define H2_FRUIT_SLICE_BUTTON_CARDS 4u
#define H2_FRUIT_SLICE_BUTTON_LANGUAGE 5u
#define H2_FRUIT_SLICE_BUTTON_START 6u
#define H2_FRUIT_SLICE_BUTTON_SFX 7u
#define H2_FRUIT_SLICE_BUTTON_MUSIC 8u
#define H2_FRUIT_SLICE_BUTTON_CLAIM 10u
#define H2_FRUIT_SLICE_BUTTON_WEAPON 20u

const h2_fruit_slice_weapon_t* h2_fruit_slice_weapon(unsigned id);
const h2_fruit_slice_task_t* h2_fruit_slice_task(unsigned id);
uint32_t h2_fruit_slice_task_progress(const h2_fruit_slice_progress_t* progress,
                                      unsigned id);
/* Explicit little-endian wire format; decode leaves progress unchanged on
 * malformed/unknown versions, invalid ownership or out-of-range values. */
void h2_fruit_slice_progress_encode(const h2_fruit_slice_progress_t* progress,
                                    uint8_t out[H2_FRUIT_SLICE_PROGRESS_BYTES]);
bool h2_fruit_slice_progress_decode(h2_fruit_slice_progress_t* progress,
                                    const void* data, size_t size);

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
  /** Fruit-local orientation relative to the cut normal; preserves new
   * silhouettes as the two cut halves separate and rotate. */
  float shape_angle;
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
  h2_fruit_slice_language_t language;
  uint64_t phase_started_ms;

  uint32_t score, best, lives, waves;
  uint32_t sliced_total, missed_total;
  /** One-shot effect request: best rose during this run and needs saving. */
  bool best_changed;
  bool new_best;

  h2_fruit_slice_cards_t cards;
  h2_fruit_slice_progress_t progress;
  bool progress_dirty, progress_urgent;
  uint32_t progress_revision; /**< Invalidates cached wallet/task/shop text. */
  uint32_t save_status;       /**< 0: available, 1: session only, 2: retry, 3:
                                 unreadable. */
  h2_fruit_slice_phase_t return_phase;
  uint32_t pressed_button;

  /* Main-loop effects; queue saturation drops low-priority requests first. */
  uint8_t sounds[H2_FRUIT_SLICE_SOUND_QUEUE];
  uint8_t sound_count, audio_mute;
  bool result_sounded;

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

/** Shared Back bounds, also used to reserve album/detail content space. */
void h2_fruit_slice_game_back_rect(int32_t width, int32_t height,
                                   h2_fruit_slice_rect_t* out);

/** Shared layout for tap targets and rendered buttons. False when hidden. */
bool h2_fruit_slice_game_button_rect(const h2_fruit_slice_game_t* game,
                                     uint32_t button,
                                     h2_fruit_slice_rect_t* out);

/** Enqueue a sound effect; false if invalid or the bounded queue is full.
 * Swing/slice/coin bursts coalesce within one drain; critical effects replace
 * one such low-priority request when full. No PAL/audio resource is touched. */
bool h2_fruit_slice_game_sound(h2_fruit_slice_game_t* game,
                               h2_fruit_slice_sound_t sound);
/** Remove the oldest effect. Called by the App after pointer/step updates. */
bool h2_fruit_slice_game_take_sound(h2_fruit_slice_game_t* game,
                                    h2_fruit_slice_sound_t* sound);

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
