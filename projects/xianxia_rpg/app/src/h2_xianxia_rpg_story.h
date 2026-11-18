#ifndef H2_XIANXIA_RPG_STORY_H
#define H2_XIANXIA_RPG_STORY_H

/* Static world and story tables read by the game model and the renderer.
 * Chapter 1 lives in h2_xianxia_rpg_story.c with the shared UI text; each
 * later chapter owns one source file (h2_xianxia_rpg_chapter2.c, ...) with
 * its maps, characters, scripts and rules, reached through the
 * h2_xianxia_rpg_c<N>_* functions below. */

#include "h2_xianxia_rpg_game.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_xianxia_rpg_script_id {
  H2_XIANXIA_RPG_SCRIPT_INTRO = 0,
  H2_XIANXIA_RPG_SCRIPT_ELDER_BRIEF,
  H2_XIANXIA_RPG_SCRIPT_ELDER_WAIT,
  H2_XIANXIA_RPG_SCRIPT_ELDER_GO,
  H2_XIANXIA_RPG_SCRIPT_ELDER_REPORT,
  H2_XIANXIA_RPG_SCRIPT_HERBALIST_EARLY,
  H2_XIANXIA_RPG_SCRIPT_HERBALIST_GIVE,
  H2_XIANXIA_RPG_SCRIPT_HERBALIST_REFILL,
  H2_XIANXIA_RPG_SCRIPT_HERBALIST_ENOUGH,
  H2_XIANXIA_RPG_SCRIPT_HERBALIST_THANKS,
  H2_XIANXIA_RPG_SCRIPT_CHILD,
  H2_XIANXIA_RPG_SCRIPT_CHILD_THANKS,
  H2_XIANXIA_RPG_SCRIPT_DAOIST_EARLY,
  H2_XIANXIA_RPG_SCRIPT_DAOIST_TEACH,
  H2_XIANXIA_RPG_SCRIPT_DAOIST_LATER,
  H2_XIANXIA_RPG_SCRIPT_DAOIST_RETURN,
  H2_XIANXIA_RPG_SCRIPT_DRAGON,
  H2_XIANXIA_RPG_SCRIPT_WELL,
  H2_XIANXIA_RPG_SCRIPT_SPRING,
  H2_XIANXIA_RPG_SCRIPT_BARRIER,
  H2_XIANXIA_RPG_SCRIPT_VICTORY,
  H2_XIANXIA_RPG_SCRIPT_REVIVE,
  H2_XIANXIA_RPG_SCRIPT_REVIVE_BOSS,
  /* Chapter 2, owned by h2_xianxia_rpg_chapter2.c. */
  H2_XIANXIA_RPG_SCRIPT_C2_FIRST,
  H2_XIANXIA_RPG_SCRIPT_C2_INTRO = H2_XIANXIA_RPG_SCRIPT_C2_FIRST,
  H2_XIANXIA_RPG_SCRIPT_C2_MASTER_BRIEF,
  H2_XIANXIA_RPG_SCRIPT_C2_MASTER_WAIT,
  H2_XIANXIA_RPG_SCRIPT_C2_MASTER_LATER,
  H2_XIANXIA_RPG_SCRIPT_C2_MASTER_REPORT,
  H2_XIANXIA_RPG_SCRIPT_C2_JIANGLI_MEET,
  H2_XIANXIA_RPG_SCRIPT_C2_JIANGLI_JOIN,
  H2_XIANXIA_RPG_SCRIPT_C2_TEA_FIRST,
  H2_XIANXIA_RPG_SCRIPT_C2_TEA_REST,
  H2_XIANXIA_RPG_SCRIPT_C2_TEA_CLEAR,
  H2_XIANXIA_RPG_SCRIPT_C2_TRAVELER,
  H2_XIANXIA_RPG_SCRIPT_C2_TRAVELER_CLEAR,
  H2_XIANXIA_RPG_SCRIPT_C2_SIGN,
  H2_XIANXIA_RPG_SCRIPT_C2_DOCK_CLOSED,
  H2_XIANXIA_RPG_SCRIPT_C2_REEDS_TIP,
  H2_XIANXIA_RPG_SCRIPT_C2_FOG_WALL,
  H2_XIANXIA_RPG_SCRIPT_C2_POST1,
  H2_XIANXIA_RPG_SCRIPT_C2_POST2,
  H2_XIANXIA_RPG_SCRIPT_C2_POST_REST,
  H2_XIANXIA_RPG_SCRIPT_C2_FISHER,
  H2_XIANXIA_RPG_SCRIPT_C2_FISHER_LATER,
  H2_XIANXIA_RPG_SCRIPT_C2_ROPE_C1,
  H2_XIANXIA_RPG_SCRIPT_C2_ROPE,
  H2_XIANXIA_RPG_SCRIPT_C2_ROPE_LATER,
  H2_XIANXIA_RPG_SCRIPT_C2_TRUST,
  H2_XIANXIA_RPG_SCRIPT_C2_BASKET,
  H2_XIANXIA_RPG_SCRIPT_C2_BASKET_EMPTY,
  H2_XIANXIA_RPG_SCRIPT_C2_FOUND_BOAT,
  H2_XIANXIA_RPG_SCRIPT_C2_SAIL,
  H2_XIANXIA_RPG_SCRIPT_C2_WRECK_LAND,
  H2_XIANXIA_RPG_SCRIPT_C2_GUARD,
  H2_XIANXIA_RPG_SCRIPT_C2_GUARD_DOWN,
  H2_XIANXIA_RPG_SCRIPT_C2_WRAITH,
  H2_XIANXIA_RPG_SCRIPT_C2_RELIT,
  H2_XIANXIA_RPG_SCRIPT_C2_REVIVE_BOSS,
  H2_XIANXIA_RPG_SCRIPT_C2_BOAT_STAY,
  H2_XIANXIA_RPG_SCRIPT_C2_SAIL_HOME,
  H2_XIANXIA_RPG_SCRIPT_C2_LAMP_LIT,
  H2_XIANXIA_RPG_SCRIPT_C2_BERTH,
  H2_XIANXIA_RPG_SCRIPT_COUNT,
} h2_xianxia_rpg_script_id_t;

typedef enum h2_xianxia_rpg_text_id {
  H2_XIANXIA_RPG_TEXT_TITLE = 0,
  H2_XIANXIA_RPG_TEXT_SUBTITLE,
  H2_XIANXIA_RPG_TEXT_CHAPTER,
  H2_XIANXIA_RPG_TEXT_NEW_GAME,
  H2_XIANXIA_RPG_TEXT_CONTINUE,
  H2_XIANXIA_RPG_TEXT_TITLE_HINT,
  H2_XIANXIA_RPG_TEXT_HP,
  H2_XIANXIA_RPG_TEXT_MP,
  H2_XIANXIA_RPG_TEXT_PILLS,
  H2_XIANXIA_RPG_TEXT_LEVEL,
  H2_XIANXIA_RPG_TEXT_OBJECTIVE,
  H2_XIANXIA_RPG_TEXT_TAP_NEXT,
  H2_XIANXIA_RPG_TEXT_TIPS,
  H2_XIANXIA_RPG_TEXT_OBJECTIVE_UPDATE,
  H2_XIANXIA_RPG_TEXT_APPEARS,
  H2_XIANXIA_RPG_TEXT_BOSS_APPEARS,
  H2_XIANXIA_RPG_TEXT_CHOOSE,
  H2_XIANXIA_RPG_TEXT_ATTACK,
  H2_XIANXIA_RPG_TEXT_CRITICAL,
  H2_XIANXIA_RPG_TEXT_SKILL,
  H2_XIANXIA_RPG_TEXT_NO_SKILL,
  H2_XIANXIA_RPG_TEXT_NO_MP,
  H2_XIANXIA_RPG_TEXT_PILL,
  H2_XIANXIA_RPG_TEXT_NO_PILLS,
  H2_XIANXIA_RPG_TEXT_GUARD,
  H2_XIANXIA_RPG_TEXT_FLED,
  H2_XIANXIA_RPG_TEXT_FLEE_FAILED,
  H2_XIANXIA_RPG_TEXT_NO_FLEE,
  H2_XIANXIA_RPG_TEXT_ENEMY_ATTACK,
  H2_XIANXIA_RPG_TEXT_ENEMY_SPECIAL,
  H2_XIANXIA_RPG_TEXT_BOSS_TAIL,
  H2_XIANXIA_RPG_TEXT_GUARDED,
  H2_XIANXIA_RPG_TEXT_CHARGE,
  H2_XIANXIA_RPG_TEXT_ENRAGE,
  H2_XIANXIA_RPG_TEXT_VICTORY,
  H2_XIANXIA_RPG_TEXT_DROP,
  H2_XIANXIA_RPG_TEXT_LEVEL_UP,
  H2_XIANXIA_RPG_TEXT_DEFEAT,
  H2_XIANXIA_RPG_TEXT_ENDING_TITLE,
  H2_XIANXIA_RPG_TEXT_ENDING_1,
  H2_XIANXIA_RPG_TEXT_ENDING_2,
  H2_XIANXIA_RPG_TEXT_ENDING_NEXT,
  H2_XIANXIA_RPG_TEXT_ENDING_STATS,
  H2_XIANXIA_RPG_TEXT_ENDING_HINT,
  /* Chapters, title and records. */
  H2_XIANXIA_RPG_TEXT_ENDING_ONWARD,
  H2_XIANXIA_RPG_TEXT_CHAPTERS,
  H2_XIANXIA_RPG_TEXT_CHAPTERS_HEAD,
  H2_XIANXIA_RPG_TEXT_CONTINUE_AT,
  H2_XIANXIA_RPG_TEXT_BACK,
  H2_XIANXIA_RPG_TEXT_RESTART,
  H2_XIANXIA_RPG_TEXT_STATUS_CLEARED,
  H2_XIANXIA_RPG_TEXT_STATUS_PLAYING,
  H2_XIANXIA_RPG_TEXT_STATUS_OPEN,
  H2_XIANXIA_RPG_TEXT_STATUS_SOON,
  H2_XIANXIA_RPG_TEXT_UPCOMING,
  H2_XIANXIA_RPG_TEXT_UNREADABLE,
  H2_XIANXIA_RPG_TEXT_RECAP_HEAD,
  H2_XIANXIA_RPG_TEXT_CARRY,
  H2_XIANXIA_RPG_TEXT_DEFAULTS,
  H2_XIANXIA_RPG_TEXT_PROLOGUE_HINT,
  /* Companion battles. */
  H2_XIANXIA_RPG_TEXT_JOINT,
  H2_XIANXIA_RPG_TEXT_RAPPORT,
  H2_XIANXIA_RPG_TEXT_INTENT,
  H2_XIANXIA_RPG_TEXT_INTENT_LANTERN,
  H2_XIANXIA_RPG_TEXT_INTENT_POLE,
  H2_XIANXIA_RPG_TEXT_INTENT_STEADY,
  H2_XIANXIA_RPG_TEXT_APPEARS_VEILED,
  H2_XIANXIA_RPG_TEXT_VEILED_HIT,
  H2_XIANXIA_RPG_TEXT_VEILED_SKILL,
  H2_XIANXIA_RPG_TEXT_HINDERED,
  H2_XIANXIA_RPG_TEXT_JOINT_STRIKE,
  H2_XIANXIA_RPG_TEXT_JOINT_BREAK,
  H2_XIANXIA_RPG_TEXT_ALLY_LANTERN,
  H2_XIANXIA_RPG_TEXT_ALLY_POLE,
  H2_XIANXIA_RPG_TEXT_ALLY_STEADY,
  H2_XIANXIA_RPG_TEXT_RAPPORT_UP,
  H2_XIANXIA_RPG_TEXT_ENEMY_VEIL,
  H2_XIANXIA_RPG_TEXT_ENEMY_DOUBLE,
  H2_XIANXIA_RPG_TEXT_ENEMY_ENTANGLE,
  H2_XIANXIA_RPG_TEXT_WRAITH_VEIL,
  H2_XIANXIA_RPG_TEXT_WRAITH_ATTACK,
  H2_XIANXIA_RPG_TEXT_WRAITH_CHARGE,
  H2_XIANXIA_RPG_TEXT_WRAITH_LOCK,
  H2_XIANXIA_RPG_TEXT_WRAITH_SPLIT,
  H2_XIANXIA_RPG_TEXT_WRAITH_SHIFT,
  H2_XIANXIA_RPG_TEXT_WRONG_SHADOW,
  H2_XIANXIA_RPG_TEXT_SEALED,
  H2_XIANXIA_RPG_TEXT_TUTORIAL_HOLD,
  H2_XIANXIA_RPG_TEXT_HINT_VEILED,
  H2_XIANXIA_RPG_TEXT_HINT_JOINT,
  H2_XIANXIA_RPG_TEXT_HINT_CHARGE,
  H2_XIANXIA_RPG_TEXT_HINT_SEALED,
  H2_XIANXIA_RPG_TEXT_HINT_TARGET,
  H2_XIANXIA_RPG_TEXT_HINT_SPLIT,
  H2_XIANXIA_RPG_TEXT_HINT_TUTORIAL1,
  H2_XIANXIA_RPG_TEXT_HINT_TUTORIAL2,
  H2_XIANXIA_RPG_TEXT_HINT_TUTORIAL3,
  H2_XIANXIA_RPG_TEXT_COUNT,
} h2_xianxia_rpg_text_id_t;

/** UI and battle strings; formats use ASCII placeholders only. */
extern const char* const h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_COUNT];

const h2_xianxia_rpg_script_t* h2_xianxia_rpg_script(
    h2_xianxia_rpg_script_id_t id);

/** Script an actor says at the current progress, or SCRIPT_COUNT. */
h2_xianxia_rpg_script_id_t h2_xianxia_rpg_talk_script(
    const h2_xianxia_rpg_game_t* game, h2_xianxia_rpg_actor_t actor);

/** Whether tapping a map cell interacts with something there, and the
 * script it starts (SCRIPT_COUNT when nothing happens). */
bool h2_xianxia_rpg_tile_interactive(const h2_xianxia_rpg_game_t* game,
                                     int32_t x, int32_t y);
h2_xianxia_rpg_script_id_t h2_xianxia_rpg_tile_script(
    const h2_xianxia_rpg_game_t* game, int32_t x, int32_t y);

/** Map transition when standing on an exit cell. */
bool h2_xianxia_rpg_exit_at(h2_xianxia_rpg_map_t map, int32_t x, int32_t y,
                            h2_xianxia_rpg_map_t* to_map, int32_t* to_x,
                            int32_t* to_y);
/** Script explaining why an exit is still closed, or SCRIPT_COUNT. */
h2_xianxia_rpg_script_id_t h2_xianxia_rpg_exit_closed(
    const h2_xianxia_rpg_game_t* game, h2_xianxia_rpg_map_t map);
/** Script played when entering a map, or SCRIPT_COUNT. */
h2_xianxia_rpg_script_id_t h2_xianxia_rpg_enter_script(
    const h2_xianxia_rpg_game_t* game, h2_xianxia_rpg_map_t map);

/** Enemies that may ambush the hero on a map; returns the count. */
size_t h2_xianxia_rpg_encounters(
    h2_xianxia_rpg_map_t map,
    h2_xianxia_rpg_enemy_t out[H2_XIANXIA_RPG_ENCOUNTER_MAX]);
/** Whether a step onto a cell counts toward the next ambush. */
bool h2_xianxia_rpg_ambush_cell(const h2_xianxia_rpg_game_t* game,
                                h2_xianxia_rpg_map_t map, int32_t x, int32_t y);
/** Cells pathfinding avoids when another way exists (ambush ground). */
bool h2_xianxia_rpg_costly_cell(h2_xianxia_rpg_map_t map, int32_t x, int32_t y);

/* Chapter 2 tables, owned by h2_xianxia_rpg_chapter2.c. */
char h2_xianxia_rpg_c2_tile(h2_xianxia_rpg_map_t map, int32_t x, int32_t y);
const char* h2_xianxia_rpg_c2_map_name(h2_xianxia_rpg_map_t map);
const h2_xianxia_rpg_script_t* h2_xianxia_rpg_c2_script(
    h2_xianxia_rpg_script_id_t id);
size_t h2_xianxia_rpg_c2_npcs(
    const h2_xianxia_rpg_game_t* game, h2_xianxia_rpg_map_t map,
    h2_xianxia_rpg_npc_t out[H2_XIANXIA_RPG_MAX_NPCS]);
h2_xianxia_rpg_script_id_t h2_xianxia_rpg_c2_talk_script(
    const h2_xianxia_rpg_game_t* game, h2_xianxia_rpg_actor_t actor);
bool h2_xianxia_rpg_c2_walkable(const h2_xianxia_rpg_game_t* game, char tile);
bool h2_xianxia_rpg_c2_tile_interactive(const h2_xianxia_rpg_game_t* game,
                                        char tile);
h2_xianxia_rpg_script_id_t h2_xianxia_rpg_c2_tile_script(
    const h2_xianxia_rpg_game_t* game, char tile);
const h2_xianxia_rpg_chapter_info_t* h2_xianxia_rpg_c2_info(void);
size_t h2_xianxia_rpg_c2_strings(const char** out, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
