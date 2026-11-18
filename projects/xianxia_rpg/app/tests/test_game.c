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

/* Records as storage would hand them over with one chapter-1 save. */
static h2_xianxia_rpg_records_t with_chapter1(const h2_xianxia_rpg_save_t* s) {
  h2_xianxia_rpg_records_t r;
  memset(&r, 0, sizeof(r));
  uint32_t words[H2_XIANXIA_RPG_SAVE_WORDS];
  h2_xianxia_rpg_save_pack(s, words);
  r.has_chapter1 = h2_xianxia_rpg_save_unpack(words, &r.chapter1);
  return r;
}

static void init_with(h2_xianxia_rpg_game_t* g, int32_t w, int32_t h,
                      uint32_t seed, const h2_xianxia_rpg_save_t* s) {
  const h2_xianxia_rpg_records_t r = with_chapter1(s);
  CHECK(r.has_chapter1);
  h2_xianxia_rpg_game_init(g, w, h, seed, &r);
}

static void tap_continue(h2_xianxia_rpg_game_t* g) {
  run(g, 300);
  tap_rect(g, &g->layout.title_buttons[1]);
  /* Past the tap guard of the new screen. */
  run(g, 300);
}

/* Take the pending save of the current chapter's journey. */
static bool take_slot(h2_xianxia_rpg_game_t* g, h2_xianxia_rpg_save_t* out) {
  h2_xianxia_rpg_writes_t writes;
  if (!h2_xianxia_rpg_game_take_writes(g, &writes)) return false;
  if (writes.chapter1) *out = writes.chapter1_save;
  if (writes.journey) *out = writes.journey_save;
  if (writes.journey3) *out = writes.journey3_save;
  if (writes.journey4) *out = writes.journey4_save;
  return writes.chapter1 || writes.journey || writes.journey3 ||
         writes.journey4;
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
    for (int32_t b = 0; b < H2_XIANXIA_RPG_CHAPTER_ROWS; ++b)
      CHECK(rect_inside(&l->chapter_rows[b], w, h));
    CHECK(rect_inside(&l->chapter_panel, w, h));
    CHECK(rect_inside(&l->chapter_back, w, h));
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
  CHECK(h2_xianxia_rpg_continue_save(&game) == NULL);
  tap_rect(&game, &game.layout.title_buttons[1]);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_TITLE);
  /* The chapter screen opens from the title and leads back. */
  tap_rect(&game, &game.layout.title_buttons[2]);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_CHAPTERS);
  run(&game, 300);
  tap_rect(&game, &game.layout.chapter_rows[H2_XIANXIA_RPG_CHAPTERS_PLAYABLE]);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_CHAPTERS);
  tap_rect(&game, &game.layout.chapter_back);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_TITLE);
  /* The v0.1 places of the first two buttons are kept at 640x480. */
  CHECK(game.layout.title_buttons[0].y == 288);
  CHECK(game.layout.title_buttons[1].y == 350);
  /* Nothing is written without play. */
  h2_xianxia_rpg_writes_t writes;
  CHECK(!h2_xianxia_rpg_game_take_writes(&game, &writes));
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
  CHECK(take_slot(&game, &save));
  CHECK(save.stage == H2_XIANXIA_RPG_STAGE_SEEK_DAOIST);
  CHECK(!take_slot(&game, &save));
  talk_to(&game, 11, 3);
  CHECK(game.hero.pills == 3u);
  CHECK(game.flags & H2_XIANXIA_RPG_FLAG_PILLS_GIVEN);
  /* Resting at the well restores and records a save point. */
  game.hero.hp = 5u;
  talk_to(&game, 8, 4);
  CHECK(game.hero.hp == game.hero.hp_max);
  CHECK(take_slot(&game, &save));
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
  CHECK(take_slot(&game, &save));

  walk_to(&game, 7, 0);
  CHECK(game.map == H2_XIANXIA_RPG_MAP_SHRINE);
  CHECK(game.tx == 7 && game.ty == 8);
  game.encounter_steps = 100000u;
  talk_to(&game, 4, 7);
  CHECK(take_slot(&game, &save));
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
  CHECK(take_slot(&game, &save));
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
  h2_xianxia_rpg_writes_t ending;
  CHECK(h2_xianxia_rpg_game_take_writes(&game, &ending));
  save = ending.chapter1_save;
  CHECK(ending.chapter1 && !ending.journey && ending.progress);
  CHECK(save.stage == H2_XIANXIA_RPG_STAGE_CLEARED);
  CHECK(save.battles == game.battles_won && save.turns == game.turns_taken);
  CHECK(ending.progress_record.cleared == 1u);
  CHECK(ending.progress_record.chapters[0].level == game.hero.level);
  const uint32_t cleared_level = game.hero.level;
  const uint32_t cleared_pills = game.hero.pills;
  /* The ending ignores early taps, then leads straight into chapter 2. */
  run(&game, 300);
  tap(&game, 320.0f, 240.0f);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_ENDING);
  run(&game, 1500);
  tap(&game, 320.0f, 240.0f);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_PROLOGUE);
  CHECK(game.prologue_chapter == 2u && game.prologue_carry);
  run(&game, 1000);
  tap(&game, 320.0f, 240.0f);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_EXPLORE);
  CHECK(game.chapter == 2u && game.stage == H2_XIANXIA_RPG_STAGE_C2_ARRIVE);
  CHECK(game.map == H2_XIANXIA_RPG_MAP_DOCK);
  CHECK(game.hero.level == cleared_level && game.hero.pills == cleared_pills);
  CHECK(game.flags & H2_XIANXIA_RPG_FLAG_C1_CLEARED);
  CHECK(game.battles_won == 0u);
  h2_xianxia_rpg_writes_t opening;
  CHECK(h2_xianxia_rpg_game_take_writes(&game, &opening));
  CHECK(opening.journey && !opening.chapter1);
  CHECK(opening.journey_save.chapter == 2u);
  /* A reload of the chapter-1 record reopens the result, not the boss. */
  const uint32_t won = save.battles, turns = save.turns;
  init_with(&game, 640, 480, 99u, &save);
  tap_continue(&game);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_ENDING);
  CHECK(game.stage == H2_XIANXIA_RPG_STAGE_CLEARED);
  CHECK(game.hero.level == cleared_level);
  CHECK(game.battles_won == won && game.turns_taken == turns);
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
  init_with(&game, 640, 480, 12u, &save);
  tap_continue(&game);
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
  h2_xianxia_rpg_save_t expected = save;
  expected.chapter = 1u;
  expected.rest_map = save.map;
  expected.rest_x = save.x;
  expected.rest_y = save.y;
  CHECK(memcmp(&loaded, &expected, sizeof(save)) == 0);
  /* Chapter-2 maps never fit the v0.1 record. */
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

  memcpy(bad, words, sizeof(bad));
  bad[2] = (bad[2] & ~0xFFu) | (uint32_t)H2_XIANXIA_RPG_MAP_DOCK;
  CHECK(!h2_xianxia_rpg_save_unpack(bad, &loaded));

  /* Continue restores the journey at the saved cell with full strength. */
  init_with(&game, 480, 800, 4u, &save);
  CHECK(h2_xianxia_rpg_continue_save(&game) != NULL);
  tap_continue(&game);
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
  init_with(&game, 640, 480, 4u, &sealed);
  tap_continue(&game);
  CHECK(game.stage == H2_XIANXIA_RPG_STAGE_RETURN);
  h2_xianxia_rpg_npc_t npcs[H2_XIANXIA_RPG_MAX_NPCS];
  CHECK(h2_xianxia_rpg_npcs(&game, H2_XIANXIA_RPG_MAP_SHRINE, npcs) == 0u);

  /* A save on a blocked cell falls back to the village. */
  h2_xianxia_rpg_save_t blocked = save;
  blocked.x = 0u;
  blocked.y = 0u;
  init_with(&game, 480, 800, 4u, &blocked);
  tap_continue(&game);
  CHECK(game.map == H2_XIANXIA_RPG_MAP_VILLAGE);
}

static void test_utf8(void) {
  CHECK(h2_xianxia_rpg_utf8_length("竹溪") == 2u);
  CHECK(h2_xianxia_rpg_utf8_length("Lv3") == 3u);
  const char* text = "仙a";
  CHECK(h2_xianxia_rpg_utf8_next(&text) == 0x4ED9u);
  CHECK(h2_xianxia_rpg_utf8_next(&text) == 'a');
  CHECK(h2_xianxia_rpg_utf8_next(&text) == 0u);
  /* A lead byte of a three-byte sequence followed by ASCII. */
  const char broken[] =
      "\xE4"
      "x";
  text = broken;
  CHECK(h2_xianxia_rpg_utf8_next(&text) == 0xFFFDu);
  CHECK(h2_xianxia_rpg_utf8_next(&text) == 'x');
}

/* ---- Chapter 2 ---------------------------------------------------------- */

static void tap_center(h2_xianxia_rpg_game_t* g) {
  tap(g, (float)g->width * 0.5f, (float)g->height * 0.5f);
}

/* Back to the title the way a reload would show it. */
static void enter_phase_title(h2_xianxia_rpg_game_t* g) {
  const h2_xianxia_rpg_records_t r = g->records;
  h2_xianxia_rpg_game_init(g, g->width, g->height, 77u, &r);
}

/* Title -> chapters -> chapter 2 -> card -> dock. */
static void enter_chapter2(h2_xianxia_rpg_game_t* g) {
  run(g, 300);
  tap_rect(g, &g->layout.title_buttons[2]);
  CHECK(g->phase == H2_XIANXIA_RPG_PHASE_CHAPTERS);
  run(g, 300);
  tap_rect(g, &g->layout.chapter_rows[1]);
  CHECK(g->phase == H2_XIANXIA_RPG_PHASE_PROLOGUE);
  /* The card ignores early taps. */
  run(g, 300);
  tap_center(g);
  CHECK(g->phase == H2_XIANXIA_RPG_PHASE_PROLOGUE);
  run(g, 700);
  tap_center(g);
  CHECK(g->phase == H2_XIANXIA_RPG_PHASE_EXPLORE);
  CHECK(g->chapter == 2u && g->map == H2_XIANXIA_RPG_MAP_DOCK);
  CHECK(g->dialogue.active);
  finish_dialogue(g);
  g->encounter_steps = 100000u;
}

static h2_xianxia_rpg_command_t pick_c2(const h2_xianxia_rpg_game_t* g) {
  const h2_xianxia_rpg_battle_t* b = &g->battle;
  const bool joint =
      h2_xianxia_rpg_joint_ready(g) &&
      h2_xianxia_rpg_command_available(g, H2_XIANXIA_RPG_COMMAND_SKILL);
  if (b->charging)
    return joint ? H2_XIANXIA_RPG_COMMAND_SKILL : H2_XIANXIA_RPG_COMMAND_GUARD;
  if (g->hero.hp * 2u < g->hero.hp_max &&
      h2_xianxia_rpg_command_available(g, H2_XIANXIA_RPG_COMMAND_PILL))
    return H2_XIANXIA_RPG_COMMAND_PILL;
  if (joint) return H2_XIANXIA_RPG_COMMAND_SKILL;
  if (b->veiled) return H2_XIANXIA_RPG_COMMAND_GUARD;
  return H2_XIANXIA_RPG_COMMAND_ATTACK;
}

static void tap_shadow(h2_xianxia_rpg_game_t* g, uint32_t index) {
  tap_rect(g, &g->layout.shadows[index]);
}

typedef struct seen {
  bool veil, lantern, steady, joint, broke, sealed, split, target, wrong;
} seen_t;

/* Fight a companion battle with a sensible strategy; returns the end
 * state. The first split is answered with the wrong shadow once. */
static h2_xianxia_rpg_battle_state_t fight_c2(h2_xianxia_rpg_game_t* g,
                                              seen_t* seen) {
  CHECK(g->phase == H2_XIANXIA_RPG_PHASE_BATTLE);
  run(g, 300);
  bool wrong_done = false;
  for (int i = 0; i < 2000; ++i) {
    h2_xianxia_rpg_battle_t* b = &g->battle;
    if (b->veiled) seen->veil = true;
    if (b->mp_sealed) seen->sealed = true;
    if (b->split) seen->split = true;
    if (b->state == H2_XIANXIA_RPG_BATTLE_ALLY_ACT) {
      if (b->ally_action == H2_XIANXIA_RPG_ALLY_LANTERN) seen->lantern = true;
      if (b->ally_action == H2_XIANXIA_RPG_ALLY_STEADY) seen->steady = true;
    }
    if (b->state == H2_XIANXIA_RPG_BATTLE_TARGET) {
      seen->target = true;
      if (!wrong_done) {
        wrong_done = true;
        const uint32_t hp = b->enemy_hp, turns = g->turns_taken;
        tap_shadow(g, 1u - (uint32_t)b->real_shadow);
        CHECK(b->state == H2_XIANXIA_RPG_BATTLE_HERO_ACT);
        CHECK(b->enemy_hp >= hp && g->turns_taken == turns + 1u);
        CHECK(!b->split);
        seen->wrong = true;
      } else {
        tap_shadow(g, b->real_shadow);
        CHECK(b->state == H2_XIANXIA_RPG_BATTLE_HERO_ACT);
      }
    } else if (b->state == H2_XIANXIA_RPG_BATTLE_COMMAND) {
      const h2_xianxia_rpg_command_t command = pick_c2(g);
      const bool charging = b->charging;
      const bool joint = command == H2_XIANXIA_RPG_COMMAND_SKILL &&
                         h2_xianxia_rpg_joint_ready(g);
      tap_rect(g, &g->layout.commands[command]);
      if (joint && b->state == H2_XIANXIA_RPG_BATTLE_HERO_ACT) {
        seen->joint = true;
        CHECK(b->joint && b->rapport == 0u && !b->veiled);
        if (charging) {
          CHECK(!b->charging);
          seen->broke = true;
        }
      }
    } else if (b->state == H2_XIANXIA_RPG_BATTLE_VICTORY ||
               b->state == H2_XIANXIA_RPG_BATTLE_DEFEAT) {
      const h2_xianxia_rpg_battle_state_t state = b->state;
      run(g, 500);
      tap_center(g);
      CHECK(g->phase == H2_XIANXIA_RPG_PHASE_EXPLORE);
      return state;
    }
    run(g, 50);
  }
  CHECK(!"chapter-2 battle did not finish");
  return H2_XIANXIA_RPG_BATTLE_DEFEAT;
}

/* Lose the current battle by standing still at 1 HP. */
static void lose_battle(h2_xianxia_rpg_game_t* g) {
  CHECK(g->phase == H2_XIANXIA_RPG_PHASE_BATTLE);
  run(g, 1000);
  for (int i = 0; i < 400 && g->battle.state != H2_XIANXIA_RPG_BATTLE_DEFEAT;
       ++i) {
    if (g->battle.state == H2_XIANXIA_RPG_BATTLE_COMMAND) {
      g->hero.hp = 1u;
      g->hero.pills = 0u;
      tap_rect(g, &g->layout.commands[H2_XIANXIA_RPG_COMMAND_GUARD]);
    }
    run(g, 100);
  }
  CHECK(g->battle.state == H2_XIANXIA_RPG_BATTLE_DEFEAT);
  run(g, 500);
  tap_center(g);
  CHECK(g->phase == H2_XIANXIA_RPG_PHASE_EXPLORE);
}

/* The teaching fight on the pier: veil, lantern, rapport and a joint
 * strike, and neither side can fall early. */
static void play_tutorial(h2_xianxia_rpg_game_t* g) {
  CHECK(g->phase == H2_XIANXIA_RPG_PHASE_BATTLE);
  h2_xianxia_rpg_battle_t* b = &g->battle;
  CHECK(b->tutorial && b->ally && b->veiled);
  CHECK(b->enemy == H2_XIANXIA_RPG_ENEMY_FOG_GHOST);
  CHECK(!h2_xianxia_rpg_command_available(g, H2_XIANXIA_RPG_COMMAND_FLEE));
  CHECK(h2_xianxia_rpg_ally_intent(g) == H2_XIANXIA_RPG_ALLY_LANTERN);
  run(g, 1000);
  CHECK(b->state == H2_XIANXIA_RPG_BATTLE_COMMAND);
  /* A veiled hit is weak and earns no rapport; the lantern strips it. */
  const uint32_t hp = b->enemy_hp;
  tap_rect(g, &g->layout.commands[H2_XIANXIA_RPG_COMMAND_ATTACK]);
  CHECK(b->hits[0] * 100u <= (hp - b->hits[0]) * 100u);
  CHECK(b->rapport == 0u);
  run(g, H2_XIANXIA_RPG_ACT_MS + 20u);
  CHECK(b->state == H2_XIANXIA_RPG_BATTLE_ALLY_ACT);
  CHECK(b->ally_action == H2_XIANXIA_RPG_ALLY_LANTERN);
  CHECK(!b->veiled && b->rapport == 1u);
  bool joint = false;
  for (int i = 0; i < 400 && b->state != H2_XIANXIA_RPG_BATTLE_VICTORY; ++i) {
    CHECK(g->hero.hp >= 1u);
    CHECK(b->state != H2_XIANXIA_RPG_BATTLE_DEFEAT);
    if (b->state == H2_XIANXIA_RPG_BATTLE_COMMAND) {
      if (h2_xianxia_rpg_joint_ready(g)) {
        CHECK(
            h2_xianxia_rpg_command_available(g, H2_XIANXIA_RPG_COMMAND_SKILL));
        tap_rect(g, &g->layout.commands[H2_XIANXIA_RPG_COMMAND_SKILL]);
        CHECK(b->joint);
        joint = true;
      } else {
        /* Before the joint strike the ghost keeps a wisp of life. */
        g->hero.attack = 400u;
        tap_rect(g, &g->layout.commands[H2_XIANXIA_RPG_COMMAND_ATTACK]);
      }
    }
    run(g, 100);
    if (!joint) CHECK(b->enemy_hp >= 1u);
  }
  CHECK(joint && b->state == H2_XIANXIA_RPG_BATTLE_VICTORY);
  h2_xianxia_rpg_hero_at_level(&g->hero, g->hero.level);
  run(g, 500);
  tap_center(g);
  CHECK(g->phase == H2_XIANXIA_RPG_PHASE_EXPLORE && g->dialogue.active);
  finish_dialogue(g);
}

static void test_chapter2_defaults_and_full_run(void) {
  h2_xianxia_rpg_game_init(&game, 640, 480, 31u, NULL);
  enter_chapter2(&game);
  /* Without a chapter-1 record the chapter starts from its defaults. */
  CHECK(!game.prologue_carry);
  CHECK(game.hero.level == 4u && game.hero.pills == 3u);
  CHECK(h2_xianxia_rpg_command_available(&game, H2_XIANXIA_RPG_COMMAND_SKILL));
  CHECK(!(game.flags & H2_XIANXIA_RPG_FLAG_C1_CLEARED));
  CHECK(game.stage == H2_XIANXIA_RPG_STAGE_C2_ARRIVE);
  h2_xianxia_rpg_writes_t writes;
  CHECK(h2_xianxia_rpg_game_take_writes(&game, &writes));
  CHECK(writes.journey && !writes.chapter1 && !writes.progress);
  CHECK(writes.journey_save.stage == H2_XIANXIA_RPG_STAGE_C2_ARRIVE);
  /* The reed flats stay closed until the companion joins. */
  walk_to(&game, 0, 3);
  CHECK(game.map == H2_XIANXIA_RPG_MAP_DOCK && game.dialogue.active);
  CHECK(game.dialogue.script ==
        h2_xianxia_rpg_script(H2_XIANXIA_RPG_SCRIPT_C2_DOCK_CLOSED));
  finish_dialogue(&game);
  talk_to(&game, 5, 4);
  CHECK(game.stage == H2_XIANXIA_RPG_STAGE_C2_FIND_JIANGLI);
  CHECK(take_slot(&game, &writes.journey_save));
  /* The tea stall rests, saves and tells the rumour. */
  game.hero.hp = 3u;
  talk_to(&game, 13, 2);
  CHECK(game.hero.hp == game.hero.hp_max);
  CHECK(game.flags & H2_XIANXIA_RPG_FLAG_C2_RUMOR);
  CHECK(game.rest_map == H2_XIANXIA_RPG_MAP_DOCK);
  CHECK(take_slot(&game, &writes.journey_save));
  /* Jiangli on the pier, then the teaching fight. */
  tap_tile(&game, 9, 8);
  settle(&game);
  CHECK(game.tx == 9 && game.ty == 7);
  finish_dialogue(&game);
  play_tutorial(&game);
  CHECK(game.stage == H2_XIANXIA_RPG_STAGE_C2_TO_REEDS);
  CHECK(game.ally_visible);
  CHECK(take_slot(&game, &writes.journey_save));
  h2_xianxia_rpg_npc_t npcs[H2_XIANXIA_RPG_MAX_NPCS];
  CHECK(h2_xianxia_rpg_npcs(&game, H2_XIANXIA_RPG_MAP_DOCK, npcs) == 3u);

  /* Reed flats: the fog walls hold until their lamp posts are lit. */
  walk_to(&game, 0, 3);
  CHECK(game.map == H2_XIANXIA_RPG_MAP_REEDS && game.tx == 13);
  CHECK(game.dialogue.active);
  finish_dialogue(&game);
  CHECK(game.flags & H2_XIANXIA_RPG_FLAG_C2_REEDS_TIP);
  game.encounter_steps = 100000u;
  CHECK(!h2_xianxia_rpg_walkable(&game, game.map, 10, 4));
  tap_tile(&game, 8, 4);
  CHECK(game.path_len == 0u);
  /* Paths go around the tall reeds when they can. */
  tap_tile(&game, 13, 7);
  for (uint32_t i = 0; i < game.path_len; ++i)
    CHECK(h2_xianxia_rpg_tile(game.map, game.path_x[i], game.path_y[i]) != 'v');
  settle(&game);
  talk_to(&game, 12, 7);
  CHECK(game.flags & H2_XIANXIA_RPG_FLAG_C2_POST1);
  CHECK(h2_xianxia_rpg_walkable(&game, game.map, 10, 4));
  CHECK(game.rest_map == H2_XIANXIA_RPG_MAP_REEDS && game.rest_x == game.tx &&
        game.rest_y == game.ty);
  CHECK(take_slot(&game, &writes.journey_save));
  CHECK(writes.journey_save.flags & H2_XIANXIA_RPG_FLAG_C2_POST1);
  talk_to(&game, 12, 1);
  CHECK(game.flags & H2_XIANXIA_RPG_FLAG_C2_FISHER);
  talk_to(&game, 8, 5);
  CHECK(game.flags & H2_XIANXIA_RPG_FLAG_C2_POST2);
  CHECK(h2_xianxia_rpg_walkable(&game, game.map, 5, 4));
  /* The cut line: without a chapter-1 clear there is no talisman remark. */
  CHECK(h2_xianxia_rpg_tile_script(&game, 8, 1) ==
        H2_XIANXIA_RPG_SCRIPT_C2_ROPE);
  tap_tile(&game, 8, 1);
  settle(&game);
  finish_dialogue(&game);
  /* Both clues together raise the companion's trust once. */
  CHECK(game.trust == 1u && (game.flags & H2_XIANXIA_RPG_FLAG_C2_TRUST));
  const uint32_t pills = game.hero.pills;
  talk_to(&game, 7, 8);
  CHECK(game.hero.pills == pills + 1u);
  talk_to(&game, 7, 8);
  CHECK(game.hero.pills == pills + 1u);
  /* The boat, its lamp, and the crossing. */
  talk_to(&game, 2, 5);
  CHECK(game.stage == H2_XIANXIA_RPG_STAGE_C2_TO_WRECK);
  CHECK(game.flags & H2_XIANXIA_RPG_FLAG_C2_BOAT);
  CHECK(take_slot(&game, &writes.journey_save));
  tap_tile(&game, 2, 5);
  settle(&game);
  finish_dialogue(&game);
  CHECK(game.map == H2_XIANXIA_RPG_MAP_WRECK && game.tx == 2 && game.ty == 5);
  CHECK(game.stage == H2_XIANXIA_RPG_STAGE_C2_WRECK);
  finish_dialogue(&game);
  CHECK(game.flags & H2_XIANXIA_RPG_FLAG_C2_POST4);
  CHECK(game.rest_map == H2_XIANXIA_RPG_MAP_WRECK && game.rest_x == 5 &&
        game.rest_y == 3);
  CHECK(take_slot(&game, &writes.journey_save));
  CHECK(writes.journey_save.rest_x == 5u && writes.journey_save.rest_y == 3u);
  /* No way back before the lamp is relit. */
  talk_to(&game, 1, 5);
  CHECK(game.map == H2_XIANXIA_RPG_MAP_WRECK);

  /* The hatch guard, then the wraith. */
  h2_xianxia_rpg_hero_at_level(&game.hero, 6u);
  game.hero.pills = 5u;
  tap_tile(&game, 6, 5);
  settle(&game);
  finish_dialogue(&game);
  seen_t seen;
  memset(&seen, 0, sizeof(seen));
  CHECK(game.battle.enemy == H2_XIANXIA_RPG_ENEMY_WRECK_GUARD);
  CHECK(game.battle.ally && game.battle.rapport == 1u);
  CHECK(fight_c2(&game, &seen) == H2_XIANXIA_RPG_BATTLE_VICTORY);
  CHECK(game.flags & H2_XIANXIA_RPG_FLAG_C2_GUARD);
  CHECK(take_slot(&game, &writes.journey_save));
  finish_dialogue(&game);
  CHECK(h2_xianxia_rpg_npcs(&game, H2_XIANXIA_RPG_MAP_WRECK, npcs) == 0u);
  tap_tile(&game, 12, 2);
  settle(&game);
  finish_dialogue(&game);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_BATTLE && game.battle.boss);
  CHECK(game.battle.enemy == H2_XIANXIA_RPG_ENEMY_FOG_WRAITH);
  memset(&seen, 0, sizeof(seen));
  game.hero.pills = 9u;
  CHECK(fight_c2(&game, &seen) == H2_XIANXIA_RPG_BATTLE_VICTORY);
  CHECK(seen.veil && seen.lantern && seen.joint && seen.split && seen.target &&
        seen.wrong);
  CHECK(seen.broke || seen.sealed);
  CHECK(game.stage == H2_XIANXIA_RPG_STAGE_C2_RETURN);
  CHECK(game.flags & H2_XIANXIA_RPG_FLAG_C2_HAND_LIGHT);
  CHECK(take_slot(&game, &writes.journey_save));
  CHECK(writes.journey_save.stage == H2_XIANXIA_RPG_STAGE_C2_RETURN);
  CHECK(writes.journey_save.map == H2_XIANXIA_RPG_MAP_WRECK);
  /* The relit lamp, the way home and the report. */
  CHECK(game.dialogue.active);
  finish_dialogue(&game);
  CHECK(game.map == H2_XIANXIA_RPG_MAP_DOCK && game.tx == 9 && game.ty == 6);
  CHECK(take_slot(&game, &writes.journey_save));
  CHECK(writes.journey_save.map == H2_XIANXIA_RPG_MAP_DOCK);
  talk_to(&game, 9, 9);
  talk_to(&game, 5, 4);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_ENDING);
  CHECK(game.stage == H2_XIANXIA_RPG_STAGE_C2_CLEARED);
  CHECK(h2_xianxia_rpg_game_take_writes(&game, &writes));
  CHECK(writes.journey && writes.progress && !writes.chapter1);
  CHECK(writes.progress_record.cleared == 2u);
  CHECK(writes.progress_record.chapters[1].level == game.hero.level);
  CHECK(writes.progress_record.chapters[1].trust == 1u);
  CHECK(writes.progress_record.chapters[1].flags &
        H2_XIANXIA_RPG_FLAG_C2_HAND_LIGHT);
  const uint32_t won = game.battles_won, turns = game.turns_taken;
  CHECK(won >= 3u);
  run(&game, 1600);
  tap_center(&game);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_PROLOGUE);
  CHECK(game.prologue_chapter == 3u && game.prologue_carry);
  /* Reloaded, the chapter reopens its result and is marked cleared. */
  h2_xianxia_rpg_records_t r;
  memset(&r, 0, sizeof(r));
  r.has_journey = true;
  r.journey = writes.journey_save;
  r.journey_latest = true;
  r.progress = writes.progress_record;
  h2_xianxia_rpg_game_init(&game, 640, 480, 5u, &r);
  CHECK(h2_xianxia_rpg_unfinished(&game, 2u) == NULL);
  tap_continue(&game);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_ENDING && game.chapter == 2u);
  CHECK(game.battles_won == won && game.turns_taken == turns);
  CHECK(!h2_xianxia_rpg_game_take_writes(&game, &writes));
}

/* A v0.1 player who finished chapter 1 continues straight into chapter 2. */
static void test_upgrade_from_cleared_v01(void) {
  const h2_xianxia_rpg_save_t cleared = {
      .stage = H2_XIANXIA_RPG_STAGE_CLEARED,
      .level = 4u,
      .exp = 99u,
      .pills = 2u,
      .flags = H2_XIANXIA_RPG_FLAG_PILLS_GIVEN,
      .map = H2_XIANXIA_RPG_MAP_VILLAGE,
      .x = 4u,
      .y = 4u,
      .battles = 6u,
      .turns = 39u};
  init_with(&game, 640, 480, 41u, &cleared);
  /* The clear is copied into the chapter record; save0..3 stay as they
   * are. */
  h2_xianxia_rpg_writes_t writes;
  CHECK(h2_xianxia_rpg_game_take_writes(&game, &writes));
  CHECK(writes.progress && !writes.chapter1 && !writes.journey);
  CHECK(writes.progress_record.cleared == 1u);
  const h2_xianxia_rpg_clear_t* c1 = &writes.progress_record.chapters[0];
  CHECK(c1->level == 4u && c1->exp == 99u && c1->pills == 2u);
  CHECK(c1->battles == 6u && c1->turns == 39u);
  CHECK(!h2_xianxia_rpg_game_take_writes(&game, &writes));
  /* Continue reopens the chapter-1 result, then flows into chapter 2. */
  tap_continue(&game);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_ENDING && game.chapter == 1u);
  CHECK(game.battles_won == 6u && game.turns_taken == 39u);
  run(&game, 1600);
  tap_center(&game);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_PROLOGUE && game.prologue_carry);
  run(&game, 1000);
  tap_center(&game);
  CHECK(game.chapter == 2u && game.hero.level == 4u && game.hero.exp == 99u);
  CHECK(game.hero.pills == 2u);
  CHECK(game.flags & H2_XIANXIA_RPG_FLAG_C1_CLEARED);
  CHECK(!(game.flags & H2_XIANXIA_RPG_FLAG_PILLS_GIVEN));
  CHECK(h2_xianxia_rpg_tile_script(&game, 0, 0) == H2_XIANXIA_RPG_SCRIPT_COUNT);
  game.map = H2_XIANXIA_RPG_MAP_REEDS;
  CHECK(h2_xianxia_rpg_tile_script(&game, 8, 1) ==
        H2_XIANXIA_RPG_SCRIPT_C2_ROPE_C1);
  game.map = H2_XIANXIA_RPG_MAP_DOCK;
  CHECK(h2_xianxia_rpg_game_take_writes(&game, &writes));
  CHECK(writes.journey && !writes.chapter1);

  /* An unreadable chapter blob must not be replaced automatically, but
   * the valid legacy clear still carries into a player-started chapter 2. */
  h2_xianxia_rpg_records_t damaged = game.records;
  memset(&damaged.progress, 0, sizeof(damaged.progress));
  damaged.progress_unreadable = damaged.unreadable = true;
  damaged.has_progress = false;
  damaged.has_journey = damaged.journey_latest = false;
  h2_xianxia_rpg_game_init(&game, 640, 480, 41u, &damaged);
  CHECK(!h2_xianxia_rpg_game_take_writes(&game, &writes));
  enter_chapter2(&game);
  CHECK(game.hero.level == 4u && game.hero.pills == 2u);
  CHECK(game.flags & H2_XIANXIA_RPG_FLAG_C1_CLEARED);
  CHECK(h2_xianxia_rpg_game_take_writes(&game, &writes));
  CHECK(writes.journey && !writes.progress && !writes.chapter1);

  /* A chapter-1 run still in progress is not a clear: chapter 2 then
   * starts from its defaults and leaves that run resumable. */
  h2_xianxia_rpg_save_t midway = cleared;
  midway.stage = H2_XIANXIA_RPG_STAGE_TO_SHRINE;
  midway.map = H2_XIANXIA_RPG_MAP_FOREST;
  midway.x = 4u;
  init_with(&game, 640, 480, 42u, &midway);
  CHECK(!h2_xianxia_rpg_game_take_writes(&game, &writes));
  CHECK(h2_xianxia_rpg_unfinished(&game, 1u) != NULL);
  enter_chapter2(&game);
  CHECK(game.hero.level == 4u && game.hero.pills == 3u);
  CHECK(!(game.flags & H2_XIANXIA_RPG_FLAG_C1_CLEARED));
  CHECK(h2_xianxia_rpg_game_take_writes(&game, &writes));
  CHECK(writes.journey && !writes.chapter1);
  /* Continue now resumes chapter 2; the chapter row still resumes 1. */
  enter_phase_title(&game);
  CHECK(h2_xianxia_rpg_continue_save(&game)->chapter == 2u);
  run(&game, 300);
  tap_rect(&game, &game.layout.title_buttons[2]);
  run(&game, 300);
  tap_rect(&game, &game.layout.chapter_rows[0]);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_EXPLORE && game.chapter == 1u);
  CHECK(game.map == H2_XIANXIA_RPG_MAP_FOREST &&
        game.stage == H2_XIANXIA_RPG_STAGE_TO_SHRINE);
}

/* Which journey Continue resumes after v0.1 changed save0..3 again. */
static void test_continue_follows_latest_save(void) {
  const h2_xianxia_rpg_save_t c1 = {.stage = H2_XIANXIA_RPG_STAGE_SEEK_DAOIST,
                                    .level = 2u,
                                    .exp = 20u,
                                    .pills = 3u,
                                    .flags = 1u,
                                    .map = H2_XIANXIA_RPG_MAP_VILLAGE,
                                    .x = 8u,
                                    .y = 4u};
  h2_xianxia_rpg_records_t r = with_chapter1(&c1);
  r.has_journey = true;
  r.journey = (h2_xianxia_rpg_save_t){
      .chapter = 2u,
      .stage = H2_XIANXIA_RPG_STAGE_C2_TO_REEDS,
      .level = 5u,
      .exp = 160u,
      .pills = 4u,
      .flags = H2_XIANXIA_RPG_FLAG_C2_POST1 | H2_XIANXIA_RPG_FLAG_C2_REEDS_TIP,
      .map = H2_XIANXIA_RPG_MAP_REEDS,
      .x = 12u,
      .y = 6u,
      .rest_map = H2_XIANXIA_RPG_MAP_REEDS,
      .rest_x = 11u,
      .rest_y = 7u};
  r.journey_latest = true;
  h2_xianxia_rpg_game_init(&game, 640, 480, 51u, &r);
  CHECK(h2_xianxia_rpg_continue_save(&game)->chapter == 2u);
  tap_continue(&game);
  CHECK(game.chapter == 2u && game.map == H2_XIANXIA_RPG_MAP_REEDS);
  CHECK(game.tx == 12 && game.ty == 6);
  CHECK(game.rest_x == 11 && game.rest_y == 7);
  CHECK(game.ally_visible);
  CHECK(h2_xianxia_rpg_walkable(&game, game.map, 10, 4));
  CHECK(!h2_xianxia_rpg_walkable(&game, game.map, 5, 4));
  /* v0.1 played on: save0..3 changed after the journey was written. */
  r.journey_latest = false;
  h2_xianxia_rpg_game_init(&game, 640, 480, 52u, &r);
  CHECK(h2_xianxia_rpg_continue_save(&game)->chapter == 1u);
  run(&game, 300);
  tap_rect(&game, &game.layout.title_buttons[2]);
  run(&game, 300);
  /* The unfinished chapter 2 is still one tap away, and can restart. */
  CHECK(h2_xianxia_rpg_unfinished(&game, 2u) != NULL);
  tap_rect(&game, &game.layout.chapter_restart[1]);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_PROLOGUE);
  enter_phase_title(&game);
  run(&game, 300);
  tap_rect(&game, &game.layout.title_buttons[2]);
  run(&game, 300);
  tap_rect(&game, &game.layout.chapter_rows[1]);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_EXPLORE && game.chapter == 2u);
  CHECK(game.map == H2_XIANXIA_RPG_MAP_REEDS);
  /* A new chapter-1 run makes chapter 1 the latest once it saves. */
  h2_xianxia_rpg_writes_t writes;
  CHECK(!h2_xianxia_rpg_game_take_writes(&game, &writes));
  r.journey_latest = true;
  h2_xianxia_rpg_game_init(&game, 640, 480, 53u, &r);
  run(&game, 300);
  tap_rect(&game, &game.layout.title_buttons[0]);
  finish_dialogue(&game);
  run(&game, 300);
  game.encounter_steps = 100000u;
  talk_to(&game, 4, 3);
  CHECK(h2_xianxia_rpg_game_take_writes(&game, &writes));
  CHECK(writes.chapter1 && !writes.journey && !writes.progress);
  CHECK(h2_xianxia_rpg_continue_save(&game)->chapter == 1u);
  CHECK(h2_xianxia_rpg_unfinished(&game, 2u) != NULL);
}

/* Defeat returns to the latest lamp or rest point and keeps progress. */
static void test_chapter2_defeat_fallbacks(void) {
  h2_xianxia_rpg_records_t r;
  memset(&r, 0, sizeof(r));
  r.has_journey = true;
  r.journey_latest = true;
  /* Reed flats with the first post lit: revive beside it. */
  r.journey = (h2_xianxia_rpg_save_t){
      .chapter = 2u,
      .stage = H2_XIANXIA_RPG_STAGE_C2_TO_REEDS,
      .level = 4u,
      .exp = 90u,
      .pills = 0u,
      .flags = H2_XIANXIA_RPG_FLAG_C2_POST1 | H2_XIANXIA_RPG_FLAG_C2_REEDS_TIP |
               H2_XIANXIA_RPG_FLAG_C2_FISHER,
      .map = H2_XIANXIA_RPG_MAP_REEDS,
      .x = 13u,
      .y = 6u,
      .rest_map = H2_XIANXIA_RPG_MAP_REEDS,
      .rest_x = 13u,
      .rest_y = 7u};
  h2_xianxia_rpg_game_init(&game, 640, 480, 61u, &r);
  tap_continue(&game);
  game.encounter_steps = 1u;
  tap_tile(&game, 12, 6);
  for (int i = 0; i < 100 && game.phase != H2_XIANXIA_RPG_PHASE_BATTLE; ++i)
    run(&game, 20);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_BATTLE);
  CHECK(game.battle.ally);
  lose_battle(&game);
  CHECK(game.map == H2_XIANXIA_RPG_MAP_REEDS && game.tx == 13 && game.ty == 7);
  CHECK(game.hero.hp == game.hero.hp_max);
  CHECK(game.stage == H2_XIANXIA_RPG_STAGE_C2_TO_REEDS);
  CHECK(game.flags & H2_XIANXIA_RPG_FLAG_C2_FISHER);
  CHECK(h2_xianxia_rpg_walkable(&game, game.map, 10, 4));
  h2_xianxia_rpg_writes_t writes;
  CHECK(!h2_xianxia_rpg_game_take_writes(&game, &writes));
  finish_dialogue(&game);
  /* No post lit yet: back to the tea stall on the dock. */
  r.journey.flags = H2_XIANXIA_RPG_FLAG_C2_REEDS_TIP;
  r.journey.x = 13u;
  r.journey.y = 4u;
  r.journey.rest_map = H2_XIANXIA_RPG_MAP_DOCK;
  r.journey.rest_x = 13u;
  r.journey.rest_y = 3u;
  h2_xianxia_rpg_game_init(&game, 640, 480, 62u, &r);
  tap_continue(&game);
  game.encounter_steps = 1u;
  tap_tile(&game, 12, 3);
  for (int i = 0; i < 100 && game.phase != H2_XIANXIA_RPG_PHASE_BATTLE; ++i)
    run(&game, 20);
  lose_battle(&game);
  CHECK(game.map == H2_XIANXIA_RPG_MAP_DOCK && game.tx == 13 && game.ty == 3);
  finish_dialogue(&game);
  /* The hatch guard: back at the bow lamp, the guard still there. */
  r.journey = (h2_xianxia_rpg_save_t){
      .chapter = 2u,
      .stage = H2_XIANXIA_RPG_STAGE_C2_WRECK,
      .level = 4u,
      .exp = 120u,
      .pills = 1u,
      .flags = H2_XIANXIA_RPG_FLAG_C2_POST1 | H2_XIANXIA_RPG_FLAG_C2_POST2 |
               H2_XIANXIA_RPG_FLAG_C2_BOAT | H2_XIANXIA_RPG_FLAG_C2_POST4,
      .map = H2_XIANXIA_RPG_MAP_WRECK,
      .x = 2u,
      .y = 5u,
      .rest_map = H2_XIANXIA_RPG_MAP_WRECK,
      .rest_x = 5u,
      .rest_y = 3u};
  h2_xianxia_rpg_game_init(&game, 640, 480, 63u, &r);
  tap_continue(&game);
  CHECK(game.map == H2_XIANXIA_RPG_MAP_WRECK);
  tap_tile(&game, 6, 5);
  settle(&game);
  finish_dialogue(&game);
  CHECK(game.battle.enemy == H2_XIANXIA_RPG_ENEMY_WRECK_GUARD);
  lose_battle(&game);
  CHECK(game.tx == 5 && game.ty == 3);
  h2_xianxia_rpg_npc_t npcs[H2_XIANXIA_RPG_MAX_NPCS];
  CHECK(h2_xianxia_rpg_npcs(&game, H2_XIANXIA_RPG_MAP_WRECK, npcs) == 1u);
  finish_dialogue(&game);
  /* The wraith: a fresh fight from the bow lamp, explained once more. */
  r.journey.flags |=
      H2_XIANXIA_RPG_FLAG_C2_GUARD | H2_XIANXIA_RPG_FLAG_C2_TRUST;
  r.journey.trust = 1u;
  r.journey.x = 11u;
  r.journey.y = 2u;
  h2_xianxia_rpg_game_init(&game, 640, 480, 64u, &r);
  tap_continue(&game);
  talk_to(&game, 12, 2);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_BATTLE && game.battle.boss);
  CHECK(game.battle.rapport == 1u);
  CHECK(!h2_xianxia_rpg_command_available(&game, H2_XIANXIA_RPG_COMMAND_FLEE));
  /* Drive it into phase two before losing. */
  run(&game, 1000);
  game.battle.enemy_hp = game.battle.enemy_hp_max / 3u;
  lose_battle(&game);
  CHECK(game.tx == 5 && game.ty == 3);
  CHECK(game.dialogue.active &&
        game.dialogue.script ==
            h2_xianxia_rpg_script(H2_XIANXIA_RPG_SCRIPT_C2_REVIVE_BOSS));
  finish_dialogue(&game);
  CHECK(game.stage == H2_XIANXIA_RPG_STAGE_C2_WRECK);
  talk_to(&game, 12, 2);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_BATTLE);
  CHECK(game.battle.enemy_hp == game.battle.enemy_hp_max);
  CHECK(!game.battle.phase_two && !game.battle.split);
  CHECK(game.battle.rapport == 1u);
}

static void wait_command(h2_xianxia_rpg_game_t* g) {
  for (int i = 0; i < 200 && g->battle.state != H2_XIANXIA_RPG_BATTLE_COMMAND;
       ++i)
    run(g, 50);
  CHECK(g->battle.state == H2_XIANXIA_RPG_BATTLE_COMMAND);
}

/* The mechanics of one wraith fight, step by step. */
static void test_wraith_pattern(void) {
  h2_xianxia_rpg_records_t r;
  memset(&r, 0, sizeof(r));
  r.has_journey = true;
  r.journey_latest = true;
  r.journey = (h2_xianxia_rpg_save_t){
      .chapter = 2u,
      .stage = H2_XIANXIA_RPG_STAGE_C2_WRECK,
      .level = 5u,
      .exp = 150u,
      .pills = 3u,
      .flags = H2_XIANXIA_RPG_FLAG_C2_POST4 | H2_XIANXIA_RPG_FLAG_C2_GUARD,
      .map = H2_XIANXIA_RPG_MAP_WRECK,
      .x = 11u,
      .y = 2u,
      .rest_map = H2_XIANXIA_RPG_MAP_WRECK,
      .rest_x = 5u,
      .rest_y = 3u};
  h2_xianxia_rpg_game_init(&game, 640, 480, 71u, &r);
  tap_continue(&game);
  talk_to(&game, 12, 2);
  h2_xianxia_rpg_battle_t* b = &game.battle;
  run(&game, 1000);
  CHECK(b->state == H2_XIANXIA_RPG_BATTLE_COMMAND && !b->veiled);
  /* Turn 1: the wraith veils itself. */
  tap_rect(&game, &game.layout.commands[H2_XIANXIA_RPG_COMMAND_GUARD]);
  run(&game, 2600);
  CHECK(b->state == H2_XIANXIA_RPG_BATTLE_COMMAND && b->veiled);
  CHECK(h2_xianxia_rpg_ally_intent(&game) == H2_XIANXIA_RPG_ALLY_LANTERN);
  /* Guarding while veiled: the lantern comes first. */
  tap_rect(&game, &game.layout.commands[H2_XIANXIA_RPG_COMMAND_GUARD]);
  run(&game, H2_XIANXIA_RPG_ACT_MS + 20u);
  CHECK(b->ally_action == H2_XIANXIA_RPG_ALLY_LANTERN && !b->veiled);
  run(&game, 2000);
  /* Turn 3 gathers the mist lock; a joint strike breaks it. */
  tap_rect(&game, &game.layout.commands[H2_XIANXIA_RPG_COMMAND_ATTACK]);
  run(&game, 2600);
  CHECK(b->charging);
  b->rapport = H2_XIANXIA_RPG_RAPPORT_MAX;
  CHECK(h2_xianxia_rpg_joint_ready(&game));
  tap_rect(&game, &game.layout.commands[H2_XIANXIA_RPG_COMMAND_SKILL]);
  CHECK(b->joint && !b->charging && b->rapport == 0u);
  run(&game, 2600);
  CHECK(!game.battle.mp_sealed);
  /* Unbroken, the lock lands and seals the sword arts for a command. */
  for (int i = 0; i < 20 && !b->charging; ++i) {
    if (b->state == H2_XIANXIA_RPG_BATTLE_COMMAND)
      tap_rect(&game, &game.layout.commands[H2_XIANXIA_RPG_COMMAND_GUARD]);
    run(&game, 400);
  }
  CHECK(b->charging);
  wait_command(&game);
  tap_rect(&game, &game.layout.commands[H2_XIANXIA_RPG_COMMAND_GUARD]);
  run(&game, 2600);
  CHECK(b->mp_sealed && !b->charging);
  CHECK(!h2_xianxia_rpg_command_available(&game, H2_XIANXIA_RPG_COMMAND_SKILL));
  tap_rect(&game, &game.layout.commands[H2_XIANXIA_RPG_COMMAND_SKILL]);
  CHECK(b->state == H2_XIANXIA_RPG_BATTLE_COMMAND);
  CHECK(strcmp(b->message, h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_SEALED]) ==
        0);
  tap_rect(&game, &game.layout.commands[H2_XIANXIA_RPG_COMMAND_GUARD]);
  CHECK(!b->mp_sealed);
  run(&game, 2600);
  /* Below half health it splits; strikes then wait for a shadow. */
  b->enemy_hp = b->enemy_hp_max / 2u - 1u;
  tap_rect(&game, &game.layout.commands[H2_XIANXIA_RPG_COMMAND_GUARD]);
  run(&game, 2600);
  CHECK(b->phase_two && b->split);
  tap_rect(&game, &game.layout.commands[H2_XIANXIA_RPG_COMMAND_ATTACK]);
  CHECK(b->state == H2_XIANXIA_RPG_BATTLE_TARGET);
  /* Another command can still be chosen instead. */
  tap_rect(&game, &game.layout.commands[H2_XIANXIA_RPG_COMMAND_GUARD]);
  CHECK(b->state == H2_XIANXIA_RPG_BATTLE_HERO_ACT && b->guarding);
  run(&game, 2600);
  CHECK(b->split);
  const uint32_t hp = b->enemy_hp;
  tap_rect(&game, &game.layout.commands[H2_XIANXIA_RPG_COMMAND_ATTACK]);
  tap_shadow(&game, b->real_shadow);
  CHECK(b->state == H2_XIANXIA_RPG_BATTLE_HERO_ACT && b->hit_count == 1u);
  run(&game, H2_XIANXIA_RPG_ACT_MS + 20u);
  CHECK(b->enemy_hp < hp);
  run(&game, 2000);
  /* The wrong shadow feeds the wraith and costs the turn. */
  CHECK(b->split);
  const uint32_t before = b->enemy_hp;
  tap_rect(&game, &game.layout.commands[H2_XIANXIA_RPG_COMMAND_ATTACK]);
  tap_shadow(&game, 1u - (uint32_t)b->real_shadow);
  CHECK(b->enemy_hp == before + b->enemy_hp_max * 5u / 100u);
  CHECK(!b->split);
}

static void test_record_codecs(void) {
  const h2_xianxia_rpg_save_t journey = {
      .chapter = 2u,
      .stage = H2_XIANXIA_RPG_STAGE_C2_WRECK,
      .level = 6u,
      .exp = 260u,
      .pills = 4u,
      .flags = H2_XIANXIA_RPG_FLAG_C2_POST4 | H2_XIANXIA_RPG_FLAG_C1_CLEARED,
      .map = H2_XIANXIA_RPG_MAP_WRECK,
      .x = 2u,
      .y = 5u,
      .battles = 7u,
      .turns = 51u,
      .trust = 1u,
      .rest_map = H2_XIANXIA_RPG_MAP_WRECK,
      .rest_x = 5u,
      .rest_y = 3u};
  uint8_t bytes[H2_XIANXIA_RPG_JOURNEY_BYTES];
  h2_xianxia_rpg_journey_encode(&journey, 0xC0FFEEu, bytes);
  h2_xianxia_rpg_save_t loaded;
  uint32_t tag = 0;
  CHECK(h2_xianxia_rpg_journey_decode(bytes, sizeof(bytes), &loaded, &tag));
  CHECK(memcmp(&loaded, &journey, sizeof(journey)) == 0 && tag == 0xC0FFEEu);
  CHECK(
      !h2_xianxia_rpg_journey_decode(bytes, sizeof(bytes) - 1u, &loaded, &tag));
  /* Any flipped byte fails the checksum; impossible fields fail checks. */
  for (size_t i = 0; i < sizeof(bytes); ++i) {
    uint8_t bad[H2_XIANXIA_RPG_JOURNEY_BYTES];
    memcpy(bad, bytes, sizeof(bad));
    bad[i] ^= 0x10u;
    CHECK(!h2_xianxia_rpg_journey_decode(bad, sizeof(bad), &loaded, &tag));
  }
  h2_xianxia_rpg_save_t odd = journey;
  odd.chapter = 1u;
  h2_xianxia_rpg_journey_encode(&odd, 0u, bytes);
  CHECK(!h2_xianxia_rpg_journey_decode(bytes, sizeof(bytes), &loaded, &tag));
  odd = journey;
  odd.chapter = 3u;
  h2_xianxia_rpg_journey_encode(&odd, 0u, bytes);
  CHECK(!h2_xianxia_rpg_journey_decode(bytes, sizeof(bytes), &loaded, &tag));
  odd = journey;
  odd.stage = H2_XIANXIA_RPG_STAGE_TO_SHRINE;
  h2_xianxia_rpg_journey_encode(&odd, 0u, bytes);
  CHECK(!h2_xianxia_rpg_journey_decode(bytes, sizeof(bytes), &loaded, &tag));
  odd = journey;
  odd.map = H2_XIANXIA_RPG_MAP_SHRINE;
  h2_xianxia_rpg_journey_encode(&odd, 0u, bytes);
  CHECK(!h2_xianxia_rpg_journey_decode(bytes, sizeof(bytes), &loaded, &tag));

  h2_xianxia_rpg_progress_t progress;
  memset(&progress, 0, sizeof(progress));
  progress.cleared = 3u;
  progress.chapters[0] = (h2_xianxia_rpg_clear_t){4u, 99u, 2u, 6u, 39u, 1u, 0u};
  progress.chapters[1] = (h2_xianxia_rpg_clear_t){
      6u, 300u, 5u, 9u, 70u, H2_XIANXIA_RPG_FLAG_C2_HAND_LIGHT, 1u};
  uint8_t record[H2_XIANXIA_RPG_PROGRESS_BYTES];
  h2_xianxia_rpg_progress_encode(&progress, record);
  h2_xianxia_rpg_progress_t back;
  CHECK(h2_xianxia_rpg_progress_decode(record, sizeof(record), &back));
  CHECK(memcmp(&back, &progress, sizeof(back)) == 0);
  record[10] ^= 1u;
  CHECK(!h2_xianxia_rpg_progress_decode(record, sizeof(record), &back));
  CHECK(!h2_xianxia_rpg_progress_decode(record, 12u, &back));

  /* The chapter-1 tag notices any change of save0..3, and absence. */
  const uint32_t words[H2_XIANXIA_RPG_SAVE_WORDS] = {1u, 2u, 3u, 4u};
  uint32_t other[H2_XIANXIA_RPG_SAVE_WORDS] = {1u, 2u, 3u, 5u};
  CHECK(h2_xianxia_rpg_chapter1_tag(words, 0u) == 0u);
  CHECK(h2_xianxia_rpg_chapter1_tag(words, 0xFu) != 0u);
  CHECK(h2_xianxia_rpg_chapter1_tag(words, 0xFu) !=
        h2_xianxia_rpg_chapter1_tag(other, 0xFu));
  other[3] = 4u;
  CHECK(h2_xianxia_rpg_chapter1_tag(words, 0xFu) ==
        h2_xianxia_rpg_chapter1_tag(other, 0xFu));
  CHECK(h2_xianxia_rpg_chapter1_tag(words, 0x7u) !=
        h2_xianxia_rpg_chapter1_tag(words, 0xFu));

  /* Invalid parts handed to init are dropped, not trusted. */
  h2_xianxia_rpg_records_t r;
  memset(&r, 0, sizeof(r));
  r.has_journey = true;
  r.journey = journey;
  r.journey.level = 0u;
  r.journey_latest = true;
  r.unreadable = true;
  h2_xianxia_rpg_game_init(&game, 640, 480, 81u, &r);
  CHECK(h2_xianxia_rpg_continue_save(&game) == NULL);
  CHECK(game.records.unreadable);
}

/* Full chapter 3 through the pointer path; no forced growth or random
 * encounters. Checkpoint reloads preserve the puzzle, knowledge and ending. */
static void enter_chapter3(h2_xianxia_rpg_game_t* g, uint32_t seed) {
  h2_xianxia_rpg_game_init(g, 640, 480, seed, NULL);
  run(g, 300);
  tap_rect(g, &g->layout.title_buttons[2]);
  run(g, 300);
  tap_rect(g, &g->layout.chapter_rows[2]);
  CHECK(g->phase == H2_XIANXIA_RPG_PHASE_PROLOGUE && !g->prologue_carry);
  run(g, 1000);
  tap_center(g);
  finish_dialogue(g);
  CHECK(g->chapter == 3u && g->hero.level == 6u && g->ally_visible);
}

static void test_chapter3_full_run(void) {
  enter_chapter3(&game, 91u);
  talk_to(&game, 3, 7);
  CHECK(game.rest_map == H2_XIANXIA_RPG_MAP_RAIN_PATH);
  talk_to(&game, 3, 6);
  CHECK(game.stage == H2_XIANXIA_RPG_STAGE_C3_ARRAY);
  talk_to(&game, 2, 2);
  walk_to(&game, 7, 0);
  CHECK(game.map == H2_XIANXIA_RPG_MAP_SWORD_COURT);
  CHECK(!h2_xianxia_rpg_walkable(&game, game.map, 7, 3));
  tap_tile(&game, 7, 0);
  CHECK(game.path_len == 0u);
  talk_to(&game, 7, 5);
  talk_to(&game, 2, 4);
  talk_to(&game, 12, 4);
  CHECK(game.flags & H2_XIANXIA_RPG_FLAG_C3_STELE1);
  CHECK(game.flags & H2_XIANXIA_RPG_FLAG_C3_STELE2);
  tap_tile(&game, 4, 5);
  settle(&game);
  CHECK(h2_xianxia_rpg_c3_post_direction(game.flags, 0u) == 1u);
  h2_xianxia_rpg_writes_t writes;
  CHECK(h2_xianxia_rpg_game_take_writes(&game, &writes));
  CHECK(writes.journey3 && !writes.journey && !writes.chapter1 &&
        !writes.progress);
  const uint32_t flags = game.flags;
  enter_phase_title(&game);
  tap_continue(&game);
  CHECK(game.chapter == 3u && game.flags == flags);
  CHECK(h2_xianxia_rpg_unfinished(&game, 3u) != NULL);
  tap_tile(&game, 7, 7);
  settle(&game);
  CHECK(!h2_xianxia_rpg_c3_posts_aligned(game.flags));
  tap_tile(&game, 7, 7);
  settle(&game);
  tap_tile(&game, 10, 5);
  settle(&game);
  CHECK(!h2_xianxia_rpg_c3_posts_aligned(game.flags));
  tap_tile(&game, 10, 5);
  settle(&game);
  CHECK(h2_xianxia_rpg_c3_posts_aligned(game.flags) && game.dialogue.active);
  finish_dialogue(&game);
  CHECK(game.stage == H2_XIANXIA_RPG_STAGE_C3_HALL);
  CHECK(h2_xianxia_rpg_walkable(&game, game.map, 7, 3));
  talk_to(&game, 8, 2);
  CHECK(game.flags & H2_XIANXIA_RPG_FLAG_C3_STELE3);
  walk_to(&game, 7, 0);
  finish_dialogue(&game);
  CHECK(game.map == H2_XIANXIA_RPG_MAP_SWORD_HALL && game.rest_x == 3 &&
        game.rest_y == 7);
  CHECK(game.hero.hp == game.hero.hp_max);
  CHECK(game.battles_won == 0u);
  CHECK(h2_xianxia_rpg_game_take_writes(&game, &writes));
  const h2_xianxia_rpg_records_t before_boss = game.records;
  talk_to(&game, 7, 3);
  /* Deliberate error on wind, then defeat: no progress is automatically
   * saved and the retry begins with the first stance at full enemy HP. */
  wait_command(&game);
  tap_rect(&game, &game.layout.commands[H2_XIANXIA_RPG_COMMAND_GUARD]);
  wait_command(&game);
  CHECK(game.battle.sword_stance == 1u && game.battle.rapport == 3u);
  const uint32_t hp = game.hero.hp;
  tap_rect(&game, &game.layout.commands[H2_XIANXIA_RPG_COMMAND_SKILL]);
  CHECK(!game.battle.charging && game.battle.joint);
  wait_command(&game);
  CHECK(game.hero.hp == hp && game.battle.sword_stance == 2u);
  const uint32_t enemy_hp = game.battle.enemy_hp;
  tap_rect(&game, &game.layout.commands[H2_XIANXIA_RPG_COMMAND_ATTACK]);
  CHECK(game.battle.sword_counter && game.battle.hit_count == 0u);
  wait_command(&game);
  CHECK(game.battle.enemy_hp == enemy_hp && game.hero.hp < hp);
  lose_battle(&game);
  CHECK(game.tx == 3 && game.ty == 7 && game.hero.hp == game.hero.hp_max);
  CHECK(!h2_xianxia_rpg_game_take_writes(&game, &writes));
  finish_dialogue(&game);
  talk_to(&game, 7, 3);
  CHECK(game.battle.sword_stance == 0u && game.battle.enemy_hp == 520u);
  /* Even with no pills left, reading the three stances wins. Several seeds
   * guard against accidental dependence on a critical or a random drop. */
  for (uint32_t seed = 1; seed <= 12u; ++seed) {
    h2_xianxia_rpg_game_init(&game, 640, 480, seed, &before_boss);
    tap_continue(&game);
    game.hero.pills = 0u;
    talk_to(&game, 7, 3);
    for (int i = 0;
         i < 2400 && game.battle.state != H2_XIANXIA_RPG_BATTLE_VICTORY; ++i) {
      CHECK(game.battle.state != H2_XIANXIA_RPG_BATTLE_DEFEAT);
      if (game.battle.state == H2_XIANXIA_RPG_BATTLE_COMMAND) {
        const uint32_t stance = game.battle.sword_stance;
        if (stance == 1u)
          CHECK(h2_xianxia_rpg_joint_ready(&game) && game.hero.mp >= 7u);
        tap_rect(
            &game,
            &game.layout.commands[stance == 1u ? H2_XIANXIA_RPG_COMMAND_SKILL
                                               : H2_XIANXIA_RPG_COMMAND_GUARD]);
      }
      run(&game, 50);
    }
    CHECK(game.battle.state == H2_XIANXIA_RPG_BATTLE_VICTORY);
    CHECK(game.stage == H2_XIANXIA_RPG_STAGE_C3_RETURN &&
          game.battles_won == 1u);
  }
  CHECK(h2_xianxia_rpg_game_take_writes(&game, &writes));
  enter_phase_title(&game);
  tap_continue(&game);
  CHECK(game.stage == H2_XIANXIA_RPG_STAGE_C3_RETURN);
  talk_to(&game, 3, 2);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_ENDING);
  CHECK(game.flags & H2_XIANXIA_RPG_FLAG_C3_WENZHOU);
  CHECK(game.flags & H2_XIANXIA_RPG_FLAG_C3_GLYPH);
  CHECK(h2_xianxia_rpg_game_take_writes(&game, &writes));
  CHECK(writes.journey3 && writes.progress &&
        writes.progress_record.cleared == 4u);
  const uint32_t turns = game.turns_taken;
  enter_phase_title(&game);
  tap_continue(&game);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_ENDING && game.turns_taken == turns);
  CHECK(!h2_xianxia_rpg_game_take_writes(&game, &writes));
}

static void test_chapter3_carry_and_latest(void) {
  h2_xianxia_rpg_records_t r = {0};
  r.has_journey = r.journey_latest = true;
  r.journey = (h2_xianxia_rpg_save_t){.chapter = 2,
                                      .stage = H2_XIANXIA_RPG_STAGE_C2_CLEARED,
                                      .level = 7,
                                      .exp = 340,
                                      .pills = 1,
                                      .map = H2_XIANXIA_RPG_MAP_DOCK,
                                      .x = 6,
                                      .y = 4,
                                      .rest_map = H2_XIANXIA_RPG_MAP_DOCK,
                                      .rest_x = 13,
                                      .rest_y = 3};
  r.progress.cleared = 2u;
  r.progress.chapters[1] =
      (h2_xianxia_rpg_clear_t){.level = 7,
                               .exp = 340,
                               .pills = 1,
                               .trust = 1,
                               .flags = H2_XIANXIA_RPG_FLAG_C2_ROPE};
  h2_xianxia_rpg_game_init(&game, 640, 480, 94u, &r);
  tap_continue(&game);
  run(&game, 1600);
  tap_center(&game);
  CHECK(game.prologue_chapter == 3u && game.prologue_carry);
  run(&game, 1000);
  tap_center(&game);
  finish_dialogue(&game);
  CHECK(game.chapter == 3u && game.hero.level == 7u && game.hero.pills == 1u &&
        game.trust == 1u);
  CHECK(game.flags & H2_XIANXIA_RPG_FLAG_C2_ROPE);
  h2_xianxia_rpg_writes_t w;
  CHECK(h2_xianxia_rpg_game_take_writes(&game, &w) && w.journey3 && !w.journey);
  r = game.records;
  uint8_t bytes[H2_XIANXIA_RPG_JOURNEY_BYTES];
  h2_xianxia_rpg_journey_encode(&r.journey3, 123u, bytes);
  CHECK(h2_xianxia_rpg_journey_decode(bytes, sizeof(bytes), &r.journey3, NULL));
  r.journey3_latest = false; /* The older build has played and saved again. */
  h2_xianxia_rpg_game_init(&game, 640, 480, 95u, &r);
  CHECK(h2_xianxia_rpg_continue_save(&game)->chapter == 2u);
  CHECK(h2_xianxia_rpg_unfinished(&game, 3u) != NULL);
  r.journey3_latest = true;
  h2_xianxia_rpg_game_init(&game, 640, 480, 95u, &r);
  CHECK(h2_xianxia_rpg_continue_save(&game)->chapter == 3u);
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
  test_chapter2_defaults_and_full_run();
  test_upgrade_from_cleared_v01();
  test_continue_follows_latest_save();
  test_chapter2_defeat_fallbacks();
  test_wraith_pattern();
  test_record_codecs();
  test_chapter3_full_run();
  test_chapter3_carry_and_latest();
  printf("xianxia_rpg game tests passed\n");
  return 0;
}
