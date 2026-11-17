#ifndef H2_XIANXIA_RPG_GAME_H
#define H2_XIANXIA_RPG_GAME_H

/* Deterministic game model. It owns no PAL resources: the run loop feeds it
 * elapsed time and pointer edges, and the renderer reads it. Coordinates are
 * display pixels with the origin at the top-left corner; the map is a fixed
 * tile grid scaled to the display. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Host-endian RGB565 from 8-bit channels. */
#define H2_XIANXIA_RPG_RGB(r, g, b)            \
  ((uint16_t)((((unsigned)(r) & 0xF8u) << 8) | \
              (((unsigned)(g) & 0xFCu) << 3) | ((unsigned)(b) >> 3)))

#define H2_XIANXIA_RPG_PI 3.14159265f

#define H2_XIANXIA_RPG_MAP_COLS 15
#define H2_XIANXIA_RPG_MAP_ROWS 10
#define H2_XIANXIA_RPG_PATH_MAX \
  (H2_XIANXIA_RPG_MAP_COLS * H2_XIANXIA_RPG_MAP_ROWS)
#define H2_XIANXIA_RPG_MAX_NPCS 4
#define H2_XIANXIA_RPG_MAX_PARTICLES 20
#define H2_XIANXIA_RPG_MAX_POPUPS 4
#define H2_XIANXIA_RPG_MAX_HITS 3
#define H2_XIANXIA_RPG_COMMANDS 5
#define H2_XIANXIA_RPG_TITLE_BUTTONS 2
#define H2_XIANXIA_RPG_MESSAGE_MAX 160
/** Line pitch of body text; the body font is generated at 18 px. */
#define H2_XIANXIA_RPG_LINE_PX 24

/** Time to walk one tile. */
#define H2_XIANXIA_RPG_STEP_MS 160u
/** Typewriter speed of dialogue text. */
#define H2_XIANXIA_RPG_REVEAL_MS_PER_CHAR 28u
#define H2_XIANXIA_RPG_BANNER_MS 1800u
/** How long an objective change stays announced. */
#define H2_XIANXIA_RPG_OBJECTIVE_MS 2600u
#define H2_XIANXIA_RPG_INTRO_MS 900u
#define H2_XIANXIA_RPG_ACT_MS 900u
#define H2_XIANXIA_RPG_HIT_MS 260u
#define H2_XIANXIA_RPG_HIT_GAP_MS 160u
#define H2_XIANXIA_RPG_FLASH_MS 220u
#define H2_XIANXIA_RPG_POPUP_MS 900u
/** Taps are ignored this long after a screen change, so one press cannot
 * select through two screens. */
#define H2_XIANXIA_RPG_TAP_GUARD_MS 250u

#define H2_XIANXIA_RPG_SKILL_MP 7u
#define H2_XIANXIA_RPG_PILLS_MAX 9u
#define H2_XIANXIA_RPG_LEVEL_MAX 20u

/** Persisted progress format version; other versions are ignored. */
#define H2_XIANXIA_RPG_SAVE_VERSION 1u
/** Words 0..2 hold progress. Word 3 holds the ending statistics and decodes
 * as zero when absent, so saves written before it existed stay valid. */
#define H2_XIANXIA_RPG_SAVE_WORDS 4u
#define H2_XIANXIA_RPG_SAVE_REQUIRED_WORDS 3u

typedef enum h2_xianxia_rpg_phase {
  H2_XIANXIA_RPG_PHASE_TITLE = 0,
  H2_XIANXIA_RPG_PHASE_EXPLORE,
  H2_XIANXIA_RPG_PHASE_BATTLE,
  H2_XIANXIA_RPG_PHASE_ENDING,
} h2_xianxia_rpg_phase_t;

typedef enum h2_xianxia_rpg_map {
  H2_XIANXIA_RPG_MAP_VILLAGE = 0,
  H2_XIANXIA_RPG_MAP_FOREST,
  H2_XIANXIA_RPG_MAP_SHRINE,
  H2_XIANXIA_RPG_MAP_COUNT,
} h2_xianxia_rpg_map_t;

/** Story progress; each value names the current objective. */
typedef enum h2_xianxia_rpg_stage {
  H2_XIANXIA_RPG_STAGE_MEET_ELDER = 0,
  H2_XIANXIA_RPG_STAGE_SEEK_DAOIST,
  H2_XIANXIA_RPG_STAGE_TO_SHRINE,
  /** The boss is sealed; report back to the elder. */
  H2_XIANXIA_RPG_STAGE_RETURN,
  H2_XIANXIA_RPG_STAGE_CLEARED,
  H2_XIANXIA_RPG_STAGE_COUNT,
} h2_xianxia_rpg_stage_t;

typedef enum h2_xianxia_rpg_facing {
  H2_XIANXIA_RPG_FACING_DOWN = 0,
  H2_XIANXIA_RPG_FACING_UP,
  H2_XIANXIA_RPG_FACING_LEFT,
  H2_XIANXIA_RPG_FACING_RIGHT,
} h2_xianxia_rpg_facing_t;

/** Character art; also the dialogue speaker. */
typedef enum h2_xianxia_rpg_actor {
  H2_XIANXIA_RPG_ACTOR_NARRATOR = 0,
  H2_XIANXIA_RPG_ACTOR_HERO,
  H2_XIANXIA_RPG_ACTOR_ELDER,
  H2_XIANXIA_RPG_ACTOR_HERBALIST,
  H2_XIANXIA_RPG_ACTOR_CHILD,
  H2_XIANXIA_RPG_ACTOR_DAOIST,
  H2_XIANXIA_RPG_ACTOR_DRAGON,
  H2_XIANXIA_RPG_ACTOR_COUNT,
} h2_xianxia_rpg_actor_t;

typedef enum h2_xianxia_rpg_enemy {
  H2_XIANXIA_RPG_ENEMY_LEAF_SPRITE = 0,
  H2_XIANXIA_RPG_ENEMY_CIVET,
  H2_XIANXIA_RPG_ENEMY_WISP,
  H2_XIANXIA_RPG_ENEMY_GOLEM,
  H2_XIANXIA_RPG_ENEMY_DRAGON,
  H2_XIANXIA_RPG_ENEMY_COUNT,
} h2_xianxia_rpg_enemy_t;

typedef enum h2_xianxia_rpg_battle_state {
  H2_XIANXIA_RPG_BATTLE_INTRO = 0,
  H2_XIANXIA_RPG_BATTLE_COMMAND,
  H2_XIANXIA_RPG_BATTLE_HERO_ACT,
  H2_XIANXIA_RPG_BATTLE_ENEMY_ACT,
  H2_XIANXIA_RPG_BATTLE_VICTORY,
  H2_XIANXIA_RPG_BATTLE_DEFEAT,
  H2_XIANXIA_RPG_BATTLE_FLED,
} h2_xianxia_rpg_battle_state_t;

typedef enum h2_xianxia_rpg_command {
  H2_XIANXIA_RPG_COMMAND_ATTACK = 0,
  H2_XIANXIA_RPG_COMMAND_SKILL,
  H2_XIANXIA_RPG_COMMAND_PILL,
  H2_XIANXIA_RPG_COMMAND_GUARD,
  H2_XIANXIA_RPG_COMMAND_FLEE,
} h2_xianxia_rpg_command_t;

typedef enum h2_xianxia_rpg_pointer {
  H2_XIANXIA_RPG_POINTER_DOWN = 1,
  H2_XIANXIA_RPG_POINTER_MOVE,
  H2_XIANXIA_RPG_POINTER_UP,
  /** Abandon the press without acting, e.g. after a read failure. */
  H2_XIANXIA_RPG_POINTER_CANCEL,
} h2_xianxia_rpg_pointer_t;

/** One-shot effect applied when a dialogue script finishes. */
typedef enum h2_xianxia_rpg_effect {
  H2_XIANXIA_RPG_EFFECT_NONE = 0,
  H2_XIANXIA_RPG_EFFECT_BRIEFED,
  H2_XIANXIA_RPG_EFFECT_GIVE_PILLS,
  H2_XIANXIA_RPG_EFFECT_TAUGHT,
  H2_XIANXIA_RPG_EFFECT_REST,
  H2_XIANXIA_RPG_EFFECT_BOSS_BATTLE,
  H2_XIANXIA_RPG_EFFECT_ENDING,
} h2_xianxia_rpg_effect_t;

/** Story flags kept in the save. */
#define H2_XIANXIA_RPG_FLAG_PILLS_GIVEN 0x01u

typedef struct h2_xianxia_rpg_rect {
  int32_t x, y, w, h;
} h2_xianxia_rpg_rect_t;

typedef struct h2_xianxia_rpg_layout {
  int32_t tile;
  int32_t map_x, map_y;
  /** True places HUD and dialogue below the map; false overlays the
   * dialogue on the lower map edge under a HUD strip. */
  bool stacked;
  h2_xianxia_rpg_rect_t hud, dialog;
  int32_t dialog_lines;
  h2_xianxia_rpg_rect_t title_buttons[H2_XIANXIA_RPG_TITLE_BUTTONS];
  /** Battle: picture, lower panel, its message box and command buttons. */
  h2_xianxia_rpg_rect_t stage, panel, message;
  h2_xianxia_rpg_rect_t commands[H2_XIANXIA_RPG_COMMANDS];
} h2_xianxia_rpg_layout_t;

typedef struct h2_xianxia_rpg_hero {
  uint32_t level, exp;
  uint32_t hp, hp_max, mp, mp_max, attack, defense;
  uint32_t pills;
} h2_xianxia_rpg_hero_t;

typedef struct h2_xianxia_rpg_line {
  uint8_t speaker; /**< h2_xianxia_rpg_actor_t */
  const char* text;
} h2_xianxia_rpg_line_t;

typedef struct h2_xianxia_rpg_script {
  const h2_xianxia_rpg_line_t* lines;
  uint8_t count;
  uint8_t effect; /**< h2_xianxia_rpg_effect_t */
} h2_xianxia_rpg_script_t;

typedef struct h2_xianxia_rpg_dialogue {
  bool active;
  const h2_xianxia_rpg_script_t* script;
  uint8_t index;
  /** Codepoints of the current line shown so far. */
  uint32_t revealed, total;
  uint32_t reveal_ms;
} h2_xianxia_rpg_dialogue_t;

/** A character standing on the map. */
typedef struct h2_xianxia_rpg_npc {
  uint8_t actor;
  uint8_t x, y;
  uint8_t facing;
} h2_xianxia_rpg_npc_t;

typedef struct h2_xianxia_rpg_enemy_info {
  const char* name;
  const char* special;
  uint32_t hp, attack, defense, exp;
  /** Main body and accent colors, RGB565. */
  uint16_t body, accent;
} h2_xianxia_rpg_enemy_info_t;

typedef struct h2_xianxia_rpg_popup {
  bool active;
  bool on_enemy;
  bool heal;
  uint32_t value;
  uint32_t age_ms;
  int32_t slot; /**< Small horizontal offset for multi-hit popups. */
} h2_xianxia_rpg_popup_t;

typedef struct h2_xianxia_rpg_battle {
  h2_xianxia_rpg_battle_state_t state;
  uint32_t state_ms;
  uint8_t enemy; /**< h2_xianxia_rpg_enemy_t */
  bool boss;
  uint32_t enemy_hp, enemy_hp_max, enemy_attack, enemy_defense;
  uint32_t turn;
  bool charging, enraged, guarding;
  uint8_t command; /**< Last chosen h2_xianxia_rpg_command_t */
  /** Pending hits of the current action, applied over its animation. */
  uint32_t hits[H2_XIANXIA_RPG_MAX_HITS];
  uint32_t hit_count, hits_applied;
  bool hit_on_enemy, special;
  uint32_t enemy_flash_ms, hero_flash_ms;
  uint32_t exp_gain;
  char message[H2_XIANXIA_RPG_MESSAGE_MAX];
  /** Changes whenever the message text changes. */
  uint32_t message_id;
  h2_xianxia_rpg_popup_t popups[H2_XIANXIA_RPG_MAX_POPUPS];
} h2_xianxia_rpg_battle_t;

typedef struct h2_xianxia_rpg_particle {
  bool active;
  float x, y, vx, vy, phase;
  uint32_t age_ms, life_ms;
  uint8_t kind; /**< 0 leaf, 1 petal, 2 firefly */
} h2_xianxia_rpg_particle_t;

typedef struct h2_xianxia_rpg_save {
  uint32_t stage, level, exp, pills, flags, map, x, y;
  /** Battles won and turns taken, shown on the ending screen. */
  uint32_t battles, turns;
} h2_xianxia_rpg_save_t;

typedef struct h2_xianxia_rpg_game {
  int32_t width, height;
  float unit; /**< Scale relative to a 480-pixel short side. */
  h2_xianxia_rpg_layout_t layout;
  uint32_t rng;
  uint64_t time_ms;
  h2_xianxia_rpg_phase_t phase;
  uint64_t phase_started_ms;

  /* Title. */
  bool has_save;
  h2_xianxia_rpg_save_t saved;

  /* Progress. */
  h2_xianxia_rpg_stage_t stage;
  uint32_t flags;
  h2_xianxia_rpg_hero_t hero;
  uint32_t battles_won, turns_taken;

  /* Exploration. */
  h2_xianxia_rpg_map_t map;
  int32_t tx, ty, from_x, from_y;
  h2_xianxia_rpg_facing_t facing;
  bool moving;
  uint32_t move_ms;
  uint32_t walk_steps;
  uint8_t path_x[H2_XIANXIA_RPG_PATH_MAX], path_y[H2_XIANXIA_RPG_PATH_MAX];
  uint32_t path_len, path_pos;
  /** Pending interaction at the end of the path, or -1. */
  int32_t target_x, target_y;
  uint32_t encounter_steps;
  h2_xianxia_rpg_map_t rest_map;
  int32_t rest_x, rest_y;
  h2_xianxia_rpg_dialogue_t dialogue;
  uint32_t banner_ms;
  /** Remaining time of the objective-update announcement. */
  uint32_t objective_ms;
  int32_t marker_x, marker_y;
  uint32_t marker_ms;

  h2_xianxia_rpg_battle_t battle;
  h2_xianxia_rpg_particle_t particles[H2_XIANXIA_RPG_MAX_PARTICLES];

  /** One-shot effect request: progress needs saving. */
  bool save_pending;
} h2_xianxia_rpg_game_t;

/** Reset to the title screen. width/height must be positive; save may be
 * NULL and is copied when valid. */
void h2_xianxia_rpg_game_init(h2_xianxia_rpg_game_t* game, int32_t width,
                              int32_t height, uint32_t seed,
                              const h2_xianxia_rpg_save_t* save);

/** Feed one pointer edge at the current game time. Presses act on DOWN. */
void h2_xianxia_rpg_game_pointer(h2_xianxia_rpg_game_t* game,
                                 h2_xianxia_rpg_pointer_t kind, float x,
                                 float y);

/** Advance the simulation; large steps are split internally. */
void h2_xianxia_rpg_game_step(h2_xianxia_rpg_game_t* game, uint32_t dt_ms);

/** Take the one-shot save request; false when none is pending. */
bool h2_xianxia_rpg_game_take_save(h2_xianxia_rpg_game_t* game,
                                   h2_xianxia_rpg_save_t* out_save);

/** Encode a save into fixed words, or decode and validate them. */
void h2_xianxia_rpg_save_pack(const h2_xianxia_rpg_save_t* save,
                              uint32_t words[H2_XIANXIA_RPG_SAVE_WORDS]);
bool h2_xianxia_rpg_save_unpack(const uint32_t words[H2_XIANXIA_RPG_SAVE_WORDS],
                                h2_xianxia_rpg_save_t* out_save);

/* Static world data, owned by the story table. */

/** Map tile character at a cell; '#' outside the grid. */
char h2_xianxia_rpg_tile(h2_xianxia_rpg_map_t map, int32_t x, int32_t y);
/** Whether the hero may stand on a cell at the given stage. */
bool h2_xianxia_rpg_walkable(const h2_xianxia_rpg_game_t* game,
                             h2_xianxia_rpg_map_t map, int32_t x, int32_t y);
/** Characters standing on a map at the current stage; returns the count. */
size_t h2_xianxia_rpg_npcs(const h2_xianxia_rpg_game_t* game,
                           h2_xianxia_rpg_map_t map,
                           h2_xianxia_rpg_npc_t out[H2_XIANXIA_RPG_MAX_NPCS]);
const char* h2_xianxia_rpg_map_name(h2_xianxia_rpg_map_t map);
const char* h2_xianxia_rpg_actor_name(h2_xianxia_rpg_actor_t actor);
const char* h2_xianxia_rpg_objective(h2_xianxia_rpg_stage_t stage);
const h2_xianxia_rpg_enemy_info_t* h2_xianxia_rpg_enemy_info(
    h2_xianxia_rpg_enemy_t enemy);
const char* h2_xianxia_rpg_command_label(h2_xianxia_rpg_command_t command);
/** Every fixed UTF-8 string the story and UI may draw, for glyph checks. */
size_t h2_xianxia_rpg_story_strings(const char** out, size_t capacity);

/** Hero stats at a level, with full HP/MP and no pills. */
void h2_xianxia_rpg_hero_at_level(h2_xianxia_rpg_hero_t* hero, uint32_t level);
/** Total experience needed to reach a level from level 1. */
uint32_t h2_xianxia_rpg_exp_for_level(uint32_t level);

/** Whether a command can be chosen now; reason is set when it cannot. */
bool h2_xianxia_rpg_command_available(const h2_xianxia_rpg_game_t* game,
                                      h2_xianxia_rpg_command_t command);

/** Number of UTF-8 codepoints in a string. */
uint32_t h2_xianxia_rpg_utf8_length(const char* text);
/** Decode one codepoint; advances *text. Invalid bytes decode as U+FFFD. */
uint32_t h2_xianxia_rpg_utf8_next(const char** text);

#ifdef __cplusplus
}
#endif

#endif
