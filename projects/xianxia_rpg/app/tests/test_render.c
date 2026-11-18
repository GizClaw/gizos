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

static void test_glyph_coverage(void) {
  const char* strings[1024];
  const size_t count = h2_xianxia_rpg_story_strings(strings, 1024);
  CHECK(count > 200u && count <= 1024u);
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
  static const char* const titles[] = {
      "竹溪仙缘", "竹溪灯影", "渡口迷雾", "终", "0123456789", "+",
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

int main(void) {
  test_glyph_coverage();
  test_keys();
  static const int32_t sizes[][2] = {{640, 480}, {480, 800}, {480, 480},
                                     {368, 448}, {480, 320}, {320, 240},
                                     {1024, 768}};
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
    test_screens(sizes[i][0], sizes[i][1]);
    test_chapter2_screens(sizes[i][0], sizes[i][1]);
  }
  printf("xianxia_rpg render tests passed\n");
  return 0;
}
