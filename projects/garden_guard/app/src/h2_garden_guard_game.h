#ifndef H2_GARDEN_GUARD_GAME_H
#define H2_GARDEN_GUARD_GAME_H

/* Deterministic game model. It owns no PAL resources: the run loop feeds it
 * elapsed time and pointer edges, and the renderer reads it.
 *
 * The field has LANES lanes of COLS cells. Positions along a lane are in
 * cells: 0 is the house edge and COLS the far edge where pests enter. Plant
 * column c covers [c, c + 1). Lane 0 is the farthest from the viewer.
 *
 * The lawn is a ground plane seen from its front edge by a camera looking
 * down at it. Ground coordinates are u across the lawn (0 at the house edge
 * on the left, 1 at the entry edge on the right) and f in depth (0 at the
 * near edge, 1 at the far edge). The layout projects them with a pinhole
 * perspective: far rows are narrower and shorter on screen, vertical lines
 * stay vertical, and upright characters scale with depth. Every display shape
 * uses the same projection; portrait displays only get a taller courtyard.
 * Suns, particles and the toolbar use display pixels directly. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Host-endian RGB565 from 8-bit channels. */
#define H2_GARDEN_GUARD_RGB(r, g, b)           \
  ((uint16_t)((((unsigned)(r) & 0xF8u) << 8) | \
              (((unsigned)(g) & 0xFCu) << 3) | ((unsigned)(b) >> 3)))

#define H2_GARDEN_GUARD_PI 3.14159265f

#define H2_GARDEN_GUARD_LANES 5
#define H2_GARDEN_GUARD_COLS 9
#define H2_GARDEN_GUARD_WAVES 5

#define H2_GARDEN_GUARD_MAX_PESTS 32
#define H2_GARDEN_GUARD_MAX_PEAS 48
#define H2_GARDEN_GUARD_MAX_SUNS 12
#define H2_GARDEN_GUARD_MAX_PARTICLES 64
#define H2_GARDEN_GUARD_BANNER_TEXT_MAX 16

#define H2_GARDEN_GUARD_START_SUN 150u
#define H2_GARDEN_GUARD_SUN_VALUE 25u
#define H2_GARDEN_GUARD_SUN_LIMIT 9990u
/** A resting sun disappears after this long unless it is collected. */
#define H2_GARDEN_GUARD_SUN_LIFE_MS 9000u
/** Taps on the result screen are ignored this long after it appears. */
#define H2_GARDEN_GUARD_RESTART_DELAY_MS 1000u
/** Ready time before the first wave. */
#define H2_GARDEN_GUARD_FIRST_WAVE_MS 20000u
#define H2_GARDEN_GUARD_BANNER_MS 2200u
#define H2_GARDEN_GUARD_WIN_BONUS 250u
/** Height of a flying pea above the ground, in lane sizes. */
#define H2_GARDEN_GUARD_PEA_HEIGHT 0.6f
#define H2_GARDEN_GUARD_PLANT_IDLE_MS 120u
#define H2_GARDEN_GUARD_PLANT_GROW_MS 280u
#define H2_GARDEN_GUARD_SHOT_POSE_MS 200u
#define H2_GARDEN_GUARD_BLOOM_POSE_MS 400u
#define H2_GARDEN_GUARD_BITE_IMPACT_MS 180u
#define H2_GARDEN_GUARD_PEST_HIT_MS 160u
#define H2_GARDEN_GUARD_PEST_EXIT_MS 400u
#define H2_GARDEN_GUARD_PEST_BITE_MS 720u

typedef enum h2_garden_guard_phase {
  H2_GARDEN_GUARD_PHASE_TITLE = 0,
  H2_GARDEN_GUARD_PHASE_PLAYING,
  H2_GARDEN_GUARD_PHASE_WON,
  H2_GARDEN_GUARD_PHASE_LOST,
} h2_garden_guard_phase_t;

typedef enum h2_garden_guard_plant_kind {
  H2_GARDEN_GUARD_PLANT_NONE = 0,
  /** Produces a sun periodically. */
  H2_GARDEN_GUARD_PLANT_SUNFLOWER,
  /** Fires peas along its lane while a pest is ahead of it. */
  H2_GARDEN_GUARD_PLANT_SHOOTER,
  /** A tough pumpkin that holds pests back while they chew. */
  H2_GARDEN_GUARD_PLANT_PUMPKIN,
  H2_GARDEN_GUARD_PLANT_FROST,
  H2_GARDEN_GUARD_PLANT_BRAMBLE,
  H2_GARDEN_GUARD_PLANT_KIND_COUNT,
} h2_garden_guard_plant_kind_t;

/** Cards in toolbar order. */
typedef enum h2_garden_guard_card {
  H2_GARDEN_GUARD_CARD_SUNFLOWER = 0,
  H2_GARDEN_GUARD_CARD_SHOOTER,
  H2_GARDEN_GUARD_CARD_PUMPKIN,
  /** Removes a plant without refund. */
  H2_GARDEN_GUARD_CARD_SHOVEL,
  H2_GARDEN_GUARD_CARD_FROST,
  H2_GARDEN_GUARD_CARD_BRAMBLE,
  H2_GARDEN_GUARD_CARD_COUNT,
} h2_garden_guard_card_t;

/** Selection value when no card is selected. */
#define H2_GARDEN_GUARD_CARD_NONE (-1)

typedef enum h2_garden_guard_pest_kind {
  H2_GARDEN_GUARD_PEST_SHAMBLER = 0,
  /** A bulky zombie whose salvaged plate absorbs damage first. */
  H2_GARDEN_GUARD_PEST_SHELLBACK = 1,
  /** A fast, fragile courier zombie. */
  H2_GARDEN_GUARD_PEST_RUNNER = 2,
  H2_GARDEN_GUARD_PEST_KIND_COUNT,
} h2_garden_guard_pest_kind_t;

typedef enum h2_garden_guard_pointer {
  H2_GARDEN_GUARD_POINTER_DOWN = 1,
  H2_GARDEN_GUARD_POINTER_MOVE,
  H2_GARDEN_GUARD_POINTER_UP,
  /** Abandon the gesture, e.g. after a read failure. */
  H2_GARDEN_GUARD_POINTER_CANCEL,
} h2_garden_guard_pointer_t;

typedef enum h2_garden_guard_cart_state {
  /** Parked at the house edge, triggered by the first pest to get through. */
  H2_GARDEN_GUARD_CART_READY = 0,
  H2_GARDEN_GUARD_CART_SWEEPING,
  H2_GARDEN_GUARD_CART_USED,
} h2_garden_guard_cart_state_t;

/** Display geometry shared by input hit testing and rendering. */
typedef struct h2_garden_guard_layout {
  int32_t width, height;
  /** Scale relative to a 480-pixel short side. */
  float unit;
  /** Taller than wide; the courtyard behind the lawn gets the extra rows. */
  bool portrait;
  /** Screen rows of the lawn's near and far edges. */
  float near_y, far_y;
  /** Screen column of the lawn's centre line, which stays vertical. */
  float mid_x;
  /** Lawn width at the near edge, pixels. */
  float near_w;
  /** Perspective factor at the far edge: far width over near width. */
  float far_ratio;
  /** Bounding box of the lawn on screen, pixels. */
  float field_x, field_y, field_w, field_h;
  /** Cell width at the near edge: the scale of the nearest characters. */
  float sprite;
  /** Toolbar strip along the top edge. */
  float bar_h;
  /** Sun counter box and the cards, left to right. */
  float sun_x, sun_w;
  float card_x[H2_GARDEN_GUARD_CARD_COUNT];
  float card_y, card_w, card_h;
} h2_garden_guard_layout_t;

typedef struct h2_garden_guard_plant {
  uint8_t kind; /**< h2_garden_guard_plant_kind_t. */
  /** Damage stage shown by the renderer, 0 (intact) to 2. */
  uint8_t stage;
  float hp;
  /** Time until the next sun or shot; a shooter waits at 0 for a target. */
  int32_t timer_ms;
  /** Visual counters. action_ms is set by a real shot or sun allocation. */
  uint16_t grow_ms, action_ms, impact_ms;
} h2_garden_guard_plant_t;

/** Plant pose shared by the renderer and shovel hit area. */
typedef struct h2_garden_guard_plant_pose {
  float growth, sway, lift, action, impact;
  bool blink;
} h2_garden_guard_plant_pose_t;

typedef struct h2_garden_guard_pest {
  bool active;
  /** Stopped at a plant and chewing it. */
  bool eating;
  uint8_t kind; /**< h2_garden_guard_pest_kind_t. */
  uint8_t lane;
  /** Center position along the lane, in cells. */
  float x;
  int32_t hp, armor;
  /** Remaining feedback after a hit, and visual-only exit after removal. */
  uint16_t flash_ms, exit_ms;
  /** The locomotion clock holds while biting. */
  uint16_t stride_ms, chew_ms, slow_ms;
} h2_garden_guard_pest_t;

typedef struct h2_garden_guard_pea {
  bool active;
  uint8_t lane;
  bool frost;
  float x; /**< Cells. */
} h2_garden_guard_pea_t;

typedef struct h2_garden_guard_sun {
  bool active;
  /** Tapped: flying to the counter, already credited. */
  bool collecting;
  float x, y;      /**< Center, pixels. */
  float rest_y;    /**< Where a falling sun stops, pixels. */
  float vx, vy;    /**< Pixels per second while collecting. */
  uint32_t age_ms; /**< Time spent resting. */
} h2_garden_guard_sun_t;

typedef struct h2_garden_guard_particle {
  bool active;
  float x, y, vx, vy, size;
  uint16_t color; /**< RGB565. */
  uint16_t age_ms, life_ms;
} h2_garden_guard_particle_t;

typedef struct h2_garden_guard_cart {
  uint8_t state; /**< h2_garden_guard_cart_state_t. */
  float x;       /**< Cells while sweeping. */
} h2_garden_guard_cart_t;

/* Persisted bit positions are independent of toolbar display order. */
#define H2_GARDEN_GUARD_FROST_BIT 1u
#define H2_GARDEN_GUARD_BRAMBLE_BIT 2u
#define H2_GARDEN_GUARD_ALL_UNLOCKS 3u
#define H2_GARDEN_GUARD_ALL_SIGHTINGS 7u
#define H2_GARDEN_GUARD_SAVE_BYTES 24u

typedef struct h2_garden_guard_profile {
  uint32_t total_score, coins, best;
  uint8_t seen, owned;
} h2_garden_guard_profile_t;

typedef enum h2_garden_guard_menu {
  H2_GARDEN_GUARD_MENU_NONE = 0,
  H2_GARDEN_GUARD_MENU_JOURNAL,
  H2_GARDEN_GUARD_MENU_SHOP,
  H2_GARDEN_GUARD_MENU_PAUSE,
  H2_GARDEN_GUARD_MENU_CONFIRM,
} h2_garden_guard_menu_t;

typedef enum h2_garden_guard_language {
  H2_GARDEN_GUARD_LANG_EN = 0,
  H2_GARDEN_GUARD_LANG_ZH = 1,
} h2_garden_guard_language_t;

typedef enum h2_garden_guard_action {
  H2_GARDEN_GUARD_ACTION_START,
  H2_GARDEN_GUARD_ACTION_PAUSE,
  H2_GARDEN_GUARD_ACTION_LANGUAGE,
  H2_GARDEN_GUARD_ACTION_RESUME,
  H2_GARDEN_GUARD_ACTION_PAUSE_LANGUAGE,
  H2_GARDEN_GUARD_ACTION_LEAVE,
  H2_GARDEN_GUARD_ACTION_CANCEL,
  H2_GARDEN_GUARD_ACTION_CONFIRM,
  H2_GARDEN_GUARD_ACTION_RESULT_HOME,
  H2_GARDEN_GUARD_ACTION_RESULT_AGAIN,
} h2_garden_guard_action_t;

typedef struct h2_garden_guard_rect {
  float x, y, w, h;
} h2_garden_guard_rect_t;

typedef enum h2_garden_guard_save_state {
  H2_GARDEN_GUARD_SAVE_READY = 0,
  H2_GARDEN_GUARD_SAVE_SESSION,
  H2_GARDEN_GUARD_SAVE_RETRY,
} h2_garden_guard_save_state_t;

typedef struct h2_garden_guard_game {
  h2_garden_guard_layout_t layout;
  uint32_t rng;
  uint64_t time_ms;
  h2_garden_guard_phase_t phase;
  uint64_t phase_started_ms;

  uint32_t sun, score, best, kills;
  /** One-shot effect request: best rose during this run and needs saving. */
  bool best_changed;
  bool new_best;
  h2_garden_guard_profile_t profile;
  bool profile_dirty;
  uint8_t save_state;
  uint8_t menu;
  uint8_t language, modal_press;
  bool language_dirty, language_unsaved;
  uint16_t sighting_ms;
  uint8_t sighting_kind;
  uint32_t earned_coins, defense_score;

  /** Selected card, or H2_GARDEN_GUARD_CARD_NONE. */
  int32_t selected;
  /** The current press started on a card: releasing over the lawn plants. */
  bool drag_from_card;
  bool pointer_down;
  /** Cell under a card being dragged, or -1 values when there is none. */
  int32_t hover_lane, hover_col;
  uint32_t card_cooldown_ms[H2_GARDEN_GUARD_CARD_COUNT];
  /** Brief red flash on a card that could not be used. */
  uint32_t card_denied_ms[H2_GARDEN_GUARD_CARD_COUNT];

  h2_garden_guard_plant_t plants[H2_GARDEN_GUARD_LANES][H2_GARDEN_GUARD_COLS];
  /** Incremented whenever a plant appears, disappears or changes stage. */
  uint32_t plant_revision;
  h2_garden_guard_cart_t carts[H2_GARDEN_GUARD_LANES];

  /** Waves started so far, 0..WAVES. */
  uint32_t wave;
  /** Pests of the current wave still to spawn, per kind. */
  uint32_t wave_left[H2_GARDEN_GUARD_PEST_KIND_COUNT];
  uint64_t next_spawn_ms, next_wave_ms, next_sky_sun_ms;
  uint32_t last_lane;

  char banner[H2_GARDEN_GUARD_BANNER_TEXT_MAX];
  uint32_t banner_ms; /**< Remaining banner time; 0 hides it. */

  h2_garden_guard_pest_t pests[H2_GARDEN_GUARD_MAX_PESTS];
  h2_garden_guard_pea_t peas[H2_GARDEN_GUARD_MAX_PEAS];
  h2_garden_guard_sun_t suns[H2_GARDEN_GUARD_MAX_SUNS];
  h2_garden_guard_particle_t particles[H2_GARDEN_GUARD_MAX_PARTICLES];
} h2_garden_guard_game_t;

/** Reset to the title screen. width/height must be positive. */
void h2_garden_guard_game_init(h2_garden_guard_game_t* game, int32_t width,
                               int32_t height, uint32_t seed, uint32_t best);

/** Fixed, little-endian profile bytes with version and integrity check. */
void h2_garden_guard_profile_encode(const h2_garden_guard_profile_t* profile,
                                    uint8_t out[H2_GARDEN_GUARD_SAVE_BYTES]);
bool h2_garden_guard_profile_decode(h2_garden_guard_profile_t* profile,
                                    const void* data, size_t size);

bool h2_garden_guard_paused(const h2_garden_guard_game_t* game);
h2_garden_guard_rect_t h2_garden_guard_action_rect(
    const h2_garden_guard_layout_t* layout, h2_garden_guard_action_t action);

/** Shared screen-space rectangles for the menus and touch input. */
h2_garden_guard_rect_t h2_garden_guard_menu_tab(
    const h2_garden_guard_layout_t* layout, bool result, unsigned tab);
h2_garden_guard_rect_t h2_garden_guard_menu_row(
    const h2_garden_guard_layout_t* layout, unsigned row);
h2_garden_guard_rect_t h2_garden_guard_menu_back(
    const h2_garden_guard_layout_t* layout);
uint8_t h2_garden_guard_card_unlock(int32_t card);
uint32_t h2_garden_guard_offer_price(unsigned offer);

/** Feed one pointer edge at the current game time. */
void h2_garden_guard_game_pointer(h2_garden_guard_game_t* game,
                                  h2_garden_guard_pointer_t kind, float x,
                                  float y);

/** Advance the simulation; large steps are split internally. */
void h2_garden_guard_game_step(h2_garden_guard_game_t* game, uint32_t dt_ms);

/** Take the one-shot best-score save request; false when none is pending. */
bool h2_garden_guard_game_take_best(h2_garden_guard_game_t* game,
                                    uint32_t* out_best);

/** Sun cost of a card; zero for the shovel. */
uint32_t h2_garden_guard_card_cost(h2_garden_guard_card_t card);

/** Cooldown after a card was used, ms. */
uint32_t h2_garden_guard_card_cooldown(h2_garden_guard_card_t card);

/** Full health of a plant kind; zero for NONE. */
float h2_garden_guard_plant_max_hp(h2_garden_guard_plant_kind_t kind);

/** Perspective factor at depth f: 1 at the near edge, far_ratio at f = 1. */
float h2_garden_guard_depth_scale(const h2_garden_guard_layout_t* layout,
                                  float f);

/** Project ground coordinates (u, f) to display pixels. Values outside the
 * lawn are allowed and land on the patio, path, courtyard or front wall. */
void h2_garden_guard_ground_point(const h2_garden_guard_layout_t* layout,
                                  float u, float f, float* out_x, float* out_y);

/** Inverse of h2_garden_guard_ground_point for a display pixel; false only
 * above the horizon. */
bool h2_garden_guard_ground_at(const h2_garden_guard_layout_t* layout, float x,
                               float y, float* out_u, float* out_f);

/** Depth f where characters of a lane stand. */
float h2_garden_guard_lane_depth(uint32_t lane);

/** Character scale of a lane in pixels: the cell width at its depth. */
float h2_garden_guard_lane_size(const h2_garden_guard_layout_t* layout,
                                uint32_t lane);

/** Display pixel where a character at a lane position given in cells stands
 * on the ground. */
void h2_garden_guard_field_point(const h2_garden_guard_layout_t* layout,
                                 uint32_t lane, float along, float* out_x,
                                 float* out_y);

/** Ground cell under a display point; false outside the lawn. */
bool h2_garden_guard_cell_at(const h2_garden_guard_layout_t* layout, float x,
                             float y, uint32_t* out_lane, uint32_t* out_col);

/** Height of a plant kind's upright figure in lane sizes. */
float h2_garden_guard_plant_height(h2_garden_guard_plant_kind_t kind);

/** Sample a planted figure's pose. Toolbar and title use a neutral pose. */
h2_garden_guard_plant_pose_t h2_garden_guard_plant_pose(
    const h2_garden_guard_game_t* game, uint32_t lane, uint32_t col);

/** Plant whose upright figure covers a display point, nearest lane first;
 * false when no figure is there. */
bool h2_garden_guard_plant_at(const h2_garden_guard_game_t* game, float x,
                              float y, uint32_t* out_lane, uint32_t* out_col);

/** Card under a display point, or H2_GARDEN_GUARD_CARD_NONE. */
int32_t h2_garden_guard_card_at(const h2_garden_guard_layout_t* layout, float x,
                                float y);

/** Pixel center of the sun counter icon. */
void h2_garden_guard_sun_counter_point(const h2_garden_guard_layout_t* layout,
                                       float* out_x, float* out_y);

/** Sun radius in pixels for this layout. */
float h2_garden_guard_sun_radius(const h2_garden_guard_layout_t* layout);

/** Pests still to defeat in this run: active, pending and future waves. */
uint32_t h2_garden_guard_pests_remaining(const h2_garden_guard_game_t* game);

/** Total pests of all waves. */
uint32_t h2_garden_guard_pests_total(void);

#ifdef __cplusplus
}
#endif

#endif
