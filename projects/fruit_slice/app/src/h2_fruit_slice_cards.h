#ifndef H2_FRUIT_SLICE_CARDS_H
#define H2_FRUIT_SLICE_CARDS_H

/* Card collection model: catalog, persistent counts, per-run award rules,
 * reveal sequencing and the album view state. It owns no PAL resources and
 * never allocates; every effect pool has a fixed capacity. The game model
 * reports play facts and routes pointer edges here while a reveal or the
 * album is active. Time advances only through h2_fruit_slice_cards_step(). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stable card IDs: each is a count slot in the saved collection. */
typedef enum h2_fruit_slice_card_id {
  H2_FRUIT_SLICE_CARD_WATERMELON = 0,
  H2_FRUIT_SLICE_CARD_ORANGE,
  H2_FRUIT_SLICE_CARD_APPLE,
  H2_FRUIT_SLICE_CARD_KIWI,
  H2_FRUIT_SLICE_CARD_LEMON,
  H2_FRUIT_SLICE_CARD_FRUIT_SALAD,
  H2_FRUIT_SLICE_CARD_CLASSIC_KNIFE,
  H2_FRUIT_SLICE_CARD_JADE_KATANA,
  H2_FRUIT_SLICE_CARD_FLAME_AXE,
  H2_FRUIT_SLICE_CARD_BOMB,
  H2_FRUIT_SLICE_CARD_CLOSE_CALL,
  H2_FRUIT_SLICE_CARD_BOMB_SQUAD,
  H2_FRUIT_SLICE_CARD_COMBO,
  H2_FRUIT_SLICE_CARD_MEGA_COMBO,
  H2_FRUIT_SLICE_CARD_HIGH_SCORE,
  H2_FRUIT_SLICE_CARD_LEGEND,
  H2_FRUIT_SLICE_CARD_COUNT,
} h2_fruit_slice_card_id_t;

typedef enum h2_fruit_slice_card_rarity {
  H2_FRUIT_SLICE_RARITY_COMMON = 0,
  H2_FRUIT_SLICE_RARITY_RARE,
  H2_FRUIT_SLICE_RARITY_EPIC,
  H2_FRUIT_SLICE_RARITY_LEGENDARY,
  H2_FRUIT_SLICE_RARITY_COUNT,
} h2_fruit_slice_card_rarity_t;

typedef enum h2_fruit_slice_card_category {
  H2_FRUIT_SLICE_CARD_TAB_FRUIT = 0,
  H2_FRUIT_SLICE_CARD_TAB_WEAPON,
  H2_FRUIT_SLICE_CARD_TAB_BOMB,
  H2_FRUIT_SLICE_CARD_TAB_SKILL,
  H2_FRUIT_SLICE_CARD_TABS,
} h2_fruit_slice_card_category_t;

/** Sound cues for an audio integration; see h2_fruit_slice_cards_take_cue. */
typedef enum h2_fruit_slice_card_cue {
  H2_FRUIT_SLICE_CARD_CUE_NONE = 0,
  /** A card dropped during play, or a common card turned face up. */
  H2_FRUIT_SLICE_CARD_CUE_CARD,
  /** A rare, epic or legendary card turned face up in the reveal. */
  H2_FRUIT_SLICE_CARD_CUE_RARE,
} h2_fruit_slice_card_cue_t;

/** Album view outcome of a pointer edge, applied by the game model. */
typedef enum h2_fruit_slice_cards_action {
  H2_FRUIT_SLICE_CARDS_ACTION_NONE = 0,
  /** A reveal sequence finished; the result screen accepts input again. */
  H2_FRUIT_SLICE_CARDS_ACTION_REVEAL_DONE,
} h2_fruit_slice_cards_action_t;

#define H2_FRUIT_SLICE_CARD_MAX_COUNT 99u
#define H2_FRUIT_SLICE_CARDS_BYTES 32u
#define H2_FRUIT_SLICE_CARD_SPARKS 48
#define H2_FRUIT_SLICE_CARD_FLIGHTS 4
#define H2_FRUIT_SLICE_CARD_CUES 4
#define H2_FRUIT_SLICE_CARD_TAB_SLOTS 6u

/* Reveal timeline, in milliseconds from the start of each card. */
#define H2_FRUIT_SLICE_REVEAL_ZOOM_MS 350u
#define H2_FRUIT_SLICE_REVEAL_FLIP_START_MS 650u
#define H2_FRUIT_SLICE_REVEAL_FACE_MS 950u
#define H2_FRUIT_SLICE_REVEAL_INPUT_MS 1100u
/** Delay after a run ends before new cards are revealed. */
#define H2_FRUIT_SLICE_REVEAL_DELAY_MS 700u
#define H2_FRUIT_SLICE_CARD_FLIGHT_MS 1000u
#define H2_FRUIT_SLICE_CARD_TAB_SLIDE_MS 360u

/* Award rules, exposed for tests and the album hints. */
#define H2_FRUIT_SLICE_CARD_DROP_NEW_PERCENT 12u
#define H2_FRUIT_SLICE_CARD_DROP_OWNED_PERCENT 3u
/** A fruit card that has not dropped after this many slices of its kind
 * drops on the next one. */
#define H2_FRUIT_SLICE_CARD_DROP_PITY 20u
#define H2_FRUIT_SLICE_CARD_SALAD_SLICES 30u
#define H2_FRUIT_SLICE_CARD_WEAPON_SCORE 10u
#define H2_FRUIT_SLICE_CARD_CLOSE_CALL_BOMBS 3u
#define H2_FRUIT_SLICE_CARD_SQUAD_BOMBS 8u
#define H2_FRUIT_SLICE_CARD_MEGA_COMBO_SLICES 5u
#define H2_FRUIT_SLICE_CARD_HIGH_SCORE_TARGET 30u
#define H2_FRUIT_SLICE_CARD_LEGEND_SCORE 60u

typedef struct h2_fruit_slice_card {
  const char* name;
  const char* hint; /**< How to earn it, shown for locked cards. */
  uint8_t category; /**< h2_fruit_slice_card_category_t */
  uint8_t rarity;   /**< h2_fruit_slice_card_rarity_t */
} h2_fruit_slice_card_t;

typedef struct h2_fruit_slice_card_spark {
  bool active;
  float x, y, vx, vy, size;
  uint16_t color; /**< RGB565. */
  uint16_t age_ms, life_ms;
} h2_fruit_slice_card_spark_t;

/** A card popping out of the play field and flying to the HUD badge. */
typedef struct h2_fruit_slice_card_flight {
  bool active;
  bool duplicate;
  uint8_t card;
  float x, y; /**< Origin, display pixels. */
  uint16_t age_ms;
} h2_fruit_slice_card_flight_t;

typedef struct h2_fruit_slice_cards {
  /* Persistent collection. */
  uint8_t counts[H2_FRUIT_SLICE_CARD_COUNT];
  uint32_t unseen; /**< Owned cards not yet opened in the album. */

  /* Persistence: dirty requests a save; loaded is false when the stored
   * collection could not be read yet and must be merged before writing. */
  bool dirty, loaded;
  uint32_t
      seen_before_load; /**< Locally inspected copies while awaiting a read. */
  uint32_t save_status; /**< 0 ok, 1 session only, 2 retry read/write, 3
                           protected. */

  /** Bumped whenever anything drawn on the cached static layer changes. */
  uint32_t revision;

  uint32_t rng;
  uint64_t time_ms;
  int32_t width, height;
  float unit;

  /* Current run. */
  bool run_active;
  uint32_t run_earned; /**< Cards awarded this run; one award each. */
  uint32_t run_new;    /**< Of those, first copies. */
  uint32_t run_slices, run_bombs_passed;
  uint8_t pity[5];
  uint16_t badge_pulse_ms; /**< Remaining HUD badge pulse after a catch. */

  /* Reveal of first copies after a run. */
  uint32_t reveal_queue; /**< New cards waiting, by ID bit. */
  bool reveal_active;
  uint8_t reveal_card, reveal_index, reveal_total;
  uint32_t reveal_ms;
  bool reveal_cued;

  /* Album. */
  bool album_open;
  uint8_t tab, selected; /**< Selected slot within the tab. */
  int8_t tab_direction;  /**< -1 or +1: side the new tab slides in from. */
  uint32_t tab_ms, select_ms, album_ms;
  bool pressing;
  float press_x, press_y;

  h2_fruit_slice_card_flight_t flights[H2_FRUIT_SLICE_CARD_FLIGHTS];
  h2_fruit_slice_card_spark_t sparks[H2_FRUIT_SLICE_CARD_SPARKS];

  uint8_t cues[H2_FRUIT_SLICE_CARD_CUES];
  uint8_t cue_count;
} h2_fruit_slice_cards_t;

/** Layout rectangle, display pixels, half-open. */
typedef struct h2_fruit_slice_card_rect {
  float x0, y0, x1, y1;
} h2_fruit_slice_card_rect_t;

/** Album geometry shared by input and rendering. */
typedef struct h2_fruit_slice_album_layout {
  h2_fruit_slice_card_rect_t title, progress, detail;
  h2_fruit_slice_card_rect_t tabs[H2_FRUIT_SLICE_CARD_TABS];
  h2_fruit_slice_card_rect_t slots[H2_FRUIT_SLICE_CARD_TAB_SLOTS];
  float card_w, card_h; /**< Card size inside every slot. */
  float art_radius;     /**< Fruit and bomb sprite radius on album cards. */
} h2_fruit_slice_album_layout_t;

/** Card size and center for the full-screen reveal. */
typedef struct h2_fruit_slice_reveal_layout {
  float cx, cy, card_w, card_h, art_radius;
} h2_fruit_slice_reveal_layout_t;

const h2_fruit_slice_card_t* h2_fruit_slice_card(unsigned id);

/** Stable fruit -> card mapping. -1 for fruit without an album card, bombs
 * and invalid kinds. New fruit still count for generic run achievements. */
int h2_fruit_slice_card_for_fruit(unsigned kind);

/** Rarity accent, RGB565. */
uint16_t h2_fruit_slice_card_rarity_color(unsigned rarity);
const char* h2_fruit_slice_card_rarity_name(unsigned rarity);
const char* h2_fruit_slice_card_tab_name(unsigned tab);

/** Card IDs of a tab in display order; returns how many were written. */
unsigned h2_fruit_slice_card_tab_cards(
    unsigned tab, uint8_t out[H2_FRUIT_SLICE_CARD_TAB_SLOTS]);

/** Reset to an empty, unloaded collection for a display of this size. */
void h2_fruit_slice_cards_init(h2_fruit_slice_cards_t* cards, int32_t width,
                               int32_t height, uint32_t seed);

bool h2_fruit_slice_cards_owned(const h2_fruit_slice_cards_t* cards,
                                unsigned id);
unsigned h2_fruit_slice_cards_owned_total(const h2_fruit_slice_cards_t* cards);
unsigned h2_fruit_slice_cards_run_landed(const h2_fruit_slice_cards_t* cards);

/* Explicit little-endian wire format: magic, version, unseen mask, slot
 * count, then one count byte per card. Decode leaves the collection
 * unchanged and returns false for malformed or unknown versions. */
void h2_fruit_slice_cards_encode(const h2_fruit_slice_cards_t* cards,
                                 uint8_t out[H2_FRUIT_SLICE_CARDS_BYTES]);
bool h2_fruit_slice_cards_decode(h2_fruit_slice_cards_t* cards,
                                 const void* data, size_t size);
/** Merge the stored baseline once, while !loaded. Until then counts contain
 * only session awards. Preserve local inspections, cap counts at 99 and let
 * an active reveal finish; future duplicate reveals are removed. Failure,
 * including already loaded, leaves state unchanged. */
bool h2_fruit_slice_cards_merge(h2_fruit_slice_cards_t* cards, const void* data,
                                size_t size);

/* Play facts. Positions are display pixels; they place the flight origin. */
void h2_fruit_slice_cards_run_start(h2_fruit_slice_cards_t* cards);
void h2_fruit_slice_cards_on_slice(h2_fruit_slice_cards_t* cards,
                                   unsigned fruit_kind, uint32_t score,
                                   unsigned weapon, float x, float y);
void h2_fruit_slice_cards_on_combo(h2_fruit_slice_cards_t* cards,
                                   uint32_t slices, uint32_t score,
                                   unsigned weapon, float x, float y);
void h2_fruit_slice_cards_on_bomb_hit(h2_fruit_slice_cards_t* cards, float x,
                                      float y);
void h2_fruit_slice_cards_on_bomb_passed(h2_fruit_slice_cards_t* cards, float x,
                                         float y);
/** Ends the run; new cards are queued for the reveal. */
void h2_fruit_slice_cards_run_end(h2_fruit_slice_cards_t* cards);

/** Advance effects. allow_reveal starts a queued reveal. */
void h2_fruit_slice_cards_step(h2_fruit_slice_cards_t* cards, uint32_t ms,
                               bool allow_reveal);

/** True while the reveal consumes every pointer edge. */
bool h2_fruit_slice_cards_reveal_active(const h2_fruit_slice_cards_t* cards);

/** Pointer edge during a reveal; kind uses h2_fruit_slice_pointer_t values. */
h2_fruit_slice_cards_action_t h2_fruit_slice_cards_reveal_pointer(
    h2_fruit_slice_cards_t* cards, int kind);

/** Open the album at the first unseen card, or keep the last position. */
void h2_fruit_slice_cards_open_album(h2_fruit_slice_cards_t* cards);
void h2_fruit_slice_cards_close_album(h2_fruit_slice_cards_t* cards);
/** Pointer edge inside the album outside the shared Back button. */
void h2_fruit_slice_cards_album_pointer(h2_fruit_slice_cards_t* cards, int kind,
                                        float x, float y);

/** Card ID shown in an album slot of the current tab, or -1 if empty. */
int h2_fruit_slice_cards_slot_card(const h2_fruit_slice_cards_t* cards,
                                   unsigned slot);

/** True while the album cards slide in after a tab change. */
bool h2_fruit_slice_cards_tab_sliding(const h2_fruit_slice_cards_t* cards);

void h2_fruit_slice_cards_album_layout(int32_t width, int32_t height,
                                       float unit,
                                       h2_fruit_slice_album_layout_t* out);
void h2_fruit_slice_cards_reveal_layout(int32_t width, int32_t height,
                                        h2_fruit_slice_reveal_layout_t* out);

/** Take the oldest sound cue; NONE when the bounded queue is empty. The queue
 * keeps the first H2_FRUIT_SLICE_CARD_CUES cues and drops later ones. */
h2_fruit_slice_card_cue_t h2_fruit_slice_cards_take_cue(
    h2_fruit_slice_cards_t* cards);

#ifdef __cplusplus
}
#endif

#endif
