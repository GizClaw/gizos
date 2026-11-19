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
  H2_XIANXIA_RPG_SCRIPT_C3_FIRST,
  H2_XIANXIA_RPG_SCRIPT_C3_INTRO = H2_XIANXIA_RPG_SCRIPT_C3_FIRST,
  H2_XIANXIA_RPG_SCRIPT_C3_PAVILION,
  H2_XIANXIA_RPG_SCRIPT_C3_LETTER,
  H2_XIANXIA_RPG_SCRIPT_C3_CUT,
  H2_XIANXIA_RPG_SCRIPT_C3_CUT_KNOWN,
  H2_XIANXIA_RPG_SCRIPT_C3_COURT,
  H2_XIANXIA_RPG_SCRIPT_C3_CENTER,
  H2_XIANXIA_RPG_SCRIPT_C3_BARRIER,
  H2_XIANXIA_RPG_SCRIPT_C3_OPEN,
  H2_XIANXIA_RPG_SCRIPT_C3_STELE1,
  H2_XIANXIA_RPG_SCRIPT_C3_STELE2,
  H2_XIANXIA_RPG_SCRIPT_C3_STELE3,
  H2_XIANXIA_RPG_SCRIPT_C3_HALL,
  H2_XIANXIA_RPG_SCRIPT_C3_REST,
  H2_XIANXIA_RPG_SCRIPT_C3_SHIYAN,
  H2_XIANXIA_RPG_SCRIPT_C3_BOSS,
  H2_XIANXIA_RPG_SCRIPT_C3_QUIET,
  H2_XIANXIA_RPG_SCRIPT_C3_REVEAL,
  H2_XIANXIA_RPG_SCRIPT_C3_WENZHOU,
  H2_XIANXIA_RPG_SCRIPT_C3_REPORT,
  H2_XIANXIA_RPG_SCRIPT_C3_REVIVE,
  H2_XIANXIA_RPG_SCRIPT_C4_FIRST,
  H2_XIANXIA_RPG_SCRIPT_C4_INTRO = H2_XIANXIA_RPG_SCRIPT_C4_FIRST,
  H2_XIANXIA_RPG_SCRIPT_C4_BRIEF,
  H2_XIANXIA_RPG_SCRIPT_C4_WAIT,
  H2_XIANXIA_RPG_SCRIPT_C4_FIRE,
  H2_XIANXIA_RPG_SCRIPT_C4_NEED_FIRE,
  H2_XIANXIA_RPG_SCRIPT_C4_HELP,
  H2_XIANXIA_RPG_SCRIPT_C4_COVER,
  H2_XIANXIA_RPG_SCRIPT_C4_SCOUT,
  H2_XIANXIA_RPG_SCRIPT_C4_OUT,
  H2_XIANXIA_RPG_SCRIPT_C4_SAFE,
  H2_XIANXIA_RPG_SCRIPT_C4_LAMP1,
  H2_XIANXIA_RPG_SCRIPT_C4_LAMP2,
  H2_XIANXIA_RPG_SCRIPT_C4_LAMP3,
  H2_XIANXIA_RPG_SCRIPT_C4_REST,
  H2_XIANXIA_RPG_SCRIPT_C4_BATTLE,
  H2_XIANXIA_RPG_SCRIPT_C4_REVIVE,
  H2_XIANXIA_RPG_SCRIPT_C4_HOME,
  H2_XIANXIA_RPG_SCRIPT_C4_REPORT,
  H2_XIANXIA_RPG_SCRIPT_C4_RUMOR,
  H2_XIANXIA_RPG_SCRIPT_C4_DEPART,
  H2_XIANXIA_RPG_SCRIPT_C4_HERB,
  H2_XIANXIA_RPG_SCRIPT_C4_HERB_CLEAR,
  H2_XIANXIA_RPG_SCRIPT_C4_CHILD,
  H2_XIANXIA_RPG_SCRIPT_C4_CHILD_CLEAR,
  H2_XIANXIA_RPG_SCRIPT_C5_FIRST,
  H2_XIANXIA_RPG_SCRIPT_C5_INTRO = H2_XIANXIA_RPG_SCRIPT_C5_FIRST,
  H2_XIANXIA_RPG_SCRIPT_C5_BRIEF,
  H2_XIANXIA_RPG_SCRIPT_C5_HELP,
  H2_XIANXIA_RPG_SCRIPT_C5_LOW,
  H2_XIANXIA_RPG_SCRIPT_C5_HIGH,
  H2_XIANXIA_RPG_SCRIPT_C5_PAGE,
  H2_XIANXIA_RPG_SCRIPT_C5_NEED_PAGE,
  H2_XIANXIA_RPG_SCRIPT_C5_QUESTION,
  H2_XIANXIA_RPG_SCRIPT_C5_TAKE,
  H2_XIANXIA_RPG_SCRIPT_C5_RETURN_LIGHT,
  H2_XIANXIA_RPG_SCRIPT_C5_EVIDENCE,
  H2_XIANXIA_RPG_SCRIPT_C5_DUEL,
  H2_XIANXIA_RPG_SCRIPT_C5_FLOOD,
  H2_XIANXIA_RPG_SCRIPT_C5_PICKUP,
  H2_XIANXIA_RPG_SCRIPT_C5_RETRY,
  H2_XIANXIA_RPG_SCRIPT_C5_SAFE,
  H2_XIANXIA_RPG_SCRIPT_C5_REST,
  H2_XIANXIA_RPG_SCRIPT_C5_END,
  H2_XIANXIA_RPG_SCRIPT_C5_REVIVE,
  H2_XIANXIA_RPG_SCRIPT_C5_STAY,
  H2_XIANXIA_RPG_SCRIPT_C5_OTTER,
  H2_XIANXIA_RPG_SCRIPT_C6_FIRST,
  H2_XIANXIA_RPG_SCRIPT_C6_INTRO = H2_XIANXIA_RPG_SCRIPT_C6_FIRST,
  H2_XIANXIA_RPG_SCRIPT_C6_BRIEF,
  H2_XIANXIA_RPG_SCRIPT_C6_HELP,
  H2_XIANXIA_RPG_SCRIPT_C6_HOT,
  H2_XIANXIA_RPG_SCRIPT_C6_COLD,
  H2_XIANXIA_RPG_SCRIPT_C6_REST,
  H2_XIANXIA_RPG_SCRIPT_C6_FIRE_MOSS,
  H2_XIANXIA_RPG_SCRIPT_C6_ICE_SOUL,
  H2_XIANXIA_RPG_SCRIPT_C6_NEED_BALANCE,
  H2_XIANXIA_RPG_SCRIPT_C6_RETREAT,
  H2_XIANXIA_RPG_SCRIPT_C6_READY,
  H2_XIANXIA_RPG_SCRIPT_C6_RETRY,
  H2_XIANXIA_RPG_SCRIPT_C6_RECOVERED,
  H2_XIANXIA_RPG_SCRIPT_C6_END,
  H2_XIANXIA_RPG_SCRIPT_C6_BOTH,
  H2_XIANXIA_RPG_SCRIPT_C7_FIRST,
  H2_XIANXIA_RPG_SCRIPT_C7_INTRO = H2_XIANXIA_RPG_SCRIPT_C7_FIRST,
  H2_XIANXIA_RPG_SCRIPT_C7_HELP,
  H2_XIANXIA_RPG_SCRIPT_C7_PAST,
  H2_XIANXIA_RPG_SCRIPT_C7_NOW,
  H2_XIANXIA_RPG_SCRIPT_C7_LAMP,
  H2_XIANXIA_RPG_SCRIPT_C7_WATER,
  H2_XIANXIA_RPG_SCRIPT_C7_TREATY,
  H2_XIANXIA_RPG_SCRIPT_C7_NEED_PAST,
  H2_XIANXIA_RPG_SCRIPT_C7_NEED_NOW,
  H2_XIANXIA_RPG_SCRIPT_C7_NEED_LAMP,
  H2_XIANXIA_RPG_SCRIPT_C7_NEED_TREATY,
  H2_XIANXIA_RPG_SCRIPT_C7_ANSWER1A,
  H2_XIANXIA_RPG_SCRIPT_C7_ANSWER1B,
  H2_XIANXIA_RPG_SCRIPT_C7_ANSWER2A,
  H2_XIANXIA_RPG_SCRIPT_C7_ANSWER2B,
  H2_XIANXIA_RPG_SCRIPT_C7_ANSWER3A,
  H2_XIANXIA_RPG_SCRIPT_C7_ANSWER3B,
  H2_XIANXIA_RPG_SCRIPT_C7_BATTLE,
  H2_XIANXIA_RPG_SCRIPT_C7_RETRY,
  H2_XIANXIA_RPG_SCRIPT_C7_REVEAL,
  H2_XIANXIA_RPG_SCRIPT_C7_RECALL_GIVE,
  H2_XIANXIA_RPG_SCRIPT_C7_RECALL_TAKE,
  H2_XIANXIA_RPG_SCRIPT_C8_FIRST,
  H2_XIANXIA_RPG_SCRIPT_C8_INTRO = H2_XIANXIA_RPG_SCRIPT_C8_FIRST,
  H2_XIANXIA_RPG_SCRIPT_C8_HELP,
  H2_XIANXIA_RPG_SCRIPT_C8_EAST,
  H2_XIANXIA_RPG_SCRIPT_C8_WEST,
  H2_XIANXIA_RPG_SCRIPT_C8_REST,
  H2_XIANXIA_RPG_SCRIPT_C8_LIGHT,
  H2_XIANXIA_RPG_SCRIPT_C8_BLOCK,
  H2_XIANXIA_RPG_SCRIPT_C8_FIRST_EAST,
  H2_XIANXIA_RPG_SCRIPT_C8_FIRST_WEST,
  H2_XIANXIA_RPG_SCRIPT_C8_INJURY,
  H2_XIANXIA_RPG_SCRIPT_C8_RESCUED,
  H2_XIANXIA_RPG_SCRIPT_C8_NEED_RESCUE,
  H2_XIANXIA_RPG_SCRIPT_C8_BATTLE,
  H2_XIANXIA_RPG_SCRIPT_C8_RETRY,
  H2_XIANXIA_RPG_SCRIPT_C8_REVEAL,
  H2_XIANXIA_RPG_SCRIPT_C8_JIANGLI,
  H2_XIANXIA_RPG_SCRIPT_C8_WENZHOU,
  H2_XIANXIA_RPG_SCRIPT_C8_NO_PILL,
  H2_XIANXIA_RPG_SCRIPT_C8_RECALL_GIVE,
  H2_XIANXIA_RPG_SCRIPT_C8_RECALL_TAKE,
  H2_XIANXIA_RPG_SCRIPT_C9_FIRST,
  H2_XIANXIA_RPG_SCRIPT_C9_INTRO = H2_XIANXIA_RPG_SCRIPT_C9_FIRST,
  H2_XIANXIA_RPG_SCRIPT_C9_BOOK,
  H2_XIANXIA_RPG_SCRIPT_C9_LOWER,
  H2_XIANXIA_RPG_SCRIPT_C9_HALL,
  H2_XIANXIA_RPG_SCRIPT_C9_MIST,
  H2_XIANXIA_RPG_SCRIPT_C9_TIDE,
  H2_XIANXIA_RPG_SCRIPT_C9_COLD,
  H2_XIANXIA_RPG_SCRIPT_C9_MIRROR,
  H2_XIANXIA_RPG_SCRIPT_C9_TRIAL_WON,
  H2_XIANXIA_RPG_SCRIPT_C9_NEED,
  H2_XIANXIA_RPG_SCRIPT_C9_FATHER,
  H2_XIANXIA_RPG_SCRIPT_C9_BATTLE,
  H2_XIANXIA_RPG_SCRIPT_C9_RETRY,
  H2_XIANXIA_RPG_SCRIPT_C9_REVEAL,
  H2_XIANXIA_RPG_SCRIPT_C9_REST,
  H2_XIANXIA_RPG_SCRIPT_C9_HELP,
  H2_XIANXIA_RPG_SCRIPT_C10_FIRST,
  H2_XIANXIA_RPG_SCRIPT_C10_INTRO = H2_XIANXIA_RPG_SCRIPT_C10_FIRST,
  H2_XIANXIA_RPG_SCRIPT_C10_HELP,
  H2_XIANXIA_RPG_SCRIPT_C10_NEED,
  H2_XIANXIA_RPG_SCRIPT_C10_SHRINE,
  H2_XIANXIA_RPG_SCRIPT_C10_ARRAY,
  H2_XIANXIA_RPG_SCRIPT_C10_RETURN,
  H2_XIANXIA_RPG_SCRIPT_C10_RETRY,
  H2_XIANXIA_RPG_SCRIPT_C10_REUNION,
  H2_XIANXIA_RPG_SCRIPT_C10_RELEASE,
  H2_XIANXIA_RPG_SCRIPT_C10_KEEP,
  H2_XIANXIA_RPG_SCRIPT_C10_DOCK,
  H2_XIANXIA_RPG_SCRIPT_C10_TOWER,
  H2_XIANXIA_RPG_SCRIPT_C10_FESTIVAL,
  H2_XIANXIA_RPG_SCRIPT_C10_REST,
  H2_XIANXIA_RPG_SCRIPT_C10_RECALL_GIVE,
  H2_XIANXIA_RPG_SCRIPT_C10_RECALL_TAKE,
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
  H2_XIANXIA_RPG_TEXT_C3_ONWARD,
  H2_XIANXIA_RPG_TEXT_C3_UNKNOWN,
  H2_XIANXIA_RPG_TEXT_C3_RAIN_HINT,
  H2_XIANXIA_RPG_TEXT_C3_THRUST_HINT,
  H2_XIANXIA_RPG_TEXT_C3_WIND_HINT,
  H2_XIANXIA_RPG_TEXT_C3_RAIN_HIT,
  H2_XIANXIA_RPG_TEXT_C3_RAIN_GUARD,
  H2_XIANXIA_RPG_TEXT_C3_THRUST_HIT,
  H2_XIANXIA_RPG_TEXT_C3_THRUST_BREAK,
  H2_XIANXIA_RPG_TEXT_C3_REBUFF,
  H2_XIANXIA_RPG_TEXT_C3_COUNTER,
  H2_XIANXIA_RPG_TEXT_C3_WIND_WAIT,
  H2_XIANXIA_RPG_TEXT_C3_POLE_WAIT,
  H2_XIANXIA_RPG_TEXT_C3_POSTS,
  H2_XIANXIA_RPG_TEXT_C4_ONWARD,
  H2_XIANXIA_RPG_TEXT_C4_ESCORT,
  H2_XIANXIA_RPG_TEXT_C4_COVER,
  H2_XIANXIA_RPG_TEXT_C4_SCOUT,
  H2_XIANXIA_RPG_TEXT_C4_HELP,
  H2_XIANXIA_RPG_TEXT_C4_GUARD,
  H2_XIANXIA_RPG_TEXT_C4_LAMP,
  H2_XIANXIA_RPG_TEXT_C4_ALLY,
  H2_XIANXIA_RPG_TEXT_C4_MEND,
  H2_XIANXIA_RPG_TEXT_C4_POLE,
  H2_XIANXIA_RPG_TEXT_C4_DOWN,
  H2_XIANXIA_RPG_TEXT_C4_HEAL,
  H2_XIANXIA_RPG_TEXT_C4_PILL,
  H2_XIANXIA_RPG_TEXT_C4_LAMP_HINT,
  H2_XIANXIA_RPG_TEXT_C4_ALLY_HINT,
  H2_XIANXIA_RPG_TEXT_C4_WAVE_HINT,
  H2_XIANXIA_RPG_TEXT_C4_BLOCK,
  H2_XIANXIA_RPG_TEXT_C4_HIT_LAMP,
  H2_XIANXIA_RPG_TEXT_C4_HIT_ALLY,
  H2_XIANXIA_RPG_TEXT_C4_HIT_HERO,
  H2_XIANXIA_RPG_TEXT_C4_REPAIR,
  H2_XIANXIA_RPG_TEXT_C4_REST_ALLY,
  H2_XIANXIA_RPG_TEXT_C4_GUARDING,
  H2_XIANXIA_RPG_TEXT_C4_WAVE,
  H2_XIANXIA_RPG_TEXT_C4_LOST,
  H2_XIANXIA_RPG_TEXT_C4_WON,
  H2_XIANXIA_RPG_TEXT_C4_FIRE_LIT,
  H2_XIANXIA_RPG_TEXT_C4_FIRE_NONE,
  H2_XIANXIA_RPG_TEXT_C4_HERO,
  H2_XIANXIA_RPG_TEXT_C4_RESOURCES,
  H2_XIANXIA_RPG_TEXT_C4_SWITCH,
  H2_XIANXIA_RPG_TEXT_C5_ONWARD,
  H2_XIANXIA_RPG_TEXT_C5_LOW,
  H2_XIANXIA_RPG_TEXT_C5_HIGH,
  H2_XIANXIA_RPG_TEXT_C5_HELP,
  H2_XIANXIA_RPG_TEXT_C5_TAKE,
  H2_XIANXIA_RPG_TEXT_C5_GIVE,
  H2_XIANXIA_RPG_TEXT_C5_CHOICE,
  H2_XIANXIA_RPG_TEXT_C5_STEPS,
  H2_XIANXIA_RPG_TEXT_C5_DUEL,
  H2_XIANXIA_RPG_TEXT_C5_WON,
  H2_XIANXIA_RPG_TEXT_C5_OTTER,
  H2_XIANXIA_RPG_TEXT_C5_RAIN,
  H2_XIANXIA_RPG_TEXT_C5_THRUST,
  H2_XIANXIA_RPG_TEXT_C5_WIND,
  H2_XIANXIA_RPG_TEXT_C5_VISIT,
  H2_XIANXIA_RPG_TEXT_C5_ENEMY,
  H2_XIANXIA_RPG_TEXT_C5_WATER,
  H2_XIANXIA_RPG_TEXT_C6_ONWARD,
  H2_XIANXIA_RPG_TEXT_C6_HOT,
  H2_XIANXIA_RPG_TEXT_C6_COLD,
  H2_XIANXIA_RPG_TEXT_C6_HELP,
  H2_XIANXIA_RPG_TEXT_C6_BACK,
  H2_XIANXIA_RPG_TEXT_C6_STATUS,
  H2_XIANXIA_RPG_TEXT_C6_STEADY,
  H2_XIANXIA_RPG_TEXT_C6_WARM,
  H2_XIANXIA_RPG_TEXT_C6_CHILL,
  H2_XIANXIA_RPG_TEXT_C6_RITUAL,
  H2_XIANXIA_RPG_TEXT_C6_BREATH_HOT,
  H2_XIANXIA_RPG_TEXT_C6_BREATH_COLD,
  H2_XIANXIA_RPG_TEXT_C6_COOL,
  H2_XIANXIA_RPG_TEXT_C6_HEAT,
  H2_XIANXIA_RPG_TEXT_C6_CLEAR,
  H2_XIANXIA_RPG_TEXT_C6_HOLD,
  H2_XIANXIA_RPG_TEXT_C6_STOP,
  H2_XIANXIA_RPG_TEXT_C6_WISP,
  H2_XIANXIA_RPG_TEXT_C6_RULE,
  H2_XIANXIA_RPG_TEXT_C6_CREEK,
  H2_XIANXIA_RPG_TEXT_C6_SPRING,
  H2_XIANXIA_RPG_TEXT_C6_HUT,
  H2_XIANXIA_RPG_TEXT_C6_CHARM,
  H2_XIANXIA_RPG_TEXT_C6_PAUSE,
  H2_XIANXIA_RPG_TEXT_C7_ONWARD,
  H2_XIANXIA_RPG_TEXT_C7_NOW,
  H2_XIANXIA_RPG_TEXT_C7_PAST,
  H2_XIANXIA_RPG_TEXT_C7_STATUS,
  H2_XIANXIA_RPG_TEXT_C7_HELP,
  H2_XIANXIA_RPG_TEXT_C7_TALK,
  H2_XIANXIA_RPG_TEXT_C7_Q1,
  H2_XIANXIA_RPG_TEXT_C7_Q2,
  H2_XIANXIA_RPG_TEXT_C7_Q3,
  H2_XIANXIA_RPG_TEXT_C7_A1,
  H2_XIANXIA_RPG_TEXT_C7_B1,
  H2_XIANXIA_RPG_TEXT_C7_A2,
  H2_XIANXIA_RPG_TEXT_C7_B2,
  H2_XIANXIA_RPG_TEXT_C7_A3,
  H2_XIANXIA_RPG_TEXT_C7_B3,
  H2_XIANXIA_RPG_TEXT_C7_M_ATTACK,
  H2_XIANXIA_RPG_TEXT_C7_M_SKILL,
  H2_XIANXIA_RPG_TEXT_C7_M_PILL,
  H2_XIANXIA_RPG_TEXT_C7_M_GUARD,
  H2_XIANXIA_RPG_TEXT_C7_M_JOINT,
  H2_XIANXIA_RPG_TEXT_C7_M_HIT,
  H2_XIANXIA_RPG_TEXT_C7_M_HEAL,
  H2_XIANXIA_RPG_TEXT_C7_M_WAIT,
  H2_XIANXIA_RPG_TEXT_C7_M_STILL,
  H2_XIANXIA_RPG_TEXT_C7_STONE,
  H2_XIANXIA_RPG_TEXT_C7_LAMP,
  H2_XIANXIA_RPG_TEXT_C7_WATER,
  H2_XIANXIA_RPG_TEXT_C7_TREATY,
  H2_XIANXIA_RPG_TEXT_C7_HEART,
  H2_XIANXIA_RPG_TEXT_C7_EXIT,
  H2_XIANXIA_RPG_TEXT_C7_BACK,
  H2_XIANXIA_RPG_TEXT_C7_TO_HALL,
  H2_XIANXIA_RPG_TEXT_C7_RECORDED,
  H2_XIANXIA_RPG_TEXT_C8_ONWARD,
  H2_XIANXIA_RPG_TEXT_C8_MENU_NEXT,
  H2_XIANXIA_RPG_TEXT_C8_MENU_PREVIOUS,
  H2_XIANXIA_RPG_TEXT_C8_EAST_SAFE,
  H2_XIANXIA_RPG_TEXT_C8_WEST_SAFE,
  H2_XIANXIA_RPG_TEXT_C8_EAST,
  H2_XIANXIA_RPG_TEXT_C8_WEST,
  H2_XIANXIA_RPG_TEXT_C8_BACK,
  H2_XIANXIA_RPG_TEXT_C8_HELP,
  H2_XIANXIA_RPG_TEXT_C8_COAL,
  H2_XIANXIA_RPG_TEXT_C8_PILL,
  H2_XIANXIA_RPG_TEXT_C8_REST,
  H2_XIANXIA_RPG_TEXT_C8_BRIDGE,
  H2_XIANXIA_RPG_TEXT_C8_LIGHT,
  H2_XIANXIA_RPG_TEXT_C8_TEAM,
  H2_XIANXIA_RPG_TEXT_C8_STATUS,
  H2_XIANXIA_RPG_TEXT_C8_ORDER_EAST,
  H2_XIANXIA_RPG_TEXT_C8_ORDER_WEST,
  H2_XIANXIA_RPG_TEXT_C8_ORDER_NONE,
  H2_XIANXIA_RPG_TEXT_C8_SNOW,
  H2_XIANXIA_RPG_TEXT_C8_DUAL,
  H2_XIANXIA_RPG_TEXT_C8_TARGET_EAST,
  H2_XIANXIA_RPG_TEXT_C8_TARGET_WEST,
  H2_XIANXIA_RPG_TEXT_C8_TARGET_BOTH,
  H2_XIANXIA_RPG_TEXT_C8_BLOCK_HIT,
  H2_XIANXIA_RPG_TEXT_C8_LAMP_HIT,
  H2_XIANXIA_RPG_TEXT_C8_MEND,
  H2_XIANXIA_RPG_TEXT_C8_COVER,
  H2_XIANXIA_RPG_TEXT_C8_GUARD,
  H2_XIANXIA_RPG_TEXT_C8_LOST,
  H2_XIANXIA_RPG_TEXT_C8_WON,
  H2_XIANXIA_RPG_TEXT_C8_SUPPORT_EAST,
  H2_XIANXIA_RPG_TEXT_C8_SUPPORT_WEST,
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

/* Chapter 10 reuses the earlier villages and landmarks as new states. */
char h2_xianxia_rpg_c10_tile(h2_xianxia_rpg_map_t map, int32_t x, int32_t y);
h2_xianxia_rpg_map_t h2_xianxia_rpg_c10_base_map(h2_xianxia_rpg_map_t map);
const char* h2_xianxia_rpg_c10_map_name(h2_xianxia_rpg_map_t map);
const h2_xianxia_rpg_chapter_info_t* h2_xianxia_rpg_c10_info(void);
const h2_xianxia_rpg_script_t* h2_xianxia_rpg_c10_script(
    h2_xianxia_rpg_script_id_t id);
size_t h2_xianxia_rpg_c10_strings(const char** out, size_t capacity);
extern const uint8_t h2_xianxia_rpg_c10_helpers[4];
extern const char* const h2_xianxia_rpg_c10_destinations[3];
extern const char* const h2_xianxia_rpg_c10_hints[3][3];
typedef enum h2_xianxia_rpg_c10_text_id {
  H2_XIANXIA_RPG_C10_ONWARD,
  H2_XIANXIA_RPG_C10_HELP,
  H2_XIANXIA_RPG_C10_REST,
  H2_XIANXIA_RPG_C10_SHRINE,
  H2_XIANXIA_RPG_C10_ALTAR,
  H2_XIANXIA_RPG_C10_CONTINUE,
  H2_XIANXIA_RPG_C10_FORM,
  H2_XIANXIA_RPG_C10_SWITCH,
  H2_XIANXIA_RPG_C10_STATUS,
  H2_XIANXIA_RPG_C10_ASSIGN,
  H2_XIANXIA_RPG_C10_ASSIGN_HELP,
  H2_XIANXIA_RPG_C10_ASSIGNED,
  H2_XIANXIA_RPG_C10_CORRECT,
  H2_XIANXIA_RPG_C10_WRONG,
  H2_XIANXIA_RPG_C10_WON,
  H2_XIANXIA_RPG_C10_SWITCH_HINT,
  H2_XIANXIA_RPG_C10_PROGRESS,
  H2_XIANXIA_RPG_C10_EAST,
  H2_XIANXIA_RPG_C10_WEST,
  H2_XIANXIA_RPG_C10_RELEASE,
  H2_XIANXIA_RPG_C10_KEEP,
  H2_XIANXIA_RPG_C10_SELF,
  H2_XIANXIA_RPG_C10_END_RELEASE,
  H2_XIANXIA_RPG_C10_END_KEEP,
  H2_XIANXIA_RPG_C10_END_COMPLETE,
  H2_XIANXIA_RPG_C10_TEXT_COUNT,
} h2_xianxia_rpg_c10_text_id_t;
extern const char* const h2_xianxia_rpg_c10_text[H2_XIANXIA_RPG_C10_TEXT_COUNT];

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

/* Chapter 3 tables and the clockwise sword-post puzzle. */
char h2_xianxia_rpg_c3_tile(h2_xianxia_rpg_map_t map, int32_t x, int32_t y);
const char* h2_xianxia_rpg_c3_map_name(h2_xianxia_rpg_map_t map);
const h2_xianxia_rpg_script_t* h2_xianxia_rpg_c3_script(
    h2_xianxia_rpg_script_id_t id);
const h2_xianxia_rpg_chapter_info_t* h2_xianxia_rpg_c3_info(void);
size_t h2_xianxia_rpg_c3_npcs(
    const h2_xianxia_rpg_game_t* game, h2_xianxia_rpg_map_t map,
    h2_xianxia_rpg_npc_t out[H2_XIANXIA_RPG_MAX_NPCS]);
h2_xianxia_rpg_script_id_t h2_xianxia_rpg_c3_talk_script(
    const h2_xianxia_rpg_game_t* game, h2_xianxia_rpg_actor_t actor);
bool h2_xianxia_rpg_c3_walkable(const h2_xianxia_rpg_game_t* game, char tile);
bool h2_xianxia_rpg_c3_tile_interactive(const h2_xianxia_rpg_game_t* game,
                                        char tile);
h2_xianxia_rpg_script_id_t h2_xianxia_rpg_c3_tile_script(
    const h2_xianxia_rpg_game_t* game, char tile);
uint32_t h2_xianxia_rpg_c3_post_direction(uint32_t flags, uint32_t post);
bool h2_xianxia_rpg_c3_posts_aligned(uint32_t flags);
size_t h2_xianxia_rpg_c3_strings(const char** out, size_t capacity);

/* Chapter 4: the return to the village, ridge escort and altar. */
char h2_xianxia_rpg_c4_tile(h2_xianxia_rpg_map_t map, int32_t x, int32_t y);
const char* h2_xianxia_rpg_c4_map_name(h2_xianxia_rpg_map_t map);
const h2_xianxia_rpg_script_t* h2_xianxia_rpg_c4_script(
    h2_xianxia_rpg_script_id_t id);
const h2_xianxia_rpg_chapter_info_t* h2_xianxia_rpg_c4_info(void);
size_t h2_xianxia_rpg_c4_npcs(
    const h2_xianxia_rpg_game_t* game, h2_xianxia_rpg_map_t map,
    h2_xianxia_rpg_npc_t out[H2_XIANXIA_RPG_MAX_NPCS]);
h2_xianxia_rpg_script_id_t h2_xianxia_rpg_c4_talk_script(
    const h2_xianxia_rpg_game_t* game, h2_xianxia_rpg_actor_t actor);
h2_xianxia_rpg_script_id_t h2_xianxia_rpg_c4_tile_script(
    const h2_xianxia_rpg_game_t* game, char tile);
size_t h2_xianxia_rpg_c4_strings(const char** out, size_t capacity);

/* Chapter 5: tide routes, the Wen family and a bounded rescue. */
char h2_xianxia_rpg_c5_tile(h2_xianxia_rpg_map_t map, int32_t x, int32_t y);
const char* h2_xianxia_rpg_c5_map_name(h2_xianxia_rpg_map_t map);
const h2_xianxia_rpg_chapter_info_t* h2_xianxia_rpg_c5_info(void);
const h2_xianxia_rpg_script_t* h2_xianxia_rpg_c5_script(
    h2_xianxia_rpg_script_id_t id);
size_t h2_xianxia_rpg_c5_npcs(
    const h2_xianxia_rpg_game_t* game, h2_xianxia_rpg_map_t map,
    h2_xianxia_rpg_npc_t out[H2_XIANXIA_RPG_MAX_NPCS]);
h2_xianxia_rpg_script_id_t h2_xianxia_rpg_c5_talk_script(
    const h2_xianxia_rpg_game_t* game, h2_xianxia_rpg_actor_t actor);
h2_xianxia_rpg_script_id_t h2_xianxia_rpg_c5_tile_script(
    const h2_xianxia_rpg_game_t* game, char tile);
bool h2_xianxia_rpg_c5_walkable(const h2_xianxia_rpg_game_t* game, char tile);
size_t h2_xianxia_rpg_c5_strings(const char** out, size_t capacity);

/* Chapter 6: thermal paths and the step-based healing spring. */
char h2_xianxia_rpg_c6_tile(h2_xianxia_rpg_map_t map, int32_t x, int32_t y);
const char* h2_xianxia_rpg_c6_map_name(h2_xianxia_rpg_map_t map);
const h2_xianxia_rpg_chapter_info_t* h2_xianxia_rpg_c6_info(void);
const h2_xianxia_rpg_script_t* h2_xianxia_rpg_c6_script(
    h2_xianxia_rpg_script_id_t id);
size_t h2_xianxia_rpg_c6_npcs(
    const h2_xianxia_rpg_game_t* game, h2_xianxia_rpg_map_t map,
    h2_xianxia_rpg_npc_t out[H2_XIANXIA_RPG_MAX_NPCS]);
h2_xianxia_rpg_script_id_t h2_xianxia_rpg_c6_talk_script(
    const h2_xianxia_rpg_game_t* game, h2_xianxia_rpg_actor_t actor);
h2_xianxia_rpg_script_id_t h2_xianxia_rpg_c6_tile_script(
    const h2_xianxia_rpg_game_t* game, char tile);
size_t h2_xianxia_rpg_c6_strings(const char** out, size_t capacity);

/* Chapter 7: paired-era routes and persistent heart choices. */
char h2_xianxia_rpg_c7_tile(h2_xianxia_rpg_map_t map, int32_t x, int32_t y);
const char* h2_xianxia_rpg_c7_map_name(h2_xianxia_rpg_map_t map);
const h2_xianxia_rpg_chapter_info_t* h2_xianxia_rpg_c7_info(void);
const h2_xianxia_rpg_script_t* h2_xianxia_rpg_c7_script(
    h2_xianxia_rpg_script_id_t id);
bool h2_xianxia_rpg_c7_walkable(const h2_xianxia_rpg_game_t* game, char tile);
h2_xianxia_rpg_script_id_t h2_xianxia_rpg_c7_feedback(
    const h2_xianxia_rpg_game_t* game);
size_t h2_xianxia_rpg_c7_strings(const char** out, size_t capacity);

char h2_xianxia_rpg_c9_tile(h2_xianxia_rpg_map_t map, int32_t x, int32_t y);
const char* h2_xianxia_rpg_c9_map_name(h2_xianxia_rpg_map_t map);
const h2_xianxia_rpg_chapter_info_t* h2_xianxia_rpg_c9_info(void);
const h2_xianxia_rpg_script_t* h2_xianxia_rpg_c9_script(
    h2_xianxia_rpg_script_id_t id);
size_t h2_xianxia_rpg_c9_strings(const char** out, size_t capacity);
/** Each door has two canonical reference glyphs and one altered candidate.
 * IDs 0..7 are ink/mist/sword/wind/tide/spring/mirror/snow; 8..11 are fakes. */
uint32_t h2_xianxia_rpg_c9_glyph(uint32_t room, uint32_t choice);
uint32_t h2_xianxia_rpg_c9_reference(uint32_t room, uint32_t index);
bool h2_xianxia_rpg_c9_wrong_glyph(uint32_t room, uint32_t choice);
extern const char* const h2_xianxia_rpg_c9_glyph_names[8];
extern const char* const h2_xianxia_rpg_c9_room_names[4];
extern const char* const h2_xianxia_rpg_c9_phase_hints[4];

typedef enum h2_xianxia_rpg_c9_text_id {
  H2_XIANXIA_RPG_C9_BOOK_TITLE,
  H2_XIANXIA_RPG_C9_BOOK_HELP,
  H2_XIANXIA_RPG_C9_DOOR_HELP,
  H2_XIANXIA_RPG_C9_WRONG,
  H2_XIANXIA_RPG_C9_BACK,
  H2_XIANXIA_RPG_C9_BOOK_BUTTON,
  H2_XIANXIA_RPG_C9_HELP_BUTTON,
  H2_XIANXIA_RPG_C9_SWITCH,
  H2_XIANXIA_RPG_C9_SWITCHED,
  H2_XIANXIA_RPG_C9_JIANGLI,
  H2_XIANXIA_RPG_C9_WENZHOU,
  H2_XIANXIA_RPG_C9_STATUS,
  H2_XIANXIA_RPG_C9_PHASE,
  H2_XIANXIA_RPG_C9_MIST_LIGHT,
  H2_XIANXIA_RPG_C9_TIDE_CUT,
  H2_XIANXIA_RPG_C9_COLD_WARM,
  H2_XIANXIA_RPG_C9_MIRROR_READ,
  H2_XIANXIA_RPG_C9_ALLY_HIT,
  H2_XIANXIA_RPG_C9_ENEMY_HIT,
  H2_XIANXIA_RPG_C9_MIST_HOLD,
  H2_XIANXIA_RPG_C9_CORE_WON,
  H2_XIANXIA_RPG_C9_TRIAL_WON,
  H2_XIANXIA_RPG_C9_REST,
  H2_XIANXIA_RPG_C9_DOWN,
  H2_XIANXIA_RPG_C9_UP,
  H2_XIANXIA_RPG_C9_HALL,
  H2_XIANXIA_RPG_C9_CORE,
  H2_XIANXIA_RPG_C9_INSCRIPTION,
  H2_XIANXIA_RPG_C9_FATHER,
  H2_XIANXIA_RPG_C9_READY,
  H2_XIANXIA_RPG_C9_ONWARD,
  H2_XIANXIA_RPG_C9_MIRROR_ORDER,
  H2_XIANXIA_RPG_C9_DESCEND_READY,
  H2_XIANXIA_RPG_C9_TEXT_COUNT,
} h2_xianxia_rpg_c9_text_id_t;
extern const char* const h2_xianxia_rpg_c9_text[H2_XIANXIA_RPG_C9_TEXT_COUNT];

char h2_xianxia_rpg_c8_tile(h2_xianxia_rpg_map_t map, int32_t x, int32_t y);
const char* h2_xianxia_rpg_c8_map_name(h2_xianxia_rpg_map_t map);
const h2_xianxia_rpg_chapter_info_t* h2_xianxia_rpg_c8_info(void);
const h2_xianxia_rpg_script_t* h2_xianxia_rpg_c8_script(
    h2_xianxia_rpg_script_id_t id);
bool h2_xianxia_rpg_c8_walkable(const h2_xianxia_rpg_game_t* game,
                                h2_xianxia_rpg_map_t map, char tile);
size_t h2_xianxia_rpg_c8_strings(const char** out, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
