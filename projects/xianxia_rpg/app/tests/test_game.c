#include "h2_xianxia_rpg_game.h"
#include "h2_xianxia_rpg_story.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                               \
  do {                                                                 \
    if (!(condition)) {                                                \
      fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, \
              #condition);                                             \
      exit(1);                                                         \
    }                                                                  \
  } while (0)

static h2_xianxia_rpg_game_t game;

static void run(h2_xianxia_rpg_game_t* g, uint32_t ms) {
  while (ms > 0u) {
    const uint32_t step = ms > 20u ? 20u : ms;
    h2_xianxia_rpg_game_step(g, step);
    ms -= step;
  }
}

/* A tap through the production pointer path. */
static void tap(h2_xianxia_rpg_game_t* g, float x, float y) {
  h2_xianxia_rpg_game_pointer(g, H2_XIANXIA_RPG_POINTER_DOWN, x, y);
  h2_xianxia_rpg_game_pointer(g, H2_XIANXIA_RPG_POINTER_UP, x, y);
}

static void tap_rect(h2_xianxia_rpg_game_t* g, const h2_xianxia_rpg_rect_t* r) {
  tap(g, (float)r->x + (float)r->w * 0.5f, (float)r->y + (float)r->h * 0.5f);
}

static void tap_tile(h2_xianxia_rpg_game_t* g, int32_t col, int32_t row) {
  const h2_xianxia_rpg_layout_t* l = &g->layout;
  tap(g, (float)l->map_x + ((float)col + 0.5f) * (float)l->tile,
      (float)l->map_y + ((float)row + 0.5f) * (float)l->tile);
}

static void finish_dialogue(h2_xianxia_rpg_game_t* g) {
  for (int i = 0; i < 64 && g->dialogue.active; ++i) {
    run(g, 60);
    tap(g, (float)g->width * 0.5f, (float)g->height * 0.5f);
  }
  if (g->dialogue.active)
    fprintf(stderr, "stuck dialogue phase=%d stage=%d line=%u revealed=%u/%u\n",
            (int)g->phase, (int)g->stage, (unsigned)g->dialogue.index,
            (unsigned)g->dialogue.revealed, (unsigned)g->dialogue.total);
  CHECK(!g->dialogue.active);
}

/* Walk until the path and any interaction at its end are done. */
static void settle(h2_xianxia_rpg_game_t* g) {
  for (int i = 0; i < 400; ++i) {
    if (g->phase != H2_XIANXIA_RPG_PHASE_EXPLORE || g->dialogue.active) return;
    if (!g->moving && g->path_pos >= g->path_len && g->target_x < 0) return;
    run(g, 20);
  }
  CHECK(!"walk did not settle");
}

static void start_new_game(h2_xianxia_rpg_game_t* g, int32_t width,
                           int32_t height, uint32_t seed) {
  h2_xianxia_rpg_game_init(g, width, height, seed, NULL);
  CHECK(g->phase == H2_XIANXIA_RPG_PHASE_TITLE);
  run(g, 300);
  tap_rect(g, &g->layout.title_buttons[0]);
  CHECK(g->phase == H2_XIANXIA_RPG_PHASE_EXPLORE);
  CHECK(g->dialogue.active);
  finish_dialogue(g);
  run(g, 300);
}

static void talk_to(h2_xianxia_rpg_game_t* g, int32_t col, int32_t row) {
  tap_tile(g, col, row);
  settle(g);
  CHECK(g->dialogue.active);
  finish_dialogue(g);
}

static void walk_to(h2_xianxia_rpg_game_t* g, int32_t col, int32_t row) {
  tap_tile(g, col, row);
  CHECK(g->path_len > 0u);
  settle(g);
}

static h2_xianxia_rpg_command_t pick(const h2_xianxia_rpg_game_t* g) {
  const h2_xianxia_rpg_battle_t* b = &g->battle;
  if (b->charging) return H2_XIANXIA_RPG_COMMAND_GUARD;
  if (g->hero.hp * 5u < g->hero.hp_max * 2u &&
      h2_xianxia_rpg_command_available(g, H2_XIANXIA_RPG_COMMAND_PILL))
    return H2_XIANXIA_RPG_COMMAND_PILL;
  if (h2_xianxia_rpg_command_available(g, H2_XIANXIA_RPG_COMMAND_SKILL))
    return H2_XIANXIA_RPG_COMMAND_SKILL;
  return H2_XIANXIA_RPG_COMMAND_ATTACK;
}

/* Fight with a fixed strategy until the battle screen closes; returns the
 * final battle state. */
static h2_xianxia_rpg_battle_state_t fight(h2_xianxia_rpg_game_t* g) {
  CHECK(g->phase == H2_XIANXIA_RPG_PHASE_BATTLE);
  run(g, 300);
  for (int i = 0; i < 600; ++i) {
    h2_xianxia_rpg_battle_t* b = &g->battle;
    if (b->state == H2_XIANXIA_RPG_BATTLE_COMMAND) {
      tap_rect(g, &g->layout.commands[pick(g)]);
    } else if (b->state == H2_XIANXIA_RPG_BATTLE_VICTORY ||
               b->state == H2_XIANXIA_RPG_BATTLE_DEFEAT) {
      const h2_xianxia_rpg_battle_state_t state = b->state;
      run(g, 500);
      tap(g, (float)g->width * 0.5f, (float)g->height * 0.5f);
      CHECK(g->phase == H2_XIANXIA_RPG_PHASE_EXPLORE);
      return state;
    }
    run(g, 100);
  }
  CHECK(!"battle did not finish");
  return H2_XIANXIA_RPG_BATTLE_DEFEAT;
}

static bool rect_inside(const h2_xianxia_rpg_rect_t* r, int32_t w, int32_t h) {
  return r->x >= 0 && r->y >= 0 && r->w > 0 && r->h > 0 && r->x + r->w <= w &&
         r->y + r->h <= h;
}

static void test_layouts(void) {
  static const int32_t sizes[][2] = {{640, 480},  {480, 800}, {480, 480},
                                     {368, 448},  {480, 320}, {320, 240},
                                     {1024, 768}, {800, 480}, {1024, 1024}};
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
    const int32_t w = sizes[i][0], h = sizes[i][1];
    h2_xianxia_rpg_game_init(&game, w, h, 7u, NULL);
    const h2_xianxia_rpg_layout_t* l = &game.layout;
    CHECK(l->tile >= 16);
    CHECK(l->map_x >= 0 && l->map_y >= 0);
    CHECK(l->map_x + H2_XIANXIA_RPG_MAP_COLS * l->tile <= w);
    CHECK(l->map_y + H2_XIANXIA_RPG_MAP_ROWS * l->tile <= h);
    CHECK(rect_inside(&l->hud, w, h));
    CHECK(rect_inside(&l->dialog, w, h));
    CHECK(l->hud.y + l->hud.h <= l->map_y);
    if (l->stacked)
      CHECK(l->dialog.y >= l->map_y + H2_XIANXIA_RPG_MAP_ROWS * l->tile);
    for (int32_t b = 0; b < H2_XIANXIA_RPG_TITLE_BUTTONS; ++b)
      CHECK(rect_inside(&l->title_buttons[b], w, h));
    CHECK(rect_inside(&l->panel, w, h));
    CHECK(rect_inside(&l->message, w, h));
    for (int32_t c = 0; c < H2_XIANXIA_RPG_COMMANDS; ++c) {
      CHECK(rect_inside(&l->commands[c], w, h));
      CHECK(l->commands[c].w >= 48);
      if (c > 0)
        CHECK(l->commands[c - 1].x + l->commands[c - 1].w <= l->commands[c].x);
    }
  }
  h2_xianxia_rpg_game_init(&game, 480, 800, 1u, NULL);
  CHECK(game.layout.stacked);
  h2_xianxia_rpg_game_init(&game, 368, 448, 1u, NULL);
  CHECK(game.layout.stacked);
  h2_xianxia_rpg_game_init(&game, 640, 480, 1u, NULL);
  CHECK(!game.layout.stacked);
}

static void test_maps_are_consistent(void) {
  h2_xianxia_rpg_game_init(&game, 640, 480, 1u, NULL);
  game.stage = H2_XIANXIA_RPG_STAGE_TO_SHRINE;
  for (int m = 0; m < H2_XIANXIA_RPG_MAP_COUNT; ++m)
    for (int32_t y = 0; y < H2_XIANXIA_RPG_MAP_ROWS; ++y)
      for (int32_t x = 0; x < H2_XIANXIA_RPG_MAP_COLS; ++x) {
        CHECK(h2_xianxia_rpg_tile((h2_xianxia_rpg_map_t)m, x, y) != '\0');
        h2_xianxia_rpg_map_t to;
        int32_t tx, ty;
        if (h2_xianxia_rpg_exit_at((h2_xianxia_rpg_map_t)m, x, y, &to, &tx,
                                   &ty)) {
          CHECK(h2_xianxia_rpg_walkable(&game, (h2_xianxia_rpg_map_t)m, x, y));
          CHECK(h2_xianxia_rpg_walkable(&game, to, tx, ty));
          /* The arrival cell is not itself an exit. */
          h2_xianxia_rpg_map_t again;
          int32_t ax, ay;
          CHECK(!h2_xianxia_rpg_exit_at(to, tx, ty, &again, &ax, &ay));
        }
      }
  CHECK(h2_xianxia_rpg_tile(H2_XIANXIA_RPG_MAP_VILLAGE, -1, 0) == '#');
  CHECK(h2_xianxia_rpg_tile(H2_XIANXIA_RPG_MAP_VILLAGE, 15, 0) == '#');
}

static void test_title_guard_and_continue_disabled(void) {
  h2_xianxia_rpg_game_init(&game, 640, 480, 3u, NULL);
  /* Taps during the guard after a screen change are ignored. */
  tap_rect(&game, &game.layout.title_buttons[0]);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_TITLE);
  run(&game, 300);
  CHECK(!game.has_save);
  tap_rect(&game, &game.layout.title_buttons[1]);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_TITLE);
}

static void test_walking(void) {
  start_new_game(&game, 640, 480, 11u);
  CHECK(game.map == H2_XIANXIA_RPG_MAP_VILLAGE);
  CHECK(game.tx == 7 && game.ty == 5);
  /* Water and walls are not destinations. */
  tap_tile(&game, 2, 7);
  CHECK(game.path_len == 0u);
  tap_tile(&game, 0, 0);
  CHECK(game.path_len == 0u);
  /* Across the bridge to the south bank. */
  tap_tile(&game, 2, 8);
  CHECK(game.path_len > 0u);
  bool crossed_bridge = false;
  for (uint32_t i = 0; i < game.path_len; ++i)
    if (game.path_x[i] == 4 && game.path_y[i] == 7) crossed_bridge = true;
  CHECK(crossed_bridge);
  settle(&game);
  CHECK(game.tx == 2 && game.ty == 8);
  CHECK(game.walk_steps >= 7u);
  /* Paths never pass through characters. */
  tap_tile(&game, 4, 4);
  CHECK(game.path_len > 0u);
  for (uint32_t i = 0; i < game.path_len; ++i)
    CHECK(!(game.path_x[i] == 4 && game.path_y[i] == 3));
  settle(&game);
  CHECK(game.tx == 4 && game.ty == 4);
}

static void test_full_journey(void) {
  start_new_game(&game, 640, 480, 21u);
  game.encounter_steps = 100000u;
  CHECK(game.stage == H2_XIANXIA_RPG_STAGE_MEET_ELDER);
  /* The herbalist sends the hero to the elder first. */
  talk_to(&game, 11, 3);
  CHECK(game.hero.pills == 0u);
  talk_to(&game, 4, 3);
  CHECK(game.stage == H2_XIANXIA_RPG_STAGE_SEEK_DAOIST);
  h2_xianxia_rpg_save_t save;
  CHECK(h2_xianxia_rpg_game_take_save(&game, &save));
  CHECK(save.stage == H2_XIANXIA_RPG_STAGE_SEEK_DAOIST);
  CHECK(!h2_xianxia_rpg_game_take_save(&game, &save));
  talk_to(&game, 11, 3);
  CHECK(game.hero.pills == 3u);
  CHECK(game.flags & H2_XIANXIA_RPG_FLAG_PILLS_GIVEN);
  /* Resting at the well restores and records a save point. */
  game.hero.hp = 5u;
  talk_to(&game, 8, 4);
  CHECK(game.hero.hp == game.hero.hp_max);
  CHECK(h2_xianxia_rpg_game_take_save(&game, &save));
  CHECK(save.map == H2_XIANXIA_RPG_MAP_VILLAGE);

  walk_to(&game, 14, 4);
  CHECK(game.map == H2_XIANXIA_RPG_MAP_FOREST);
  CHECK(game.tx == 1 && game.ty == 4);
  CHECK(game.banner_ms > 0u);
  game.encounter_steps = 100000u;
  /* The barrier seals the northern path until the Daoist teaches. */
  tap_tile(&game, 7, 0);
  CHECK(game.path_len == 0u);
  talk_to(&game, 7, 1);
  CHECK(game.stage == H2_XIANXIA_RPG_STAGE_SEEK_DAOIST);
  talk_to(&game, 11, 6);
  CHECK(game.stage == H2_XIANXIA_RPG_STAGE_TO_SHRINE);
  CHECK(game.hero.pills == 5u);
  CHECK(h2_xianxia_rpg_walkable(&game, H2_XIANXIA_RPG_MAP_FOREST, 7, 1));
  CHECK(h2_xianxia_rpg_game_take_save(&game, &save));

  walk_to(&game, 7, 0);
  CHECK(game.map == H2_XIANXIA_RPG_MAP_SHRINE);
  CHECK(game.tx == 7 && game.ty == 8);
  game.encounter_steps = 100000u;
  talk_to(&game, 4, 7);
  CHECK(h2_xianxia_rpg_game_take_save(&game, &save));
  CHECK(save.map == H2_XIANXIA_RPG_MAP_SHRINE);
  CHECK(game.rest_map == H2_XIANXIA_RPG_MAP_SHRINE);

  /* A seasoned hero faces the boss. */
  const uint32_t pills = game.hero.pills;
  h2_xianxia_rpg_hero_at_level(&game.hero, 6u);
  game.hero.pills = pills;
  tap_tile(&game, 7, 2);
  settle(&game);
  CHECK(game.dialogue.active);
  finish_dialogue(&game);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_BATTLE);
  CHECK(game.battle.boss);
  CHECK(!h2_xianxia_rpg_command_available(&game, H2_XIANXIA_RPG_COMMAND_FLEE));
  bool saw_charge = false;
  run(&game, 300);
  for (int i = 0; i < 800 && game.battle.state != H2_XIANXIA_RPG_BATTLE_VICTORY;
       ++i) {
    CHECK(game.battle.state != H2_XIANXIA_RPG_BATTLE_DEFEAT);
    if (game.battle.charging) saw_charge = true;
    if (game.battle.state == H2_XIANXIA_RPG_BATTLE_COMMAND)
      tap_rect(&game, &game.layout.commands[pick(&game)]);
    run(&game, 100);
  }
  CHECK(game.battle.state == H2_XIANXIA_RPG_BATTLE_VICTORY);
  CHECK(saw_charge);
  CHECK(game.stage == H2_XIANXIA_RPG_STAGE_RETURN);
  CHECK(game.objective_ms > 0u);
  CHECK(h2_xianxia_rpg_game_take_save(&game, &save));
  CHECK(save.stage == H2_XIANXIA_RPG_STAGE_RETURN);
  CHECK(save.map == H2_XIANXIA_RPG_MAP_SHRINE);
  CHECK(save.battles == 1u && save.turns == game.turns_taken);
  run(&game, 500);
  tap(&game, 320.0f, 240.0f);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_EXPLORE);
  h2_xianxia_rpg_npc_t npcs[H2_XIANXIA_RPG_MAX_NPCS];
  CHECK(h2_xianxia_rpg_npcs(&game, H2_XIANXIA_RPG_MAP_SHRINE, npcs) == 0u);
  finish_dialogue(&game);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_EXPLORE);
  /* Home again: no more ambushes, and the elder closes the chapter. */
  walk_to(&game, 7, 9);
  CHECK(game.map == H2_XIANXIA_RPG_MAP_FOREST);
  CHECK(h2_xianxia_rpg_talk_script(&game, H2_XIANXIA_RPG_ACTOR_DAOIST) ==
        H2_XIANXIA_RPG_SCRIPT_DAOIST_RETURN);
  walk_to(&game, 0, 4);
  CHECK(game.map == H2_XIANXIA_RPG_MAP_VILLAGE);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_EXPLORE);
  CHECK(game.battles_won == 1u);
  talk_to(&game, 11, 3);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_EXPLORE);
  talk_to(&game, 4, 3);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_ENDING);
  CHECK(game.stage == H2_XIANXIA_RPG_STAGE_CLEARED);
  CHECK(h2_xianxia_rpg_game_take_save(&game, &save));
  CHECK(save.stage == H2_XIANXIA_RPG_STAGE_CLEARED);
  CHECK(save.battles == game.battles_won && save.turns == game.turns_taken);
  const uint32_t cleared_level = game.hero.level;
  /* The ending ignores early taps, then returns to the title. */
  run(&game, 300);
  tap(&game, 320.0f, 240.0f);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_ENDING);
  run(&game, 1500);
  tap(&game, 320.0f, 240.0f);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_TITLE);
  CHECK(game.has_save);
  /* A reload after the ending reopens the result, not the boss fight. */
  uint32_t words[H2_XIANXIA_RPG_SAVE_WORDS];
  h2_xianxia_rpg_save_pack(&save, words);
  h2_xianxia_rpg_save_t reloaded;
  CHECK(h2_xianxia_rpg_save_unpack(words, &reloaded));
  const uint32_t won = game.battles_won, turns = game.turns_taken;
  h2_xianxia_rpg_game_init(&game, 640, 480, 99u, &reloaded);
  run(&game, 300);
  tap_rect(&game, &game.layout.title_buttons[1]);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_ENDING);
  CHECK(game.stage == H2_XIANXIA_RPG_STAGE_CLEARED);
  CHECK(game.hero.level == cleared_level);
  CHECK(game.battles_won == won && game.turns_taken == turns);
  run(&game, 1600);
  tap(&game, 320.0f, 240.0f);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_TITLE);
  CHECK(game.has_save && game.saved.stage == H2_XIANXIA_RPG_STAGE_CLEARED);
}

static void enter_forest_battle(h2_xianxia_rpg_game_t* g, uint32_t seed) {
  start_new_game(g, 640, 480, seed);
  g->encounter_steps = 100000u;
  walk_to(g, 14, 4);
  CHECK(g->map == H2_XIANXIA_RPG_MAP_FOREST);
  g->encounter_steps = 1u;
  tap_tile(g, 4, 4);
  for (int i = 0; i < 100 && g->phase != H2_XIANXIA_RPG_PHASE_BATTLE; ++i)
    run(g, 20);
  CHECK(g->phase == H2_XIANXIA_RPG_PHASE_BATTLE);
  CHECK(g->battle.enemy == H2_XIANXIA_RPG_ENEMY_LEAF_SPRITE ||
        g->battle.enemy == H2_XIANXIA_RPG_ENEMY_CIVET);
  CHECK(!g->moving && g->path_len == 0u);
}

static void test_battle_victory_and_growth(void) {
  enter_forest_battle(&game, 5u);
  const uint32_t exp = game.hero.exp;
  CHECK(fight(&game) == H2_XIANXIA_RPG_BATTLE_VICTORY);
  CHECK(game.hero.exp > exp);
  CHECK(game.battles_won == 1u);
  CHECK(game.map == H2_XIANXIA_RPG_MAP_FOREST);
  CHECK(game.encounter_steps >= 9u);

  CHECK(h2_xianxia_rpg_exp_for_level(1u) == 0u);
  CHECK(h2_xianxia_rpg_exp_for_level(2u) == 15u);
  CHECK(h2_xianxia_rpg_exp_for_level(3u) == 45u);
  h2_xianxia_rpg_hero_t hero;
  h2_xianxia_rpg_hero_at_level(&hero, 3u);
  CHECK(hero.hp == hero.hp_max && hero.hp_max == 88u);
  CHECK(hero.mp_max == 28u && hero.attack == 18u && hero.defense == 9u);
  h2_xianxia_rpg_hero_at_level(&hero, 99u);
  CHECK(hero.level == H2_XIANXIA_RPG_LEVEL_MAX);

  /* Enough experience levels the hero up after the next victory. */
  enter_forest_battle(&game, 6u);
  game.hero.exp = 14u;
  CHECK(fight(&game) == H2_XIANXIA_RPG_BATTLE_VICTORY);
  CHECK(game.hero.level >= 2u);
  CHECK(game.hero.hp == game.hero.hp_max);
}

static void test_unavailable_commands(void) {
  enter_forest_battle(&game, 8u);
  run(&game, 1000);
  CHECK(game.battle.state == H2_XIANXIA_RPG_BATTLE_COMMAND);
  const uint32_t message = game.battle.message_id;
  tap_rect(&game, &game.layout.commands[H2_XIANXIA_RPG_COMMAND_SKILL]);
  CHECK(game.battle.state == H2_XIANXIA_RPG_BATTLE_COMMAND);
  CHECK(game.battle.message_id != message);
  CHECK(strcmp(game.battle.message,
               h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_NO_SKILL]) == 0);
  tap_rect(&game, &game.layout.commands[H2_XIANXIA_RPG_COMMAND_PILL]);
  CHECK(game.battle.state == H2_XIANXIA_RPG_BATTLE_COMMAND);
  CHECK(strcmp(game.battle.message,
               h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_NO_PILLS]) == 0);
  /* Guarding restores spirit and softens the next blow. */
  game.hero.mp = 0u;
  tap_rect(&game, &game.layout.commands[H2_XIANXIA_RPG_COMMAND_GUARD]);
  CHECK(game.battle.state == H2_XIANXIA_RPG_BATTLE_HERO_ACT);
  CHECK(game.battle.guarding);
  CHECK(game.hero.mp == 4u);
  run(&game, 2000);
  CHECK(game.battle.state == H2_XIANXIA_RPG_BATTLE_COMMAND);
  CHECK(!game.battle.guarding);
  /* Fleeing eventually succeeds and returns to the map. */
  for (int i = 0; i < 20 && game.phase == H2_XIANXIA_RPG_PHASE_BATTLE; ++i) {
    if (game.battle.state == H2_XIANXIA_RPG_BATTLE_COMMAND) {
      game.hero.hp = game.hero.hp_max;
      tap_rect(&game, &game.layout.commands[H2_XIANXIA_RPG_COMMAND_FLEE]);
    }
    run(&game, 2000);
  }
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_EXPLORE);
  CHECK(game.map == H2_XIANXIA_RPG_MAP_FOREST);
}

static void test_defeat_revives_at_rest_point(void) {
  enter_forest_battle(&game, 9u);
  run(&game, 1000);
  game.hero.hp = 1u;
  tap_rect(&game, &game.layout.commands[H2_XIANXIA_RPG_COMMAND_ATTACK]);
  for (int i = 0; i < 60 && game.battle.state != H2_XIANXIA_RPG_BATTLE_DEFEAT;
       ++i) {
    if (game.battle.state == H2_XIANXIA_RPG_BATTLE_COMMAND)
      tap_rect(&game, &game.layout.commands[H2_XIANXIA_RPG_COMMAND_ATTACK]);
    run(&game, 100);
  }
  CHECK(game.battle.state == H2_XIANXIA_RPG_BATTLE_DEFEAT);
  CHECK(game.hero.hp == 0u);
  /* The defeat screen ignores an immediate tap. */
  tap(&game, 320.0f, 240.0f);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_BATTLE);
  run(&game, 500);
  tap(&game, 320.0f, 240.0f);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_EXPLORE);
  CHECK(game.map == H2_XIANXIA_RPG_MAP_VILLAGE);
  CHECK(game.tx == 8 && game.ty == 5);
  CHECK(game.hero.hp == game.hero.hp_max);
  CHECK(game.dialogue.active);
  CHECK(game.dialogue.script ==
        h2_xianxia_rpg_script(H2_XIANXIA_RPG_SCRIPT_REVIVE));
  finish_dialogue(&game);
}

/* Losing to the boss revives the hero at the spring for another try. */
static void test_boss_defeat_and_retry(void) {
  const h2_xianxia_rpg_save_t save = {.stage = H2_XIANXIA_RPG_STAGE_TO_SHRINE,
                                      .level = 1u,
                                      .exp = 0u,
                                      .pills = 0u,
                                      .flags = H2_XIANXIA_RPG_FLAG_PILLS_GIVEN,
                                      .map = H2_XIANXIA_RPG_MAP_SHRINE,
                                      .x = 5u,
                                      .y = 7u};
  h2_xianxia_rpg_game_init(&game, 640, 480, 12u, &save);
  run(&game, 300);
  tap_rect(&game, &game.layout.title_buttons[1]);
  game.encounter_steps = 100000u;
  run(&game, 300);
  talk_to(&game, 7, 2);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_BATTLE && game.battle.boss);
  run(&game, 300);
  for (int i = 0; i < 800 && game.battle.state != H2_XIANXIA_RPG_BATTLE_DEFEAT;
       ++i) {
    if (game.battle.state == H2_XIANXIA_RPG_BATTLE_COMMAND)
      tap_rect(&game, &game.layout.commands[H2_XIANXIA_RPG_COMMAND_ATTACK]);
    run(&game, 100);
  }
  CHECK(game.battle.state == H2_XIANXIA_RPG_BATTLE_DEFEAT);
  run(&game, 500);
  tap(&game, 320.0f, 240.0f);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_EXPLORE);
  CHECK(game.map == H2_XIANXIA_RPG_MAP_SHRINE);
  CHECK(game.tx == 5 && game.ty == 7);
  CHECK(game.hero.hp == game.hero.hp_max);
  CHECK(game.stage == H2_XIANXIA_RPG_STAGE_TO_SHRINE);
  /* Losing to the boss explains how to train and recover. */
  CHECK(game.dialogue.active &&
        game.dialogue.script ==
            h2_xianxia_rpg_script(H2_XIANXIA_RPG_SCRIPT_REVIVE_BOSS));
  finish_dialogue(&game);
  h2_xianxia_rpg_npc_t npcs[H2_XIANXIA_RPG_MAX_NPCS];
  CHECK(h2_xianxia_rpg_npcs(&game, H2_XIANXIA_RPG_MAP_SHRINE, npcs) == 1u);
  run(&game, 300);
  talk_to(&game, 7, 2);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_BATTLE && game.battle.boss);
  CHECK(game.battle.enemy_hp == game.battle.enemy_hp_max);
}

static void test_save_round_trip(void) {
  const h2_xianxia_rpg_save_t save = {.stage = 2u,
                                      .level = 4u,
                                      .exp = 99u,
                                      .pills = 3u,
                                      .flags = H2_XIANXIA_RPG_FLAG_PILLS_GIVEN,
                                      .map = H2_XIANXIA_RPG_MAP_SHRINE,
                                      .x = 5u,
                                      .y = 7u};
  uint32_t words[H2_XIANXIA_RPG_SAVE_WORDS];
  h2_xianxia_rpg_save_pack(&save, words);
  h2_xianxia_rpg_save_t loaded;
  CHECK(h2_xianxia_rpg_save_unpack(words, &loaded));
  CHECK(memcmp(&loaded, &save, sizeof(save)) == 0);
  /* Statistics round-trip; saves written before them decode with zero. */
  h2_xianxia_rpg_save_t counted = save;
  counted.battles = 7u;
  counted.turns = 31u;
  h2_xianxia_rpg_save_pack(&counted, words);
  CHECK(h2_xianxia_rpg_save_unpack(words, &loaded));
  CHECK(loaded.battles == 7u && loaded.turns == 31u);
  words[3] = 0u;
  CHECK(h2_xianxia_rpg_save_unpack(words, &loaded));
  CHECK(loaded.battles == 0u && loaded.turns == 0u && loaded.level == 4u);
  h2_xianxia_rpg_save_pack(&save, words);
  uint32_t bad[H2_XIANXIA_RPG_SAVE_WORDS];
  memcpy(bad, words, sizeof(bad));
  bad[0] ^= 0xFFu;
  CHECK(!h2_xianxia_rpg_save_unpack(bad, &loaded));
  memcpy(bad, words, sizeof(bad));
  bad[2] = (bad[2] & ~0xFFu) | 9u;
  CHECK(!h2_xianxia_rpg_save_unpack(bad, &loaded));
  memcpy(bad, words, sizeof(bad));
  bad[0] = (bad[0] & ~0xFF00u) | ((uint32_t)H2_XIANXIA_RPG_STAGE_CLEARED + 1u)
                                     << 8;
  CHECK(!h2_xianxia_rpg_save_unpack(bad, &loaded));
  memset(bad, 0, sizeof(bad));
  CHECK(!h2_xianxia_rpg_save_unpack(bad, &loaded));

  /* Continue restores the journey at the saved cell with full strength. */
  h2_xianxia_rpg_game_init(&game, 480, 800, 4u, &save);
  CHECK(game.has_save);
  run(&game, 300);
  tap_rect(&game, &game.layout.title_buttons[1]);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_EXPLORE);
  CHECK(game.map == H2_XIANXIA_RPG_MAP_SHRINE);
  CHECK(game.tx == 5 && game.ty == 7);
  CHECK(game.stage == H2_XIANXIA_RPG_STAGE_TO_SHRINE);
  CHECK(game.hero.level == 4u && game.hero.exp == 99u);
  CHECK(game.hero.pills == 3u);
  CHECK(game.hero.hp == game.hero.hp_max);

  /* A save after the boss continues with the shrine already cleared. */
  h2_xianxia_rpg_save_t sealed = save;
  sealed.stage = H2_XIANXIA_RPG_STAGE_RETURN;
  sealed.x = 7u;
  sealed.y = 3u;
  h2_xianxia_rpg_game_init(&game, 640, 480, 4u, &sealed);
  run(&game, 300);
  tap_rect(&game, &game.layout.title_buttons[1]);
  CHECK(game.stage == H2_XIANXIA_RPG_STAGE_RETURN);
  h2_xianxia_rpg_npc_t npcs[H2_XIANXIA_RPG_MAX_NPCS];
  CHECK(h2_xianxia_rpg_npcs(&game, H2_XIANXIA_RPG_MAP_SHRINE, npcs) == 0u);

  /* A save on a blocked cell falls back to the village. */
  h2_xianxia_rpg_save_t blocked = save;
  blocked.x = 0u;
  blocked.y = 0u;
  h2_xianxia_rpg_game_init(&game, 480, 800, 4u, &blocked);
  run(&game, 300);
  tap_rect(&game, &game.layout.title_buttons[1]);
  CHECK(game.map == H2_XIANXIA_RPG_MAP_VILLAGE);
}

static void test_utf8(void) {
  CHECK(h2_xianxia_rpg_utf8_length("竹溪") == 2u);
  CHECK(h2_xianxia_rpg_utf8_length("Lv3") == 3u);
  const char* text = "仙a";
  CHECK(h2_xianxia_rpg_utf8_next(&text) == 0x4ED9u);
  CHECK(h2_xianxia_rpg_utf8_next(&text) == 'a');
  CHECK(h2_xianxia_rpg_utf8_next(&text) == 0u);
  const char broken[] = {(char)0xE4, 'x', 0};
  text = broken;
  CHECK(h2_xianxia_rpg_utf8_next(&text) == 0xFFFDu);
  CHECK(h2_xianxia_rpg_utf8_next(&text) == 'x');
}

int main(void) {
  test_layouts();
  test_maps_are_consistent();
  test_title_guard_and_continue_disabled();
  test_walking();
  test_full_journey();
  test_battle_victory_and_growth();
  test_unavailable_commands();
  test_defeat_revives_at_rest_point();
  test_boss_defeat_and_retry();
  test_save_round_trip();
  test_utf8();
  printf("xianxia_rpg game tests passed\n");
  return 0;
}
