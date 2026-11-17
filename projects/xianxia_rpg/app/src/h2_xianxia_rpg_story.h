#ifndef H2_XIANXIA_RPG_STORY_H
#define H2_XIANXIA_RPG_STORY_H

/* Static world and story tables read by the game model and the renderer. */

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
  H2_XIANXIA_RPG_TEXT_COUNT,
} h2_xianxia_rpg_text_id_t;

/** UI and battle strings; formats use ASCII placeholders only. */
extern const char* const h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_COUNT];

const h2_xianxia_rpg_script_t* h2_xianxia_rpg_script(
    h2_xianxia_rpg_script_id_t id);

/** Script an actor says at the current progress, or SCRIPT_COUNT. */
h2_xianxia_rpg_script_id_t h2_xianxia_rpg_talk_script(
    const h2_xianxia_rpg_game_t* game, h2_xianxia_rpg_actor_t actor);

/** Map transition when standing on an exit cell. */
bool h2_xianxia_rpg_exit_at(h2_xianxia_rpg_map_t map, int32_t x, int32_t y,
                            h2_xianxia_rpg_map_t* to_map, int32_t* to_x,
                            int32_t* to_y);

/** Enemies that may ambush the hero on a map; returns the count (0..2). */
size_t h2_xianxia_rpg_encounters(h2_xianxia_rpg_map_t map,
                                 h2_xianxia_rpg_enemy_t out[2]);

#ifdef __cplusplus
}
#endif

#endif
