#include "h2_xianxia_rpg_game.h"
#include "h2_xianxia_rpg_render.h"
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

#define MAX_W 1024
#define MAX_H 1024
#define SENTINEL 0xA5A5u

static uint16_t pixels[MAX_W * MAX_H];
static h2_xianxia_rpg_game_t game;

static h2_xianxia_rpg_canvas_t canvas_of(const h2_xianxia_rpg_game_t* g,
                                         bool measure) {
  h2_xianxia_rpg_canvas_t canvas;
  memset(&canvas, 0, sizeof(canvas));
  canvas.pixels = pixels;
  canvas.width = g->width;
  canvas.height = g->height;
  canvas.stride_pixels = (size_t)g->width;
  canvas.measure = measure;
  return canvas;
}

typedef void (*layer_fn)(h2_xianxia_rpg_canvas_t*,
                         const h2_xianxia_rpg_game_t*);

/* Every pixel a layer paints lies inside the bands and spans its measure
 * pass reported, so restoring only those regions never leaves stale art. */
static size_t check_layer(const h2_xianxia_rpg_game_t* g, layer_fn layer) {
  const size_t count = (size_t)g->width * (size_t)g->height;
  for (size_t i = 0; i < count; ++i) pixels[i] = SENTINEL;
  h2_xianxia_rpg_canvas_t measure = canvas_of(g, true);
  layer(&measure, g);
  for (size_t i = 0; i < count; ++i) CHECK(pixels[i] == SENTINEL);
  h2_xianxia_rpg_canvas_t paint = canvas_of(g, false);
  layer(&paint, g);
  CHECK(paint.bands == measure.bands);
  size_t changed = 0;
  for (int32_t y = 0; y < g->height; ++y)
    for (int32_t x = 0; x < g->width; ++x) {
      if (pixels[(size_t)y * (size_t)g->width + (size_t)x] == SENTINEL)
        continue;
      ++changed;
      const int32_t band = y / H2_XIANXIA_RPG_BAND_ROWS;
      CHECK(measure.bands & ((uint64_t)1 << band));
      CHECK(x >= measure.band_x0[band] && x < measure.band_x1[band]);
    }
  return changed;
}

static void run(h2_xianxia_rpg_game_t* g, uint32_t ms) {
  while (ms > 0u) {
    const uint32_t step = ms > 20u ? 20u : ms;
    h2_xianxia_rpg_game_step(g, step);
    ms -= step;
  }
}

static void tap(h2_xianxia_rpg_game_t* g, float x, float y) {
  h2_xianxia_rpg_game_pointer(g, H2_XIANXIA_RPG_POINTER_DOWN, x, y);
  h2_xianxia_rpg_game_pointer(g, H2_XIANXIA_RPG_POINTER_UP, x, y);
}

static void tap_rect(h2_xianxia_rpg_game_t* g, const h2_xianxia_rpg_rect_t* r) {
  tap(g, (float)r->x + (float)r->w * 0.5f, (float)r->y + (float)r->h * 0.5f);
}

static void check_all_layers(const h2_xianxia_rpg_game_t* g) {
  /* The background covers the full screen. */
  const size_t total = (size_t)g->width * (size_t)g->height;
  CHECK(check_layer(g, h2_xianxia_rpg_render_background) == total);
  CHECK(check_layer(g, h2_xianxia_rpg_render_static) > 0u);
  (void)check_layer(g, h2_xianxia_rpg_render_scene);
}

static void test_story_string_capacity(void) {
  const size_t total = h2_xianxia_rpg_story_strings(NULL, 0u);
  CHECK(total > 0u);
  const char** full = malloc(total * sizeof(*full));
  const char** bounded = malloc((total + 3u) * sizeof(*bounded));
  CHECK(full != NULL && bounded != NULL);
  CHECK(h2_xianxia_rpg_story_strings(full, total) == total);
  static const char sentinel[] = "untouched";
  /* Every prefix includes exact fit, one short, and capacities exhausted
   * before any later chapter. Keep both guards and the entire unused tail. */
  for (size_t capacity = 0; capacity <= total + 1u; ++capacity) {
    for (size_t i = 0; i < total + 3u; ++i) bounded[i] = sentinel;
    CHECK(h2_xianxia_rpg_story_strings(bounded + 1, capacity) == total);
    CHECK(bounded[0] == sentinel);
    const size_t written = capacity < total ? capacity : total;
    for (size_t i = 0; i < written; ++i) CHECK(bounded[i + 1u] == full[i]);
    for (size_t i = written; i < total + 2u; ++i) {
      if (bounded[i + 1u] != sentinel)
        fprintf(stderr, "story strings: capacity=%zu wrote index=%zu\n",
                capacity, i);
      CHECK(bounded[i + 1u] == sentinel);
    }
  }
  printf(
      "story strings: total=%zu, capacities 0..%zu preserve bounds and prefix\n",
      total, total + 1u);
  free(bounded);
  free(full);
}

static void test_glyph_coverage(void) {
  const size_t count = h2_xianxia_rpg_story_strings(NULL, 0);
  CHECK(count > 200u);
  const char** strings = malloc(count * sizeof(*strings));
  CHECK(strings != NULL);
  CHECK(h2_xianxia_rpg_story_strings(strings, count) == count);
  size_t missing = 0;
  for (size_t i = 0; i < count; ++i) {
    const char* s = strings[i];
    for (uint32_t cp = h2_xianxia_rpg_utf8_next(&s); cp != 0u;
         cp = h2_xianxia_rpg_utf8_next(&s)) {
      if (!h2_xianxia_rpg_has_glyph(false, cp)) {
        fprintf(stderr, "missing body glyph U+%04X in \"%s\"\n", (unsigned)cp,
                strings[i]);
        ++missing;
      }
    }
  }
  CHECK(missing == 0u);
  free(strings);
  static const char* const titles[] = {
      "竹溪仙缘", "竹溪灯影",   "渡口迷雾", "雨夜剑阁", "万灯归山",
      "照水旧城", "百草寒谷",   "镜湖问心", "雪岭断桥", "地脉回声",
      "终",       "0123456789", "+",
  };
  for (size_t i = 0; i < sizeof(titles) / sizeof(titles[0]); ++i) {
    const char* s = titles[i];
    for (uint32_t cp = h2_xianxia_rpg_utf8_next(&s); cp != 0u;
         cp = h2_xianxia_rpg_utf8_next(&s))
      CHECK(h2_xianxia_rpg_has_glyph(true, cp));
  }
  CHECK(strcmp(h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_TITLE], "竹溪仙缘") ==
        0);
  CHECK(strcmp(h2_xianxia_rpg_text[H2_XIANXIA_RPG_TEXT_ENDING_TITLE],
               "竹溪灯影") == 0);
  CHECK(!h2_xianxia_rpg_has_glyph(false, 0x9F98u));
  CHECK(h2_xianxia_rpg_text_width(false, "AB") > 0);
  CHECK(h2_xianxia_rpg_text_width(true, "终") >
        h2_xianxia_rpg_text_width(false, "终"));
}

static void test_screens(int32_t width, int32_t height) {
  /* Title, with and without a saved journey. */
  h2_xianxia_rpg_game_init(&game, width, height, 3u, NULL);
  check_all_layers(&game);
  const h2_xianxia_rpg_save_t save = {.stage = 1u,
                                      .level = 2u,
                                      .exp = 20u,
                                      .pills = 2u,
                                      .flags = 1u,
                                      .map = 0u,
                                      .x = 7u,
                                      .y = 5u};
  h2_xianxia_rpg_records_t records;
  memset(&records, 0, sizeof(records));
  uint32_t words[H2_XIANXIA_RPG_SAVE_WORDS];
  h2_xianxia_rpg_save_pack(&save, words);
  records.has_chapter1 = h2_xianxia_rpg_save_unpack(words, &records.chapter1);
  CHECK(records.has_chapter1);
  records.unreadable = true;
  h2_xianxia_rpg_game_init(&game, width, height, 3u, &records);
  check_all_layers(&game);
  records.unreadable = false;
  h2_xianxia_rpg_game_init(&game, width, height, 3u, &records);
  check_all_layers(&game);

  /* Each map, with and without dialogue, while walking. */
  run(&game, 300);
  tap_rect(&game, &game.layout.title_buttons[0]);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_EXPLORE && game.dialogue.active);
  run(&game, 400);
  check_all_layers(&game);
  game.dialogue.active = false;
  for (int m = 0; m < H2_XIANXIA_RPG_MAP_COUNT; ++m) {
    game.map = (h2_xianxia_rpg_map_t)m;
    game.stage = m == H2_XIANXIA_RPG_MAP_SHRINE
                     ? H2_XIANXIA_RPG_STAGE_TO_SHRINE
                     : H2_XIANXIA_RPG_STAGE_MEET_ELDER;
    check_all_layers(&game);
  }
  /* After the boss: clear creek, lit lanterns and an objective banner. */
  game.map = H2_XIANXIA_RPG_MAP_VILLAGE;
  game.stage = H2_XIANXIA_RPG_STAGE_RETURN;
  game.banner_ms = 0u;
  game.objective_ms = 1000u;
  check_all_layers(&game);
  game.objective_ms = 0u;
  game.map = H2_XIANXIA_RPG_MAP_VILLAGE;
  game.stage = H2_XIANXIA_RPG_STAGE_MEET_ELDER;
  game.tx = 7;
  game.ty = 4;
  game.from_x = 7;
  game.from_y = 5;
  game.moving = true;
  game.move_ms = 80u;
  game.marker_ms = 300u;
  game.marker_x = 7;
  game.marker_y = 3;
  check_all_layers(&game);
  game.moving = false;

  /* Battle states across the forest and the shrine. */
  game.map = H2_XIANXIA_RPG_MAP_FOREST;
  game.encounter_steps = 1u;
  game.tx = game.from_x = 4;
  game.ty = game.from_y = 4;
  tap(&game, (float)game.layout.map_x + 5.5f * (float)game.layout.tile,
      (float)game.layout.map_y + 4.5f * (float)game.layout.tile);
  for (int i = 0; i < 50 && game.phase != H2_XIANXIA_RPG_PHASE_BATTLE; ++i)
    run(&game, 20);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_BATTLE);
  check_all_layers(&game);
  run(&game, 1000);
  CHECK(game.battle.state == H2_XIANXIA_RPG_BATTLE_COMMAND);
  tap_rect(&game, &game.layout.commands[H2_XIANXIA_RPG_COMMAND_ATTACK]);
  run(&game, 280);
  check_all_layers(&game);
  for (int e = 0; e < H2_XIANXIA_RPG_ENEMY_COUNT; ++e) {
    game.battle.enemy = (uint8_t)e;
    game.battle.boss = e == H2_XIANXIA_RPG_ENEMY_DRAGON;
    game.battle.charging = game.battle.boss;
    game.map = e >= H2_XIANXIA_RPG_ENEMY_WISP ? H2_XIANXIA_RPG_MAP_SHRINE
                                              : H2_XIANXIA_RPG_MAP_FOREST;
    check_all_layers(&game);
  }
  game.battle.state = H2_XIANXIA_RPG_BATTLE_VICTORY;
  game.battle.state_ms = 500u;
  check_all_layers(&game);
  game.battle.state = H2_XIANXIA_RPG_BATTLE_DEFEAT;
  check_all_layers(&game);

  /* Ending, before and after its tap hint appears. */
  game.phase = H2_XIANXIA_RPG_PHASE_ENDING;
  game.phase_started_ms = game.time_ms;
  check_all_layers(&game);
  run(&game, 1600);
  check_all_layers(&game);
}

/* Chapter screen, chapter card, the chapter-2 maps in each state, its
 * battles and its ending. */
static void test_chapter2_screens(int32_t width, int32_t height) {
  h2_xianxia_rpg_records_t records;
  memset(&records, 0, sizeof(records));
  records.progress.cleared = 1u;
  records.progress.chapters[0] =
      (h2_xianxia_rpg_clear_t){4u, 99u, 2u, 6u, 39u, 1u, 0u};
  records.has_journey = true;
  records.journey_latest = true;
  records.journey =
      (h2_xianxia_rpg_save_t){.chapter = 2u,
                              .stage = H2_XIANXIA_RPG_STAGE_C2_TO_REEDS,
                              .level = 4u,
                              .exp = 99u,
                              .pills = 2u,
                              .map = H2_XIANXIA_RPG_MAP_REEDS,
                              .x = 13u,
                              .y = 4u,
                              .rest_map = H2_XIANXIA_RPG_MAP_DOCK,
                              .rest_x = 13u,
                              .rest_y = 3u};
  h2_xianxia_rpg_game_init(&game, width, height, 5u, &records);
  check_all_layers(&game);
  run(&game, 300);
  tap_rect(&game, &game.layout.title_buttons[2]);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_CHAPTERS);
  check_all_layers(&game);
  run(&game, 300);
  tap_rect(&game, &game.layout.chapter_restart[1]);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_PROLOGUE && game.prologue_carry);
  check_all_layers(&game);
  run(&game, 1000);
  check_all_layers(&game);
  game.prologue_carry = false;
  check_all_layers(&game);
  tap(&game, (float)width * 0.5f, (float)height * 0.5f);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_EXPLORE && game.chapter == 2u);
  check_all_layers(&game);
  game.dialogue.active = false;
  /* Fog, lit lamps and the cleared dock. */
  static const uint32_t states[][2] = {
      {H2_XIANXIA_RPG_STAGE_C2_ARRIVE, 0u},
      {H2_XIANXIA_RPG_STAGE_C2_TO_REEDS, H2_XIANXIA_RPG_FLAG_C2_POST1},
      {H2_XIANXIA_RPG_STAGE_C2_TO_WRECK,
       H2_XIANXIA_RPG_FLAG_C2_POST1 | H2_XIANXIA_RPG_FLAG_C2_POST2 |
           H2_XIANXIA_RPG_FLAG_C2_BOAT | H2_XIANXIA_RPG_FLAG_C2_BASKET},
      {H2_XIANXIA_RPG_STAGE_C2_WRECK, H2_XIANXIA_RPG_FLAG_C2_POST4},
      {H2_XIANXIA_RPG_STAGE_C2_RETURN, 0x1FFF00u},
  };
  for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); ++i)
    for (int m = H2_XIANXIA_RPG_MAP_DOCK; m < H2_XIANXIA_RPG_MAP_COUNT; ++m) {
      game.map = (h2_xianxia_rpg_map_t)m;
      game.stage = (h2_xianxia_rpg_stage_t)states[i][0];
      game.flags = states[i][1];
      game.tx = game.from_x = 2;
      game.ty = game.from_y = 5;
      game.ally_visible = game.stage >= H2_XIANXIA_RPG_STAGE_C2_TO_REEDS;
      game.ally_x = game.ally_from_x = 3;
      game.ally_y = game.ally_from_y = 5;
      check_all_layers(&game);
    }
  /* Walking with the companion one tile behind. */
  game.map = H2_XIANXIA_RPG_MAP_DOCK;
  game.stage = H2_XIANXIA_RPG_STAGE_C2_TO_REEDS;
  game.tx = 6;
  game.from_x = 7;
  game.ty = game.from_y = 3;
  game.ally_x = 7;
  game.ally_from_x = 8;
  game.ally_y = game.ally_from_y = 3;
  game.moving = true;
  game.move_ms = 80u;
  check_all_layers(&game);
  game.moving = false;

  /* Companion battles: veil, intent, rapport, the joint strike, shadows. */
  game.map = H2_XIANXIA_RPG_MAP_REEDS;
  game.tx = game.from_x = 13;
  game.ty = game.from_y = 5;
  game.encounter_steps = 1u;
  game.flags = H2_XIANXIA_RPG_FLAG_C2_POST1;
  run(&game, 300);
  game.encounter_steps = 1u;
  tap(&game, (float)game.layout.map_x + 11.5f * (float)game.layout.tile,
      (float)game.layout.map_y + 5.5f * (float)game.layout.tile);
  for (int i = 0; i < 80 && game.phase != H2_XIANXIA_RPG_PHASE_BATTLE; ++i)
    run(&game, 20);
  CHECK(game.phase == H2_XIANXIA_RPG_PHASE_BATTLE && game.battle.ally);
  check_all_layers(&game);
  run(&game, 1000);
  for (int e = H2_XIANXIA_RPG_ENEMY_FOG_GHOST; e < H2_XIANXIA_RPG_ENEMY_COUNT;
       ++e) {
    game.battle.enemy = (uint8_t)e;
    game.battle.boss = e == H2_XIANXIA_RPG_ENEMY_FOG_WRAITH;
    game.battle.veiled = e == H2_XIANXIA_RPG_ENEMY_FOG_GHOST;
    game.battle.charging = game.battle.boss;
    game.battle.rapport = (uint32_t)(e - H2_XIANXIA_RPG_ENEMY_FOG_GHOST) % 4u;
    game.map = e >= H2_XIANXIA_RPG_ENEMY_WRECK_GUARD ? H2_XIANXIA_RPG_MAP_WRECK
                                                     : H2_XIANXIA_RPG_MAP_REEDS;
    check_all_layers(&game);
  }
  game.battle.mp_sealed = true;
  check_all_layers(&game);
  game.battle.mp_sealed = false;
  game.battle.split = game.battle.phase_two = true;
  game.battle.charging = false;
  for (uint32_t real = 0; real < 2u; ++real) {
    game.battle.real_shadow = (uint8_t)real;
    game.battle.state = H2_XIANXIA_RPG_BATTLE_TARGET;
    game.battle.pending = H2_XIANXIA_RPG_COMMAND_ATTACK;
    check_all_layers(&game);
  }
  /* Each companion action as it plays out. */
  static const uint8_t actions[] = {H2_XIANXIA_RPG_ALLY_LANTERN,
                                    H2_XIANXIA_RPG_ALLY_POLE,
                                    H2_XIANXIA_RPG_ALLY_STEADY};
  for (size_t i = 0; i < sizeof(actions); ++i) {
    game.battle.state = H2_XIANXIA_RPG_BATTLE_ALLY_ACT;
    game.battle.ally_action = actions[i];
    game.battle.hit_count = actions[i] == H2_XIANXIA_RPG_ALLY_POLE ? 1u : 0u;
    game.battle.state_ms = 300u;
    check_all_layers(&game);
  }
  game.battle.state = H2_XIANXIA_RPG_BATTLE_HERO_ACT;
  game.battle.command = H2_XIANXIA_RPG_COMMAND_SKILL;
  game.battle.joint = true;
  game.battle.hit_count = 2u;
  game.battle.state_ms = 280u;
  check_all_layers(&game);

  /* The chapter-2 ending. */
  game.phase = H2_XIANXIA_RPG_PHASE_ENDING;
  game.phase_started_ms = game.time_ms;
  check_all_layers(&game);
  run(&game, 1600);
  check_all_layers(&game);
}

static void test_keys(void) {
  h2_xianxia_rpg_game_init(&game, 640, 480, 3u, NULL);
  h2_xianxia_rpg_layer_key_t a, b;
  h2_xianxia_rpg_static_key(&game, &a);
  run(&game, 500);
  h2_xianxia_rpg_static_key(&game, &b);
  CHECK(memcmp(&a, &b, sizeof(a)) == 0);
  h2_xianxia_rpg_background_key(&game, &a);
  tap_rect(&game, &game.layout.title_buttons[0]);
  h2_xianxia_rpg_background_key(&game, &b);
  CHECK(memcmp(&a, &b, sizeof(a)) != 0);
  /* Revealing dialogue text changes the static layer. */
  h2_xianxia_rpg_static_key(&game, &a);
  run(&game, 200);
  h2_xianxia_rpg_static_key(&game, &b);
  CHECK(memcmp(&a, &b, sizeof(a)) != 0);
  CHECK(h2_xianxia_rpg_all_bands(16) == 1u);
  CHECK(h2_xianxia_rpg_all_bands(17) == 3u);
  CHECK(h2_xianxia_rpg_all_bands(1024) == UINT64_MAX);
  CHECK(h2_xianxia_rpg_all_bands(0) == 0u);
}

static void test_chapter3_screens(int32_t width, int32_t height) {
  h2_xianxia_rpg_game_init(&game, width, height, 33u, NULL);
  game.chapter = game.prologue_chapter = 3u;
  game.phase = H2_XIANXIA_RPG_PHASE_PROLOGUE;
  check_all_layers(&game);
  game.phase = H2_XIANXIA_RPG_PHASE_EXPLORE;
  game.stage = H2_XIANXIA_RPG_STAGE_C3_ARRAY;
  for (int m = H2_XIANXIA_RPG_MAP_RAIN_PATH; m <= H2_XIANXIA_RPG_MAP_SWORD_HALL;
       ++m) {
    game.map = (h2_xianxia_rpg_map_t)m;
    for (uint32_t state = 0; state < 4; ++state) {
      game.flags = state | state << 2 | state << 4;
      if (state == 3u) game.flags |= H2_XIANXIA_RPG_FLAG_C3_OPEN;
      check_all_layers(&game);
    }
  }
  game.phase = H2_XIANXIA_RPG_PHASE_BATTLE;
  game.battle.enemy = H2_XIANXIA_RPG_ENEMY_SWORD_SOUL;
  game.battle.boss = game.battle.ally = true;
  game.battle.state = H2_XIANXIA_RPG_BATTLE_COMMAND;
  game.battle.enemy_hp = game.battle.enemy_hp_max = 520u;
  for (uint8_t stance = 0; stance < 3; ++stance) {
    game.battle.sword_stance = stance;
    game.flags = 0u;
    check_all_layers(&game);
    game.flags = H2_XIANXIA_RPG_FLAG_C3_STELE1 << stance;
    check_all_layers(&game);
  }
  game.phase = H2_XIANXIA_RPG_PHASE_ENDING;
  check_all_layers(&game);
}

static void test_chapter4_screens(int32_t width, int32_t height) {
  h2_xianxia_rpg_game_init(&game, width, height, 44u, NULL);
  game.chapter = game.prologue_chapter = 4u;
  game.phase = H2_XIANXIA_RPG_PHASE_PROLOGUE;
  check_all_layers(&game);
  game.phase = H2_XIANXIA_RPG_PHASE_EXPLORE;
  game.stage = H2_XIANXIA_RPG_STAGE_C4_ESCORT;
  game.ally_hp = game.ally_hp_max = 102u;
  for (int m = H2_XIANXIA_RPG_MAP_NIGHT_VILLAGE;
       m <= H2_XIANXIA_RPG_MAP_LANTERN_ALTAR; ++m) {
    game.map = (h2_xianxia_rpg_map_t)m;
    game.flags = H2_XIANXIA_RPG_FLAG_C4_FIRE;
    check_all_layers(&game);
    game.flags |= H2_XIANXIA_RPG_C4_LAMPS | H2_XIANXIA_RPG_FLAG_C4_COVER;
    check_all_layers(&game);
  }
  game.phase = H2_XIANXIA_RPG_PHASE_BATTLE;
  game.battle.enemy = H2_XIANXIA_RPG_ENEMY_INK_TIDE;
  game.battle.boss = game.battle.ally = game.battle.lamp_battle = true;
  game.battle.enemy_hp = game.battle.enemy_hp_max = 150u;
  game.battle.lamp_hp = game.battle.lamp_max = 80u;
  game.battle.wave = 1u;
  game.battle.state = H2_XIANXIA_RPG_BATTLE_COMMAND;
  for (uint32_t beat = 0; beat < 3u; ++beat) {
    game.battle.wave_turn = beat;
    check_all_layers(&game);
  }
  h2_xianxia_rpg_layer_key_t before, after;
  h2_xianxia_rpg_static_key(&game, &before);
  game.battle.ally_mend = true;
  h2_xianxia_rpg_static_key(&game, &after);
  CHECK(memcmp(&before, &after, sizeof(before)) != 0);
  before = after;
  game.ally_hp = 0u;
  h2_xianxia_rpg_static_key(&game, &after);
  CHECK(memcmp(&before, &after, sizeof(before)) != 0);
  before = after;
  game.battle.lamp_hp = 12u;
  h2_xianxia_rpg_static_key(&game, &after);
  CHECK(memcmp(&before, &after, sizeof(before)) != 0);
  game.battle.state = H2_XIANXIA_RPG_BATTLE_HEAL_TARGET;
  check_all_layers(&game);
  game.battle.state = H2_XIANXIA_RPG_BATTLE_DEFEAT;
  check_all_layers(&game);
  game.phase = H2_XIANXIA_RPG_PHASE_ENDING;
  check_all_layers(&game);
}

static void test_chapter5_screens(int32_t width, int32_t height) {
  h2_xianxia_rpg_game_init(&game, width, height, 5, NULL);
  game.chapter = game.prologue_chapter = 5;
  game.phase = H2_XIANXIA_RPG_PHASE_PROLOGUE;
  check_all_layers(&game);
  game.phase = H2_XIANXIA_RPG_PHASE_EXPLORE;
  game.stage = H2_XIANXIA_RPG_STAGE_C5_PAGE;
  for (int m = H2_XIANXIA_RPG_MAP_WATER_MARKET;
       m <= H2_XIANXIA_RPG_MAP_WEN_TOWER; ++m) {
    game.map = (h2_xianxia_rpg_map_t)m;
    game.flags = 0;
    check_all_layers(&game);
    h2_xianxia_rpg_layer_key_t before, after;
    h2_xianxia_rpg_background_key(&game, &before);
    game.flags = H2_XIANXIA_RPG_FLAG_C5_HIGH;
    check_all_layers(&game);
    h2_xianxia_rpg_background_key(&game, &after);
    CHECK(memcmp(&before, &after, sizeof(before)) != 0);
  }
  game.map = H2_XIANXIA_RPG_MAP_OLD_CITY;
  game.stage = H2_XIANXIA_RPG_STAGE_C5_ESCORT;
  game.flags = 14u << H2_XIANXIA_RPG_C5_STEPS_SHIFT;
  check_all_layers(&game);
  game.stage = H2_XIANXIA_RPG_STAGE_C5_CHOICE;
  check_all_layers(&game);
  game.phase = H2_XIANXIA_RPG_PHASE_BATTLE;
  game.battle.enemy = H2_XIANXIA_RPG_ENEMY_WENZHOU;
  game.battle.enemy_hp = game.battle.enemy_hp_max = 360;
  game.battle.boss = game.battle.ally = true;
  game.battle.state = H2_XIANXIA_RPG_BATTLE_COMMAND;
  for (uint8_t stance = 0; stance < 3; ++stance) {
    game.battle.sword_stance = stance;
    game.flags = 0;
    check_all_layers(&game);
    game.flags = H2_XIANXIA_RPG_FLAG_C3_STELE1 << stance;
    check_all_layers(&game);
  }
  h2_xianxia_rpg_layer_key_t before, after;
  h2_xianxia_rpg_static_key(&game, &before);
  ++game.battle.turn;
  h2_xianxia_rpg_static_key(&game, &after);
  CHECK(memcmp(&before, &after, sizeof(before)) != 0);
  game.battle.state = H2_XIANXIA_RPG_BATTLE_DEFEAT;
  check_all_layers(&game);
  game.phase = H2_XIANXIA_RPG_PHASE_ENDING;
  check_all_layers(&game);
}

static void test_chapter6_screens(int32_t width, int32_t height) {
  h2_xianxia_rpg_game_init(&game, width, height, 6, NULL);
  game.chapter = game.prologue_chapter = 6;
  game.phase = H2_XIANXIA_RPG_PHASE_PROLOGUE;
  check_all_layers(&game);
  game.phase = H2_XIANXIA_RPG_PHASE_EXPLORE;
  game.stage = H2_XIANXIA_RPG_STAGE_C6_HERBS;
  game.flags = 4;
  for (int m = H2_XIANXIA_RPG_MAP_HERB_HUT;
       m <= H2_XIANXIA_RPG_MAP_FROZEN_CREEK; ++m) {
    game.map = (h2_xianxia_rpg_map_t)m;
    check_all_layers(&game);
  }
  game.stage = H2_XIANXIA_RPG_STAGE_C6_RITUAL;
  for (uint32_t t = 0; t < 3; ++t) {
    game.turns_taken = t;
    game.flags = 4u | (t << 4);
    game.facts = t == 2u ? 3u : 1u;
    check_all_layers(&game);
  }
  h2_xianxia_rpg_layer_key_t before, after;
  h2_xianxia_rpg_static_key(&game, &before);
  ++game.turns_taken;
  h2_xianxia_rpg_static_key(&game, &after);
  CHECK(memcmp(&before, &after, sizeof(before)) != 0);
  game.phase = H2_XIANXIA_RPG_PHASE_ENDING;
  check_all_layers(&game);
}

static void test_chapter7_screens(int32_t width, int32_t height) {
  h2_xianxia_rpg_game_init(&game, width, height, 7, NULL);
  game.chapter = game.prologue_chapter = 7;
  game.phase = H2_XIANXIA_RPG_PHASE_PROLOGUE;
  check_all_layers(&game);
  game.phase = H2_XIANXIA_RPG_PHASE_EXPLORE;
  for (int map = H2_XIANXIA_RPG_MAP_MIRROR_SHORE;
       map <= H2_XIANXIA_RPG_MAP_MIRROR_HALL; ++map) {
    game.map = (h2_xianxia_rpg_map_t)map;
    for (uint32_t era = 0; era < 2; ++era) {
      game.flags = era;
      game.facts = H2_XIANXIA_RPG_FACT_C6_HEALED;
      game.stage = H2_XIANXIA_RPG_STAGE_C7_LAMP;
      check_all_layers(&game);
      h2_xianxia_rpg_layer_key_t a, b;
      h2_xianxia_rpg_background_key(&game, &a);
      game.facts |= H2_XIANXIA_RPG_FACT_C7_LAMP | H2_XIANXIA_RPG_FACT_C7_WATER;
      h2_xianxia_rpg_background_key(&game, &b);
      CHECK(memcmp(&a, &b, sizeof(a)) != 0);
      game.stage = H2_XIANXIA_RPG_STAGE_C7_QUESTION3;
      check_all_layers(&game);
    }
  }
  game.phase = H2_XIANXIA_RPG_PHASE_BATTLE;
  game.battle.enemy = H2_XIANXIA_RPG_ENEMY_MIRROR;
  game.battle.enemy_hp = game.battle.enemy_hp_max = 520;
  game.battle.ally = game.battle.boss = true;
  game.battle.state = H2_XIANXIA_RPG_BATTLE_COMMAND;
  for (uint8_t cmd = 0; cmd < 4; ++cmd) {
    game.battle.mirror_command = cmd;
    game.battle.mirror_joint = false;
    check_all_layers(&game);
  }
  game.battle.mirror_command = H2_XIANXIA_RPG_COMMAND_SKILL;
  game.battle.mirror_joint = true;
  check_all_layers(&game);
  game.phase = H2_XIANXIA_RPG_PHASE_ENDING;
  check_all_layers(&game);
}

static void test_chapter8_screens(int32_t width, int32_t height) {
  h2_xianxia_rpg_game_init(&game, width, height, 8, NULL);
  game.chapter = game.prologue_chapter = 8;
  if (height < 400) {
    game.phase = H2_XIANXIA_RPG_PHASE_CHAPTERS;
    check_all_layers(&game);
    h2_xianxia_rpg_layer_key_t a, b;
    h2_xianxia_rpg_static_key(&game, &a);
    game.chapter_page = 1;
    h2_xianxia_rpg_static_key(&game, &b);
    CHECK(memcmp(&a, &b, sizeof(a)) != 0);
    check_all_layers(&game);
  }
  game.phase = H2_XIANXIA_RPG_PHASE_PROLOGUE;
  check_all_layers(&game);
  game.phase = H2_XIANXIA_RPG_PHASE_EXPLORE;
  for (int map = H2_XIANXIA_RPG_MAP_SNOW_CAMP;
       map <= H2_XIANXIA_RPG_MAP_SNOW_CAVE; ++map) {
    game.map = (h2_xianxia_rpg_map_t)map;
    game.stage = H2_XIANXIA_RPG_STAGE_C8_RESCUE;
    game.facts = H2_XIANXIA_RPG_FACT_C8_CHOSEN;
    check_all_layers(&game);
    h2_xianxia_rpg_layer_key_t a, b;
    h2_xianxia_rpg_background_key(&game, &a);
    game.facts |= H2_XIANXIA_RPG_FACT_C8_LIGHT |
                  H2_XIANXIA_RPG_FACT_C8_EAST_BLOCK |
                  H2_XIANXIA_RPG_FACT_C8_WEST_BLOCK;
    h2_xianxia_rpg_background_key(&game, &b);
    CHECK(memcmp(&a, &b, sizeof(a)) != 0);
    game.stage = H2_XIANXIA_RPG_STAGE_C8_INJURY;
    check_all_layers(&game);
  }
  game.phase = H2_XIANXIA_RPG_PHASE_BATTLE;
  game.battle.enemy = H2_XIANXIA_RPG_ENEMY_BRIDGE_SHADE;
  game.battle.enemy_hp = game.battle.enemy_hp_max = 900;
  game.battle.ally = game.battle.boss = game.battle.lamp_battle =
      game.battle.dual_lamps = true;
  game.battle.lamp_hp = game.battle.west_lamp_hp = game.battle.lamp_max = 60;
  game.battle.state = H2_XIANXIA_RPG_BATTLE_COMMAND;
  for (unsigned turn = 0; turn < 3; ++turn) {
    game.battle.turn = game.battle.wave_turn = turn;
    check_all_layers(&game);
  }
  h2_xianxia_rpg_layer_key_t a, b;
  h2_xianxia_rpg_static_key(&game, &a);
  --game.battle.west_lamp_hp;
  h2_xianxia_rpg_static_key(&game, &b);
  CHECK(memcmp(&a, &b, sizeof(a)) != 0);
  game.facts |=
      H2_XIANXIA_RPG_FACT_C8_ACTIVE_WEN | H2_XIANXIA_RPG_FACT_C8_WEST_FIRST;
  check_all_layers(&game);
  game.phase = H2_XIANXIA_RPG_PHASE_ENDING;
  check_all_layers(&game);
}

static void test_chapter9_screens(int32_t width, int32_t height) {
  h2_xianxia_rpg_game_init(&game, width, height, 9, NULL);
  game.chapter = game.prologue_chapter = 9;
  game.phase = H2_XIANXIA_RPG_PHASE_PROLOGUE;
  check_all_layers(&game);
  game.phase = H2_XIANXIA_RPG_PHASE_EXPLORE;
  game.stage = H2_XIANXIA_RPG_STAGE_C9_ROOMS;
  for (int map = H2_XIANXIA_RPG_MAP_SHAFT_UPPER;
       map <= H2_XIANXIA_RPG_MAP_LANTERN_CORE; ++map) {
    game.map = (h2_xianxia_rpg_map_t)map;
    check_all_layers(&game);
  }
  game.glyph_view = 1;
  check_all_layers(&game);
  game.glyph_view = 2;
  for (unsigned i = 0; i < 4; ++i) {
    game.node_room = (uint8_t)i;
    check_all_layers(&game);
  }
  h2_xianxia_rpg_layer_key_t a, b;
  h2_xianxia_rpg_static_key(&game, &a);
  game.glyph_wrong = true;
  h2_xianxia_rpg_static_key(&game, &b);
  CHECK(memcmp(&a, &b, sizeof(a)) != 0);
  check_all_layers(&game);
  game.glyph_view = 0;
  game.phase = H2_XIANXIA_RPG_PHASE_BATTLE;
  game.battle.enemy = H2_XIANXIA_RPG_ENEMY_CORE_SHADE;
  game.battle.enemy_hp = game.battle.enemy_hp_max = 1450;
  game.battle.node_battle = game.battle.ally = game.battle.boss = true;
  game.battle.state = H2_XIANXIA_RPG_BATTLE_COMMAND;
  for (unsigned i = 0; i < 8; ++i) {
    game.battle.turn = i;
    game.battle.node_phase = (uint8_t)(i / 2u);
    if (i & 1u)
      game.facts |= H2_XIANXIA_RPG_FACT_C9_ACTIVE_WEN;
    else
      game.facts &= ~H2_XIANXIA_RPG_FACT_C9_ACTIVE_WEN;
    game.battle.mirror_command = (uint8_t)(i % 5u);
    check_all_layers(&game);
  }
  h2_xianxia_rpg_static_key(&game, &a);
  ++game.battle.turn;
  h2_xianxia_rpg_static_key(&game, &b);
  CHECK(memcmp(&a, &b, sizeof(a)) != 0);
  game.phase = H2_XIANXIA_RPG_PHASE_ENDING;
  check_all_layers(&game);
}

static void test_chapter10_screens(int32_t width, int32_t height) {
  h2_xianxia_rpg_game_init(&game, width, height, 10, NULL);
  game.chapter = game.prologue_chapter = 10;
  game.phase = H2_XIANXIA_RPG_PHASE_PROLOGUE;
  check_all_layers(&game);
  game.phase = H2_XIANXIA_RPG_PHASE_EXPLORE;
  game.stage = H2_XIANXIA_RPG_STAGE_C10_ARRANGE;
  game.flags = 0x79u;
  for (int map = H2_XIANXIA_RPG_MAP_HOME_VILLAGE;
       map <= H2_XIANXIA_RPG_MAP_HOME_TOWER; ++map) {
    game.map = (h2_xianxia_rpg_map_t)map;
    check_all_layers(&game);
  }
  for (uint8_t view = 1; view <= 2; ++view) {
    game.glyph_view = view;
    check_all_layers(&game);
  }
  game.glyph_view = 0;
  game.phase = H2_XIANXIA_RPG_PHASE_BATTLE;
  game.battle.finale = game.battle.ally = game.battle.boss = true;
  game.battle.enemy_hp = game.battle.enemy_hp_max = 600;
  game.battle.state = H2_XIANXIA_RPG_BATTLE_COMMAND;
  for (unsigned phase = 0; phase < 3; ++phase) {
    game.stage =
        (h2_xianxia_rpg_stage_t)(H2_XIANXIA_RPG_STAGE_C10_SHRINE + phase);
    game.battle.enemy = (uint8_t)(H2_XIANXIA_RPG_ENEMY_HOME_DRAGON + phase);
    for (uint8_t beat = 0; beat < 3; ++beat) {
      game.battle.formation_step = beat;
      game.facts ^= H2_XIANXIA_RPG_FACT_C10_ACTIVE_WEN;
      check_all_layers(&game);
    }
  }
  h2_xianxia_rpg_layer_key_t a, b;
  h2_xianxia_rpg_static_key(&game, &a);
  ++game.battle.formation_round;
  h2_xianxia_rpg_static_key(&game, &b);
  CHECK(memcmp(&a, &b, sizeof(a)) != 0);
  game.phase = H2_XIANXIA_RPG_PHASE_ENDING;
  check_all_layers(&game);
  h2_xianxia_rpg_static_key(&game, &a);
  game.facts |= H2_XIANXIA_RPG_FACT_C10_KEEP_LIGHT;
  h2_xianxia_rpg_static_key(&game, &b);
  CHECK(memcmp(&a, &b, sizeof(a)) != 0);
  check_all_layers(&game);
  game.phase = H2_XIANXIA_RPG_PHASE_CHAPTERS;
  game.chapter_page = 0;
  h2_xianxia_rpg_static_key(&game, &a);
  game.chapter_page = 1;
  h2_xianxia_rpg_static_key(&game, &b);
  CHECK(memcmp(&a, &b, sizeof(a)) != 0);
}

int main(void) {
  test_story_string_capacity();
  test_glyph_coverage();
  test_keys();
  static const int32_t sizes[][2] = {{640, 480}, {480, 800}, {480, 480},
                                     {368, 448}, {480, 320}, {320, 240},
                                     {1024, 768}};
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
    test_screens(sizes[i][0], sizes[i][1]);
    test_chapter2_screens(sizes[i][0], sizes[i][1]);
    test_chapter3_screens(sizes[i][0], sizes[i][1]);
    test_chapter4_screens(sizes[i][0], sizes[i][1]);
    test_chapter5_screens(sizes[i][0], sizes[i][1]);
    test_chapter6_screens(sizes[i][0], sizes[i][1]);
    test_chapter7_screens(sizes[i][0], sizes[i][1]);
    test_chapter8_screens(sizes[i][0], sizes[i][1]);
    test_chapter9_screens(sizes[i][0], sizes[i][1]);
    test_chapter10_screens(sizes[i][0], sizes[i][1]);
  }
  printf("xianxia_rpg render tests passed\n");
  return 0;
}
