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
#define H2_XIANXIA_RPG_TITLE_BUTTONS 3
#define H2_XIANXIA_RPG_MESSAGE_MAX 160
/** Enemies one map may send into a random encounter. */
#define H2_XIANXIA_RPG_ENCOUNTER_MAX 3
/** Chapters the progress record can hold, and those this build plays. */
#define H2_XIANXIA_RPG_CHAPTER_MAX 10u
#define H2_XIANXIA_RPG_CHAPTERS_PLAYABLE 7u
/** Rows of the chapter screen: playable chapters and the next one. */
#define H2_XIANXIA_RPG_CHAPTER_ROWS 8
/** Lanterns of rapport with the companion; full rapport unlocks the joint
 * strike. */
#define H2_XIANXIA_RPG_RAPPORT_MAX 3u
#define H2_XIANXIA_RPG_TRUST_MAX 3u
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
/** The companion acts faster than hero and enemy. */
#define H2_XIANXIA_RPG_ALLY_ACT_MS 700u
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

/** Chapter-1 journey format shared with v0.1; other versions are ignored. */
#define H2_XIANXIA_RPG_SAVE_VERSION 1u
/** Words 0..2 hold progress. Word 3 holds the ending statistics and decodes
 * as zero when absent, so saves written before it existed stay valid. */
#define H2_XIANXIA_RPG_SAVE_WORDS 4u
#define H2_XIANXIA_RPG_SAVE_REQUIRED_WORDS 3u
/** Blob formats of the later-chapter journey and the chapter record. */
#define H2_XIANXIA_RPG_RECORD_VERSION 2u
#define H2_XIANXIA_RPG_JOURNEY_BYTES 32u
/** Chapter 6 adds a second fact word; older records keep their v2 bytes. */
#define H2_XIANXIA_RPG_JOURNEY6_BYTES 36u
#define H2_XIANXIA_RPG_JOURNEY7_BYTES 36u
#define H2_XIANXIA_RPG_CLEAR_BYTES 16u
#define H2_XIANXIA_RPG_PROGRESS_BYTES \
  (4u + H2_XIANXIA_RPG_CHAPTER_MAX * H2_XIANXIA_RPG_CLEAR_BYTES + 4u)

typedef enum h2_xianxia_rpg_phase {
  H2_XIANXIA_RPG_PHASE_TITLE = 0,
  H2_XIANXIA_RPG_PHASE_EXPLORE,
  H2_XIANXIA_RPG_PHASE_BATTLE,
  H2_XIANXIA_RPG_PHASE_ENDING,
  /** Chapter list opened from the title. */
  H2_XIANXIA_RPG_PHASE_CHAPTERS,
  /** Chapter card with the story so far, before a chapter starts. */
  H2_XIANXIA_RPG_PHASE_PROLOGUE,
} h2_xianxia_rpg_phase_t;

/** Maps of every chapter; chapter 1 owns the first three. */
typedef enum h2_xianxia_rpg_map {
  H2_XIANXIA_RPG_MAP_VILLAGE = 0,
  H2_XIANXIA_RPG_MAP_FOREST,
  H2_XIANXIA_RPG_MAP_SHRINE,
  H2_XIANXIA_RPG_MAP_DOCK,
  H2_XIANXIA_RPG_MAP_REEDS,
  H2_XIANXIA_RPG_MAP_WRECK,
  H2_XIANXIA_RPG_MAP_RAIN_PATH,
  H2_XIANXIA_RPG_MAP_SWORD_COURT,
  H2_XIANXIA_RPG_MAP_SWORD_HALL,
  H2_XIANXIA_RPG_MAP_NIGHT_VILLAGE,
  H2_XIANXIA_RPG_MAP_EAST_RIDGE,
  H2_XIANXIA_RPG_MAP_WEST_RIDGE,
  H2_XIANXIA_RPG_MAP_LANTERN_ALTAR,
  H2_XIANXIA_RPG_MAP_WATER_MARKET,
  H2_XIANXIA_RPG_MAP_OLD_CITY,
  H2_XIANXIA_RPG_MAP_WEN_TOWER,
  H2_XIANXIA_RPG_MAP_HERB_HUT,
  H2_XIANXIA_RPG_MAP_HOT_SPRING,
  H2_XIANXIA_RPG_MAP_FROZEN_CREEK,
  H2_XIANXIA_RPG_MAP_MIRROR_SHORE,
  H2_XIANXIA_RPG_MAP_MIRROR_HALL,
  H2_XIANXIA_RPG_MAP_COUNT,
} h2_xianxia_rpg_map_t;

/** Story progress across chapters, in play order; each value names the
 * current objective, so stage >= X means the story has passed X. */
typedef enum h2_xianxia_rpg_stage {
  H2_XIANXIA_RPG_STAGE_MEET_ELDER = 0,
  H2_XIANXIA_RPG_STAGE_SEEK_DAOIST,
  H2_XIANXIA_RPG_STAGE_TO_SHRINE,
  /** The boss is sealed; report back to the elder. */
  H2_XIANXIA_RPG_STAGE_RETURN,
  H2_XIANXIA_RPG_STAGE_CLEARED,
  /* Chapter 2. */
  H2_XIANXIA_RPG_STAGE_C2_ARRIVE,
  H2_XIANXIA_RPG_STAGE_C2_FIND_JIANGLI,
  H2_XIANXIA_RPG_STAGE_C2_TO_REEDS,
  H2_XIANXIA_RPG_STAGE_C2_TO_WRECK,
  H2_XIANXIA_RPG_STAGE_C2_WRECK,
  /** The wraith is gone; report back at the dock. */
  H2_XIANXIA_RPG_STAGE_C2_RETURN,
  H2_XIANXIA_RPG_STAGE_C2_CLEARED,
  /* Chapter 3. Published stage values above remain stable. */
  H2_XIANXIA_RPG_STAGE_C3_ASCEND,
  H2_XIANXIA_RPG_STAGE_C3_ARRAY,
  H2_XIANXIA_RPG_STAGE_C3_HALL,
  H2_XIANXIA_RPG_STAGE_C3_RETURN,
  H2_XIANXIA_RPG_STAGE_C3_CLEARED,
  H2_XIANXIA_RPG_STAGE_C4_HOME,
  H2_XIANXIA_RPG_STAGE_C4_ESCORT,
  H2_XIANXIA_RPG_STAGE_C4_GUARD,
  H2_XIANXIA_RPG_STAGE_C4_RETURN,
  H2_XIANXIA_RPG_STAGE_C4_CLEARED,
  H2_XIANXIA_RPG_STAGE_C5_ARRIVE,
  H2_XIANXIA_RPG_STAGE_C5_PAGE,
  H2_XIANXIA_RPG_STAGE_C5_TOWER,
  H2_XIANXIA_RPG_STAGE_C5_CHOICE,
  H2_XIANXIA_RPG_STAGE_C5_DUEL,
  H2_XIANXIA_RPG_STAGE_C5_RESCUE,
  H2_XIANXIA_RPG_STAGE_C5_ESCORT,
  H2_XIANXIA_RPG_STAGE_C5_RETURN,
  H2_XIANXIA_RPG_STAGE_C5_CLEARED,
  H2_XIANXIA_RPG_STAGE_C6_ARRIVE,
  H2_XIANXIA_RPG_STAGE_C6_HERBS,
  H2_XIANXIA_RPG_STAGE_C6_READY,
  H2_XIANXIA_RPG_STAGE_C6_RITUAL,
  H2_XIANXIA_RPG_STAGE_C6_RECOVERED,
  H2_XIANXIA_RPG_STAGE_C6_CLEARED,
  H2_XIANXIA_RPG_STAGE_C7_LAMP,
  H2_XIANXIA_RPG_STAGE_C7_TREATY,
  H2_XIANXIA_RPG_STAGE_C7_QUESTION1,
  H2_XIANXIA_RPG_STAGE_C7_QUESTION2,
  H2_XIANXIA_RPG_STAGE_C7_QUESTION3,
  H2_XIANXIA_RPG_STAGE_C7_DUEL,
  H2_XIANXIA_RPG_STAGE_C7_REVEAL,
  H2_XIANXIA_RPG_STAGE_C7_CLEARED,
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
  /* Chapter 2. */
  H2_XIANXIA_RPG_ACTOR_JIANGLI,
  H2_XIANXIA_RPG_ACTOR_FERRYMASTER,
  H2_XIANXIA_RPG_ACTOR_TEAWIFE,
  H2_XIANXIA_RPG_ACTOR_TRAVELER,
  H2_XIANXIA_RPG_ACTOR_FISHER,
  /** The mist ghost guarding the wreck, fought where it stands. */
  H2_XIANXIA_RPG_ACTOR_WRECK_GUARD,
  H2_XIANXIA_RPG_ACTOR_WRAITH,
  H2_XIANXIA_RPG_ACTOR_SHIYAN,
  H2_XIANXIA_RPG_ACTOR_WENZHOU,
  H2_XIANXIA_RPG_ACTOR_SWORD_SOUL,
  H2_XIANXIA_RPG_ACTOR_WEN_GRANDMA,
  H2_XIANXIA_RPG_ACTOR_BOAT_DOCTOR,
  H2_XIANXIA_RPG_ACTOR_XUN,
  H2_XIANXIA_RPG_ACTOR_COUNT,
} h2_xianxia_rpg_actor_t;

typedef enum h2_xianxia_rpg_enemy {
  H2_XIANXIA_RPG_ENEMY_LEAF_SPRITE = 0,
  H2_XIANXIA_RPG_ENEMY_CIVET,
  H2_XIANXIA_RPG_ENEMY_WISP,
  H2_XIANXIA_RPG_ENEMY_GOLEM,
  H2_XIANXIA_RPG_ENEMY_DRAGON,
  /* Chapter 2. */
  H2_XIANXIA_RPG_ENEMY_FOG_GHOST,
  H2_XIANXIA_RPG_ENEMY_REED_OTTER,
  H2_XIANXIA_RPG_ENEMY_REED_SPRITE,
  H2_XIANXIA_RPG_ENEMY_WRECK_GUARD,
  H2_XIANXIA_RPG_ENEMY_FOG_WRAITH,
  H2_XIANXIA_RPG_ENEMY_RAIN_CROW,
  H2_XIANXIA_RPG_ENEMY_BROKEN_SWORD,
  H2_XIANXIA_RPG_ENEMY_SWORD_SOUL,
  H2_XIANXIA_RPG_ENEMY_INK_TIDE,
  H2_XIANXIA_RPG_ENEMY_WENZHOU,
  H2_XIANXIA_RPG_ENEMY_TIDE_OTTER,
  H2_XIANXIA_RPG_ENEMY_MIRROR,
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
  /** The companion acts between the hero and the enemy. */
  H2_XIANXIA_RPG_BATTLE_ALLY_ACT,
  /** A strike waits for the player to tap one of two shadows. */
  H2_XIANXIA_RPG_BATTLE_TARGET,
  /** A pill waits for a tap on either party status box. */
  H2_XIANXIA_RPG_BATTLE_HEAL_TARGET,
} h2_xianxia_rpg_battle_state_t;

/** What the companion does after the hero; shown ahead as her intent. */
typedef enum h2_xianxia_rpg_ally_action {
  H2_XIANXIA_RPG_ALLY_NONE = 0,
  /** Lifts her lantern and strips the enemy's veil. */
  H2_XIANXIA_RPG_ALLY_LANTERN,
  /** A light pole strike. */
  H2_XIANXIA_RPG_ALLY_POLE,
  /** Braces the guarding hero against the next blow. */
  H2_XIANXIA_RPG_ALLY_STEADY,
} h2_xianxia_rpg_ally_action_t;

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
  /* Chapter 2. */
  H2_XIANXIA_RPG_EFFECT_C2_BRIEFED,
  H2_XIANXIA_RPG_EFFECT_C2_TUTORIAL,
  H2_XIANXIA_RPG_EFFECT_C2_JOINED,
  H2_XIANXIA_RPG_EFFECT_C2_TEA,
  H2_XIANXIA_RPG_EFFECT_C2_REEDS_TIP,
  H2_XIANXIA_RPG_EFFECT_C2_LIGHT_POST1,
  H2_XIANXIA_RPG_EFFECT_C2_LIGHT_POST2,
  H2_XIANXIA_RPG_EFFECT_C2_FISHER,
  H2_XIANXIA_RPG_EFFECT_C2_ROPE,
  H2_XIANXIA_RPG_EFFECT_C2_BASKET,
  H2_XIANXIA_RPG_EFFECT_C2_FOUND_BOAT,
  H2_XIANXIA_RPG_EFFECT_C2_SAIL,
  H2_XIANXIA_RPG_EFFECT_C2_LIGHT_POST4,
  H2_XIANXIA_RPG_EFFECT_C2_GUARD_BATTLE,
  H2_XIANXIA_RPG_EFFECT_C2_WRAITH_BATTLE,
  H2_XIANXIA_RPG_EFFECT_C2_HOME,
  H2_XIANXIA_RPG_EFFECT_C3_BRIEF,
  H2_XIANXIA_RPG_EFFECT_C3_STELE1,
  H2_XIANXIA_RPG_EFFECT_C3_STELE2,
  H2_XIANXIA_RPG_EFFECT_C3_STELE3,
  H2_XIANXIA_RPG_EFFECT_C3_CAMP,
  H2_XIANXIA_RPG_EFFECT_C3_BATTLE,
  H2_XIANXIA_RPG_EFFECT_C3_REVEAL,
  H2_XIANXIA_RPG_EFFECT_C4_JOIN,
  H2_XIANXIA_RPG_EFFECT_C4_FIRE,
  H2_XIANXIA_RPG_EFFECT_C4_LIGHT1,
  H2_XIANXIA_RPG_EFFECT_C4_LIGHT2,
  H2_XIANXIA_RPG_EFFECT_C4_LIGHT3,
  H2_XIANXIA_RPG_EFFECT_C4_BATTLE,
  H2_XIANXIA_RPG_EFFECT_C4_HOME,
  H2_XIANXIA_RPG_EFFECT_C4_REPORT,
  H2_XIANXIA_RPG_EFFECT_C5_BRIEF,
  H2_XIANXIA_RPG_EFFECT_C5_QUESTION,
  H2_XIANXIA_RPG_EFFECT_C5_EVIDENCE,
  H2_XIANXIA_RPG_EFFECT_C5_DUEL,
  H2_XIANXIA_RPG_EFFECT_C5_END,
  H2_XIANXIA_RPG_EFFECT_C6_BRIEF,
  H2_XIANXIA_RPG_EFFECT_C6_RITUAL,
  H2_XIANXIA_RPG_EFFECT_C7_FEEDBACK,
  H2_XIANXIA_RPG_EFFECT_C7_BATTLE,
  H2_XIANXIA_RPG_EFFECT_COUNT,
} h2_xianxia_rpg_effect_t;

/** Story flags kept in the save. Chapter 1 stores only the low byte. */
#define H2_XIANXIA_RPG_FLAG_PILLS_GIVEN 0x01u
/** Chapter 2 began from a cleared chapter 1 rather than the defaults. */
#define H2_XIANXIA_RPG_FLAG_C1_CLEARED (1u << 8)
/* Chapter-2 clues; carried to later chapters. */
#define H2_XIANXIA_RPG_FLAG_C2_RUMOR (1u << 9)
#define H2_XIANXIA_RPG_FLAG_C2_FISHER (1u << 10)
#define H2_XIANXIA_RPG_FLAG_C2_ROPE (1u << 11)
#define H2_XIANXIA_RPG_FLAG_C2_HAND_LIGHT (1u << 12)
/* Chapter-2 map state. */
#define H2_XIANXIA_RPG_FLAG_C2_BASKET (1u << 13)
#define H2_XIANXIA_RPG_FLAG_C2_TRUST (1u << 14)
#define H2_XIANXIA_RPG_FLAG_C2_REEDS_TIP (1u << 15)
#define H2_XIANXIA_RPG_FLAG_C2_POST1 (1u << 16)
#define H2_XIANXIA_RPG_FLAG_C2_POST2 (1u << 17)
#define H2_XIANXIA_RPG_FLAG_C2_BOAT (1u << 18)
#define H2_XIANXIA_RPG_FLAG_C2_POST4 (1u << 19)
#define H2_XIANXIA_RPG_FLAG_C2_GUARD (1u << 20)
/** Chapter-3 local map bits reuse the low bits discarded at chapter entry.
 * Three posts each have two clockwise direction bits (N,E,S,W). */
#define H2_XIANXIA_RPG_C3_POST_BITS 0x3Fu
#define H2_XIANXIA_RPG_C3_POST_INITIAL 0x18u
#define H2_XIANXIA_RPG_FLAG_C3_OPEN (1u << 6)
#define H2_XIANXIA_RPG_FLAG_C3_CAMP (1u << 7)
#define H2_XIANXIA_RPG_FLAG_C3_STELE1 (1u << 21)
#define H2_XIANXIA_RPG_FLAG_C3_STELE2 (1u << 22)
#define H2_XIANXIA_RPG_FLAG_C3_STELE3 (1u << 23)
#define H2_XIANXIA_RPG_FLAG_C3_WENZHOU (1u << 24)
#define H2_XIANXIA_RPG_FLAG_C3_GLYPH (1u << 25)
/** Chapter-4 local bits; party health is restored at saved checkpoints,
 * like the hero's, and its maximum derives from the saved hero level. */
#define H2_XIANXIA_RPG_C4_LAMPS 7u
#define H2_XIANXIA_RPG_FLAG_C4_FIRE (1u << 3)
#define H2_XIANXIA_RPG_FLAG_C4_COVER (1u << 4)
#define H2_XIANXIA_RPG_FLAG_C4_JOINED (1u << 26)
#define H2_XIANXIA_RPG_FLAG_C4_FAINT (1u << 27)
#define H2_XIANXIA_RPG_FLAG_C4_GLYPH (1u << 28)
#define H2_XIANXIA_RPG_FLAG_C4_BOOK (1u << 29)
/** Chapter 5 local tide/page state and a four-bit remaining rescue budget.
 * The two high carry bits record the answer and the rescue/tide glyph. */
#define H2_XIANXIA_RPG_FLAG_C5_HIGH (1u << 0)
#define H2_XIANXIA_RPG_FLAG_C5_PAGE (1u << 1)
#define H2_XIANXIA_RPG_C5_STEPS_SHIFT 4u
#define H2_XIANXIA_RPG_C5_STEPS_MASK (15u << 4)
#define H2_XIANXIA_RPG_FLAG_C5_RETURN_LIGHT (1u << 30)
#define H2_XIANXIA_RPG_FLAG_C5_SAVED (1u << 31)
/** Chapter-6 local low byte: temperature 0..8, steady streak 0..5.
 * The independent fact word never aliases a published flag. Its low byte
 * also occupies ONLY the sixth clear slot's previously reserved byte. */
#define H2_XIANXIA_RPG_C6_METER_MASK 15u
#define H2_XIANXIA_RPG_C6_STREAK_SHIFT 4u
#define H2_XIANXIA_RPG_FACT_C6_HOT (1u << 0)
#define H2_XIANXIA_RPG_FACT_C6_COLD (1u << 1)
#define H2_XIANXIA_RPG_FACT_C6_FIRST_COLD (1u << 2)
#define H2_XIANXIA_RPG_FACT_C6_HEALED (1u << 3)
#define H2_XIANXIA_RPG_FACT_C6_CHOSEN (1u << 4)
/** Chapter 7 local state; facts 0..4 keep chapter 6 unchanged. */
#define H2_XIANXIA_RPG_FLAG_C7_PAST 1u
#define H2_XIANXIA_RPG_FLAG_C7_FEEDBACK 2u
#define H2_XIANXIA_RPG_FACT_C7_LAMP (1u << 5)
#define H2_XIANXIA_RPG_FACT_C7_WATER (1u << 6)
#define H2_XIANXIA_RPG_FACT_C7_TREATY (1u << 7)
#define H2_XIANXIA_RPG_C7_ANSWERS_SHIFT 8u
#define H2_XIANXIA_RPG_FACT_C7_MIRROR (1u << 11)
/** Flags a chapter hands to the next one. */
#define H2_XIANXIA_RPG_FLAG_CARRY                                      \
  (H2_XIANXIA_RPG_FLAG_C1_CLEARED | H2_XIANXIA_RPG_FLAG_C2_RUMOR |     \
   H2_XIANXIA_RPG_FLAG_C2_FISHER | H2_XIANXIA_RPG_FLAG_C2_ROPE |       \
   H2_XIANXIA_RPG_FLAG_C2_HAND_LIGHT | H2_XIANXIA_RPG_FLAG_C3_STELE1 | \
   H2_XIANXIA_RPG_FLAG_C3_STELE2 | H2_XIANXIA_RPG_FLAG_C3_STELE3 |     \
   H2_XIANXIA_RPG_FLAG_C3_WENZHOU | H2_XIANXIA_RPG_FLAG_C3_GLYPH |     \
   H2_XIANXIA_RPG_FLAG_C4_JOINED | H2_XIANXIA_RPG_FLAG_C4_FAINT |      \
   H2_XIANXIA_RPG_FLAG_C4_GLYPH | H2_XIANXIA_RPG_FLAG_C4_BOOK |        \
   H2_XIANXIA_RPG_FLAG_C5_RETURN_LIGHT | H2_XIANXIA_RPG_FLAG_C5_SAVED)

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
  /** Chapter screen: panel, a row per chapter, the restart button of an
   * unfinished chapter and the way back. */
  h2_xianxia_rpg_rect_t chapter_panel;
  h2_xianxia_rpg_rect_t chapter_rows[H2_XIANXIA_RPG_CHAPTER_ROWS];
  h2_xianxia_rpg_rect_t chapter_restart[H2_XIANXIA_RPG_CHAPTER_ROWS];
  h2_xianxia_rpg_rect_t chapter_back;
  /** Battle: picture, lower panel, its message box and command buttons. */
  h2_xianxia_rpg_rect_t stage, panel, message;
  h2_xianxia_rpg_rect_t commands[H2_XIANXIA_RPG_COMMANDS];
  /** Battle: the two shadows of a split boss, as tap targets. */
  h2_xianxia_rpg_rect_t shadows[2];
  /** Chapter 4: exploration actions and party/pill targets. */
  h2_xianxia_rpg_rect_t escort_actions[2], party[2];
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
  /** UTF-8 byte offset of the current small-screen dialogue page. */
  uint32_t page_offset;
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

/** Hero entering a chapter, from a cleared chapter or its defaults. */
typedef struct h2_xianxia_rpg_clear {
  uint32_t level, exp, pills, battles, turns, flags, trust;
} h2_xianxia_rpg_clear_t;

/** Chapter record: which chapters are cleared and the hero at each clear. */
typedef struct h2_xianxia_rpg_progress {
  /** Bit c - 1 is set when chapter c is cleared. */
  uint32_t cleared;
  h2_xianxia_rpg_clear_t chapters[H2_XIANXIA_RPG_CHAPTER_MAX];
  /** Preserve opaque reserved bytes in other chapter slots on re-encode. */
  uint8_t reserved[H2_XIANXIA_RPG_CHAPTER_MAX];
} h2_xianxia_rpg_progress_t;

/** Static description of a playable chapter, owned by the story table. */
typedef struct h2_xianxia_rpg_chapter_info {
  uint32_t number;
  h2_xianxia_rpg_stage_t first_stage, cleared_stage;
  h2_xianxia_rpg_map_t first_map;
  uint32_t map_count;
  h2_xianxia_rpg_map_t start_map, rest_map;
  int32_t start_x, start_y, rest_x, rest_y;
  /** Hero when the previous chapter has no clear record. */
  uint32_t default_level, default_pills;
  /** Script that opens the chapter; a h2_xianxia_rpg_script_id_t. */
  uint32_t intro_script;
  /** Short label ("第二章"), full name ("第二章 · 渡口迷雾"), the ending
   * card title, its two summary lines and the pointer to the next chapter. */
  const char* label;
  const char* name;
  const char* title;
  const char* ending[2];
  const char* next;
  /** Story so far, shown on the chapter card; NULL for the first chapter. */
  const char* recap;
} h2_xianxia_rpg_chapter_info_t;

typedef struct h2_xianxia_rpg_popup {
  bool active;
  bool on_enemy;
  bool heal;
  /** Non-enemy popup owner: 0 hero, 1 companion, 2 lantern. */
  uint8_t target;
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
  /** Heart shadow repeats the previous accepted command (opening: guard).
   * Sword art also remembers whether that command was a joint strike. */
  uint8_t mirror_command;
  bool mirror_joint, chosen_joint;
  /** Fixed fights may forbid fleeing without being a boss. */
  bool no_flee;
  /* Companion play: rapport lanterns, the enemy's veil, hindrances. */
  bool ally;
  uint32_t rapport;
  bool veiled;
  /** The hero's next attack is hindered. */
  bool entangled;
  /** Sword arts are locked for the hero's next command. */
  bool mp_sealed;
  /** The companion braces the hero this enemy turn. */
  bool steadied;
  /** The teaching fight: neither side falls before a joint strike. */
  bool tutorial, joint_landed;
  /** The current hero action is the joint strike. */
  bool joint;
  uint8_t ally_action; /**< h2_xianxia_rpg_ally_action_t of this turn */
  /** Boss pattern: actions taken, and phase two with two shadows. */
  uint32_t steps;
  bool phase_two, split;
  /** Hearing Rain's next stance: 0 falling rain, 1 eaves thrust, 2 return
   * wind. Set before the command, not after the player has committed. */
  uint8_t sword_stance;
  bool sword_counter;
  /** Three waves, each ends after three enemy actions or a defeated foe. */
  bool lamp_battle, lamp_guard, ally_mend, heal_ally;
  uint32_t lamp_hp, lamp_max, wave, wave_turn;
  /** 0 hero, 1 companion, 2 lantern; used only for enemy hits. */
  uint8_t hit_target;
  uint8_t real_shadow;
  /** Command waiting for a shadow to be chosen. */
  uint8_t pending;
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
  uint8_t kind; /**< 0 leaf, 1 petal, 2 firefly, 3 mist */
} h2_xianxia_rpg_particle_t;

/** A journey that Continue resumes. */
typedef struct h2_xianxia_rpg_save {
  uint32_t stage, level, exp, pills, flags, map, x, y;
  /** Battles won and turns taken, shown on the ending screen. */
  uint32_t battles, turns;
  /** Chapter (1 for the save0..save3 record), companion trust and the rest
   * point a defeat returns to. */
  uint32_t chapter, trust, rest_map, rest_x, rest_y;
  uint32_t facts;
} h2_xianxia_rpg_save_t;

/** Everything read from storage at start. */
typedef struct h2_xianxia_rpg_records {
  /** Chapter-1 journey in the save0..save3 keys shared with v0.1. */
  bool has_chapter1;
  h2_xianxia_rpg_save_t chapter1;
  /** Journey of a later chapter in the journey key. */
  bool has_journey;
  h2_xianxia_rpg_save_t journey;
  /** The journey was saved after the current chapter-1 record, so Continue
   * resumes it; false after v0.1 or a new chapter-1 run changed save0..3. */
  bool journey_latest;
  /** Chapter 3 uses its own key so released v0.2 can still continue its
   * chapter-2 journey after rollback. Its tag covers both older journeys. */
  bool has_journey3, journey3_latest, journey3_unreadable;
  h2_xianxia_rpg_save_t journey3;
  /** Separate chapter-4 record; tag includes the bytes of journey3 too. */
  bool has_journey4, journey4_latest, journey4_unreadable;
  h2_xianxia_rpg_save_t journey4;
  /** Chapter 5 tags all four predecessors without rewriting them. */
  bool has_journey5, journey5_latest, journey5_unreadable;
  h2_xianxia_rpg_save_t journey5;
  bool has_journey6, journey6_latest, journey6_unreadable;
  h2_xianxia_rpg_save_t journey6;
  bool has_journey7, journey7_latest, journey7_unreadable;
  h2_xianxia_rpg_save_t journey7;
  h2_xianxia_rpg_progress_t progress;
  /** Each blob is missing when neither its valid nor unreadable bit is set.
   * Read errors count as unreadable: they do not prove a record is absent. */
  bool has_progress;
  bool journey_unreadable, progress_unreadable;
  /** A record was present but invalid or could not be read. */
  bool unreadable;
} h2_xianxia_rpg_records_t;

/** Records to store after a step; each part is written only when set. */
typedef struct h2_xianxia_rpg_writes {
  bool chapter1;
  h2_xianxia_rpg_save_t chapter1_save;
  bool journey;
  h2_xianxia_rpg_save_t journey_save;
  bool journey3;
  h2_xianxia_rpg_save_t journey3_save;
  bool journey4;
  h2_xianxia_rpg_save_t journey4_save;
  bool journey5;
  h2_xianxia_rpg_save_t journey5_save;
  bool journey6;
  h2_xianxia_rpg_save_t journey6_save;
  bool journey7;
  h2_xianxia_rpg_save_t journey7_save;
  bool progress;
  h2_xianxia_rpg_progress_t progress_record;
} h2_xianxia_rpg_writes_t;

typedef struct h2_xianxia_rpg_game {
  int32_t width, height;
  float unit; /**< Scale relative to a 480-pixel short side. */
  h2_xianxia_rpg_layout_t layout;
  uint32_t rng;
  uint64_t time_ms;
  h2_xianxia_rpg_phase_t phase;
  uint64_t phase_started_ms;

  /* Title and chapter screen. */
  h2_xianxia_rpg_records_t records;
  /** Chapter the chapter card leads into, and whether it carries a clear. */
  uint32_t prologue_chapter;
  bool prologue_carry;

  /* Progress. */
  uint32_t chapter;
  h2_xianxia_rpg_stage_t stage;
  uint32_t flags;
  uint32_t facts;
  /** Companion trust, 0..3; each lantern starts battles with rapport. */
  uint32_t trust;
  h2_xianxia_rpg_hero_t hero;
  uint32_t ally_hp, ally_hp_max;
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
  /** The companion follows one tile behind once she joins. */
  bool ally_visible;
  int32_t ally_x, ally_y, ally_from_x, ally_from_y;
  h2_xianxia_rpg_dialogue_t dialogue;
  uint32_t banner_ms;
  /** Remaining time of the objective-update announcement. */
  uint32_t objective_ms;
  int32_t marker_x, marker_y;
  uint32_t marker_ms;

  h2_xianxia_rpg_battle_t battle;
  h2_xianxia_rpg_particle_t particles[H2_XIANXIA_RPG_MAX_PARTICLES];

  /** One-shot effect requests: the journey or the chapter record needs
   * saving. */
  bool save_pending, progress_pending;
} h2_xianxia_rpg_game_t;

/** Reset to the title screen. width/height must be positive; records may be
 * NULL. Invalid parts are ignored. A chapter-1 clear found only in the
 * save0..save3 record is copied into the chapter record, which is then due
 * for writing; the save0..save3 record itself is never rewritten here. */
void h2_xianxia_rpg_game_init(h2_xianxia_rpg_game_t* game, int32_t width,
                              int32_t height, uint32_t seed,
                              const h2_xianxia_rpg_records_t* records);

/** Feed one pointer edge at the current game time. Presses act on DOWN. */
void h2_xianxia_rpg_game_pointer(h2_xianxia_rpg_game_t* game,
                                 h2_xianxia_rpg_pointer_t kind, float x,
                                 float y);

/** Advance the simulation; large steps are split internally. */
void h2_xianxia_rpg_game_step(h2_xianxia_rpg_game_t* game, uint32_t dt_ms);

/** The journey Continue resumes: the one saved last, or NULL. */
const h2_xianxia_rpg_save_t* h2_xianxia_rpg_continue_save(
    const h2_xianxia_rpg_game_t* game);
/** A chapter's unfinished journey, which its chapter row resumes, or NULL. */
const h2_xianxia_rpg_save_t* h2_xianxia_rpg_unfinished(
    const h2_xianxia_rpg_game_t* game, uint32_t chapter);

/** Take the pending writes; false when none is pending. Chapter-1 journeys
 * go to the save0..save3 record, later chapters to the journey record. */
bool h2_xianxia_rpg_game_take_writes(h2_xianxia_rpg_game_t* game,
                                     h2_xianxia_rpg_writes_t* out);

/** Encode a chapter-1 save into the v0.1 words, or decode and validate
 * them; decoded saves are chapter 1 and revive where they stand. */
void h2_xianxia_rpg_save_pack(const h2_xianxia_rpg_save_t* save,
                              uint32_t words[H2_XIANXIA_RPG_SAVE_WORDS]);
bool h2_xianxia_rpg_save_unpack(const uint32_t words[H2_XIANXIA_RPG_SAVE_WORDS],
                                h2_xianxia_rpg_save_t* out_save);

/** Fingerprint of the save0..save3 words, never zero; present marks the
 * keys found. Zero stands for no chapter-1 record at all. */
uint32_t h2_xianxia_rpg_chapter1_tag(
    const uint32_t words[H2_XIANXIA_RPG_SAVE_WORDS], unsigned present_mask);

/** Encode a later-chapter journey with the chapter-1 tag current when it
 * was written, or decode and validate one. Decoding fails for corrupt data
 * and for chapters this build cannot play. */
void h2_xianxia_rpg_journey_encode(const h2_xianxia_rpg_save_t* save,
                                   uint32_t chapter1_tag,
                                   uint8_t out[H2_XIANXIA_RPG_JOURNEY_BYTES]);
bool h2_xianxia_rpg_journey_decode(const uint8_t* data, size_t size,
                                   h2_xianxia_rpg_save_t* out_save,
                                   uint32_t* out_chapter1_tag);

/** Fingerprint of the two older journey records, used only by journey3.
 * NULL/zero represents an absent chapter-2 record. */
uint32_t h2_xianxia_rpg_predecessor_tag(uint32_t chapter1_tag,
                                        const uint8_t* chapter2, size_t size);

/** Version 3 is confined to journey6. The first 28 bytes retain the v2
 * fields, followed by facts u32 and the checksum over all 32 body bytes. */
void h2_xianxia_rpg_journey6_encode(const h2_xianxia_rpg_save_t* save,
                                    uint32_t predecessor_tag,
                                    uint8_t out[H2_XIANXIA_RPG_JOURNEY6_BYTES]);
bool h2_xianxia_rpg_journey6_decode(const uint8_t* data, size_t size,
                                    h2_xianxia_rpg_save_t* out_save,
                                    uint32_t* out_tag);
/** Journey7 uses the same v3 layout in its own key; its tag includes the
 * exact journey6 bytes. No previously published codec changes. */
void h2_xianxia_rpg_journey7_encode(const h2_xianxia_rpg_save_t* save,
                                    uint32_t predecessor_tag,
                                    uint8_t out[H2_XIANXIA_RPG_JOURNEY7_BYTES]);
bool h2_xianxia_rpg_journey7_decode(const uint8_t* data, size_t size,
                                    h2_xianxia_rpg_save_t* out_save,
                                    uint32_t* out_tag);
/** Rapport capacity grows only after chapter-6 recovery. Trust remains
 * 0..3 so published completion-table decoders still accept the record. */
uint32_t h2_xianxia_rpg_rapport_max(const h2_xianxia_rpg_game_t* game);
/** Next ritual breath, -2 cold or +2 hot, visible before a command. */
int32_t h2_xianxia_rpg_c6_breath(const h2_xianxia_rpg_game_t* game);

/** Encode or decode the chapter record. */
void h2_xianxia_rpg_progress_encode(const h2_xianxia_rpg_progress_t* progress,
                                    uint8_t out[H2_XIANXIA_RPG_PROGRESS_BYTES]);
bool h2_xianxia_rpg_progress_decode(const uint8_t* data, size_t size,
                                    h2_xianxia_rpg_progress_t* out);

/* Static world data, owned by the story table. */

/** Map tile character at a cell; '#' outside the grid. */
char h2_xianxia_rpg_tile(h2_xianxia_rpg_map_t map, int32_t x, int32_t y);
/** Chapter owning a map or a stage. */
uint32_t h2_xianxia_rpg_map_chapter(h2_xianxia_rpg_map_t map);
uint32_t h2_xianxia_rpg_stage_chapter(h2_xianxia_rpg_stage_t stage);
/** A playable chapter, or NULL. */
const h2_xianxia_rpg_chapter_info_t* h2_xianxia_rpg_chapter_info(
    uint32_t chapter);
/** The companion fights alongside the hero at this progress. */
bool h2_xianxia_rpg_ally_joined(const h2_xianxia_rpg_game_t* game);
/** What the companion will do after the hero's next command, assuming the
 * hero does not guard. */
h2_xianxia_rpg_ally_action_t h2_xianxia_rpg_ally_intent(
    const h2_xianxia_rpg_game_t* game);
/** Whether the sword-art command is the joint strike now. */
bool h2_xianxia_rpg_joint_ready(const h2_xianxia_rpg_game_t* game);
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

/** Number of relit chapter-4 lamps, and the next enemy target (0 hero,
 * 1 companion, 2 lantern). These queries also drive the visible hints. */
uint32_t h2_xianxia_rpg_c4_lamps(uint32_t flags);
uint32_t h2_xianxia_rpg_lamp_target(const h2_xianxia_rpg_game_t* game);

/** Number of UTF-8 codepoints in a string. */
uint32_t h2_xianxia_rpg_utf8_length(const char* text);
/** Decode one codepoint; advances *text. Invalid bytes decode as U+FFFD. */
uint32_t h2_xianxia_rpg_utf8_next(const char** text);

#ifdef __cplusplus
}
#endif

#endif
