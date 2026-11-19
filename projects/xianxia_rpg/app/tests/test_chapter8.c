/* Shipping pointer paths exercise both rescue orders, snow detours, replay,
 * consumable ownership and both lamp outcomes. No test-only gameplay entry. */
#include "h2_xianxia_rpg_game.h"
#include "h2_xianxia_rpg_story.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static h2_xianxia_rpg_game_t g;
#define CHECK(c)                                                           \
  do {                                                                     \
    if (!(c)) {                                                            \
      fprintf(stderr,                                                      \
              "%d: %s stage=%u map=%u pos=%d,%d facts=%x lamps=%u/%u\n",   \
              __LINE__, #c, (unsigned)g.stage, (unsigned)g.map, (int)g.tx, \
              (int)g.ty, (unsigned)g.facts, (unsigned)g.battle.lamp_hp,    \
              (unsigned)g.battle.west_lamp_hp);                            \
      exit(1);                                                             \
    }                                                                      \
  } while (0)
static void step(uint32_t ms) { h2_xianxia_rpg_game_step(&g, ms); }
static void tap(float x, float y) {
  h2_xianxia_rpg_game_pointer(&g, H2_XIANXIA_RPG_POINTER_DOWN, x, y);
}
static void box(const h2_xianxia_rpg_rect_t* r) {
  tap((float)(r->x + r->w / 2), (float)(r->y + r->h / 2));
}
static void dialog(void) {
  for (int i = 0; i < 300 && g.dialogue.active; ++i) {
    step(80);
    tap(160, 100);
  }
  CHECK(!g.dialogue.active);
}
static void go(int x, int y) {
  for (int attempts = 0; attempts < 3; ++attempts) {
    tap((float)g.layout.map_x + ((float)x + .5f) * (float)g.layout.tile,
        (float)g.layout.map_y + ((float)y + .5f) * (float)g.layout.tile);
    for (int i = 0; i < 1000 && !g.dialogue.active &&
                    (g.moving || g.path_pos < g.path_len || g.target_x >= 0);
         ++i)
      step(20);
    if (g.dialogue.active &&
        g.dialogue.script ==
            h2_xianxia_rpg_script(H2_XIANXIA_RPG_SCRIPT_C8_BLOCK)) {
      dialog();
      continue;
    }
    return;
  }
  CHECK(false);
}
static void talk(int x, int y) {
  go(x, y);
  CHECK(g.dialogue.active);
  dialog();
}
static void action(unsigned i) {
  box(&g.layout.escort_actions[i]);
  dialog();
}
static void reload(bool replay) {
  h2_xianxia_rpg_writes_t w;
  CHECK(h2_xianxia_rpg_game_take_writes(&g, &w));
  CHECK(w.journey8 && !w.journey7 && !w.journey6 && !w.chapter1 && !w.journey);
  uint8_t bytes[36];
  uint32_t tag = 0;
  h2_xianxia_rpg_records_t r = g.records;
  h2_xianxia_rpg_journey8_encode(&w.journey8_save, 7654u, bytes);
  CHECK(
      h2_xianxia_rpg_journey8_decode(bytes, sizeof(bytes), &r.journey8, &tag) &&
      tag == 7654u);
  CHECK(
      !h2_xianxia_rpg_journey7_decode(bytes, sizeof(bytes), &r.journey7, NULL));
  h2_xianxia_rpg_game_init(&g, 640, 480, 8, &r);
  step(300);
  box(&g.layout.title_buttons[1]);
  step(300);
  CHECK(g.chapter == 8 && g.dialogue.active == replay);
  CHECK(h2_xianxia_rpg_walkable(&g, g.map, g.tx, g.ty));
  if (replay) dialog();
}
static void begin(void) {
  h2_xianxia_rpg_game_init(&g, 640, 480, 8, NULL);
  step(300);
  box(&g.layout.title_buttons[2]);
  step(300);
  box(&g.layout.chapter_rows[7]);
  CHECK(g.phase == H2_XIANXIA_RPG_PHASE_PROLOGUE);
  step(1000);
  tap(320, 200);
  dialog();
  CHECK(g.chapter == 8 && g.hero.level == 10 && g.hero.pills == 3);
  reload(false);
}
static void command(unsigned i) {
  CHECK(g.battle.state == H2_XIANXIA_RPG_BATTLE_COMMAND);
  box(&g.layout.commands[i]);
  step(5000);
}
static void rescue(unsigned west, bool pill) {
  begin();
  action(west);
  CHECK(((g.facts & H2_XIANXIA_RPG_FACT_C8_WEST_FIRST) != 0u) == (west != 0u));
  reload(false);
  action(0);
  action(1u - west); /* Cannot silently change first order. */
  CHECK(g.map == H2_XIANXIA_RPG_MAP_SNOW_CAMP);
  action(west);
  if (west) {
    CHECK(!h2_xianxia_rpg_walkable(&g, g.map, 7, 7));
    talk(3, 3);
    reload(false);
  }
  talk(12, 3);
  CHECK(g.hero.pills == (west ? 4u : 5u));
  const uint32_t first_facts = g.facts;
  reload(false);
  talk(12, 3);
  CHECK(g.facts == first_facts && g.hero.pills == (west ? 4u : 5u));
  action(0);
  action(1u - west);
  if (!west) talk(3, 3);
  talk(12, 3);
  CHECK(g.stage == H2_XIANXIA_RPG_STAGE_C8_INJURY);
  CHECK(g.facts & (west ? H2_XIANXIA_RPG_FACT_C8_EAST_BLOCK
                        : H2_XIANXIA_RPG_FACT_C8_WEST_BLOCK));
  reload(true);
  const uint32_t before = g.hero.pills;
  action(pill ? 1u : 0u);
  CHECK(g.stage == H2_XIANXIA_RPG_STAGE_C8_GUARD);
  CHECK((g.facts & H2_XIANXIA_RPG_C8_RESCUED) == H2_XIANXIA_RPG_C8_RESCUED);
  CHECK(g.hero.pills == before - (pill ? 1u : 0u));
  CHECK(((g.facts & H2_XIANXIA_RPG_FACT_C8_COAL) != 0u) == pill);
  reload(false);
  go(3, 7);
  CHECK(g.tx == 3 && g.ty == 7); /* Return across the permanent bypass. */
  action(0);
  talk(west ? 9 : 5, 5);
  reload(false);
  talk(7, 2);
  step(1000);
  CHECK(g.battle.dual_lamps && g.battle.lamp_hp == 60 &&
        g.battle.west_lamp_hp == 60);
  for (int i = 0; i < 12 && g.battle.state == H2_XIANXIA_RPG_BATTLE_COMMAND;
       ++i)
    command(3);
  CHECK(g.battle.state == H2_XIANXIA_RPG_BATTLE_DEFEAT);
  tap(320, 200);
  dialog();
  CHECK(g.map == H2_XIANXIA_RPG_MAP_SNOW_CAMP &&
        g.stage == H2_XIANXIA_RPG_STAGE_C8_GUARD);
  CHECK((g.facts & H2_XIANXIA_RPG_C8_RESCUED) == H2_XIANXIA_RPG_C8_RESCUED);
  reload(false);
  talk(7, 2);
  step(1000);
  /* Refresh during an attempt must retain the pre-fight checkpoint. */
  command(4);
  reload(false);
  CHECK(g.phase == H2_XIANXIA_RPG_PHASE_EXPLORE &&
        g.stage == H2_XIANXIA_RPG_STAGE_C8_GUARD);
  talk(7, 2);
  step(1000);
  for (int i = 0; i < 10 && g.battle.state == H2_XIANXIA_RPG_BATTLE_COMMAND;
       ++i)
    command(4);
  CHECK(g.battle.state == H2_XIANXIA_RPG_BATTLE_VICTORY);
  CHECK(g.battle.turn == (west ? 8u : 6u));
  CHECK(g.battle.lamp_hp > 0 && g.battle.west_lamp_hp > 0);
  reload(true);
  CHECK(g.phase == H2_XIANXIA_RPG_PHASE_ENDING);
  reload(false);
  CHECK(g.records.progress.cleared == 128u &&
        (g.records.progress.reserved[7] & 1u) == west);
  CHECK(g.records.progress.reserved[7] & 4u); /* Wenzhou joined. */
}
static void opaque(void) {
  begin();
  h2_xianxia_rpg_writes_t w;
  g.save_pending = true;
  CHECK(h2_xianxia_rpg_game_take_writes(&g, &w));
  uint8_t bytes[36];
  w.journey8_save.facts |= 1u << 24;
  h2_xianxia_rpg_journey8_encode(&w.journey8_save, 0u, bytes);
  CHECK(!h2_xianxia_rpg_journey8_decode(bytes, sizeof(bytes), &w.journey8_save,
                                        NULL));
  h2_xianxia_rpg_records_t r = g.records;
  r.has_journey8 = false;
  r.journey8_unreadable = true;
  r.has_journey7 = r.journey7_latest = true;
  r.journey7 =
      (h2_xianxia_rpg_save_t){.chapter = 7,
                              .stage = H2_XIANXIA_RPG_STAGE_C7_CLEARED,
                              .level = 10,
                              .pills = 3,
                              .facts = 4095u,
                              .map = H2_XIANXIA_RPG_MAP_MIRROR_HALL,
                              .x = 11,
                              .y = 4,
                              .rest_map = H2_XIANXIA_RPG_MAP_MIRROR_HALL,
                              .rest_x = 11,
                              .rest_y = 4};
  h2_xianxia_rpg_game_init(&g, 640, 480, 8, &r);
  step(300);
  box(&g.layout.title_buttons[1]);
  CHECK(g.phase == H2_XIANXIA_RPG_PHASE_ENDING);
  step(1600);
  tap(320, 200);
  CHECK(g.phase == H2_XIANXIA_RPG_PHASE_TITLE);
  CHECK(!h2_xianxia_rpg_game_take_writes(&g, &w));
}
static void snow_frame_checkpoint(void) {
  begin();
  action(0);
  h2_xianxia_rpg_writes_t w;
  CHECK(h2_xianxia_rpg_game_take_writes(&g, &w));
  tap((float)g.layout.map_x + 6.5f * (float)g.layout.tile,
      (float)g.layout.map_y + 4.5f * (float)g.layout.tile);
  unsigned writes = 0;
  for (unsigned i = 0; i < 40; ++i) {
    step(33);
    if (!h2_xianxia_rpg_game_take_writes(&g, &w)) continue;
    ++writes;
    CHECK(w.journey8 && !g.moving);
    CHECK(w.journey8_save.y == 4u && w.journey8_save.x == 3u + writes);
    CHECK((w.journey8_save.flags & 15u) == writes);
  }
  CHECK(writes == 3u);
}
static void small_menu(void) {
  h2_xianxia_rpg_game_init(&g, 320, 240, 8, NULL);
  step(300);
  box(&g.layout.title_buttons[2]);
  step(300);
  CHECK(g.phase == H2_XIANXIA_RPG_PHASE_CHAPTERS &&
        g.layout.chapter_rows[0].h >= 24);
  box(&g.layout.chapter_more);
  CHECK(g.chapter_page == 1u);
  box(&g.layout.chapter_rows[9]);
  CHECK(g.phase == H2_XIANXIA_RPG_PHASE_PROLOGUE && g.prologue_chapter == 10u);
  h2_xianxia_rpg_game_init(&g, 320, 240, 8, NULL);
  step(300);
  box(&g.layout.title_buttons[2]);
  step(300);
  box(&g.layout.chapter_more);
  box(&g.layout.chapter_more);
  CHECK(g.chapter_page == 0u);
  box(&g.layout.chapter_more);
  box(&g.layout.chapter_rows[7]);
  CHECK(g.phase == H2_XIANXIA_RPG_PHASE_PROLOGUE && g.prologue_chapter == 8u);
}
static void clear_ownership(void) {
  rescue(0, false);
  h2_xianxia_rpg_progress_t before = g.records.progress;
  for (unsigned i = 0; i < 10; ++i) {
    if (i == 7u) continue;
    before.reserved[i] = (uint8_t)(0xA0u + i);
    before.chapters[i] = (h2_xianxia_rpg_clear_t){.level = 2u + i,
                                                  .pills = i % 9u,
                                                  .trust = i % 4u,
                                                  .flags = 0x81234500u + i,
                                                  .exp = 400u + i,
                                                  .turns = 17u + i};
  }
  before.cleared = 0x3FFu;
  g.records.progress = before;
  g.phase = H2_XIANXIA_RPG_PHASE_EXPLORE;
  g.stage = H2_XIANXIA_RPG_STAGE_C8_REVEAL;
  talk(7, 2);
  h2_xianxia_rpg_writes_t w;
  CHECK(h2_xianxia_rpg_game_take_writes(&g, &w) && w.progress && w.journey8);
  uint8_t a[168], b[168];
  h2_xianxia_rpg_progress_encode(&before, a);
  h2_xianxia_rpg_progress_encode(&w.progress_record, b);
  for (unsigned i = 0; i < 10; ++i)
    if (i != 7u) CHECK(memcmp(a + 4u + i * 16u, b + 4u + i * 16u, 16u) == 0);
  g.records.progress_unreadable = true;
  g.phase = H2_XIANXIA_RPG_PHASE_EXPLORE;
  g.stage = H2_XIANXIA_RPG_STAGE_C8_REVEAL;
  step(1000);
  talk(7, 2);
  CHECK(h2_xianxia_rpg_game_take_writes(&g, &w) && w.journey8 && !w.progress);
  /* A player reaching the second team with no pills can still use coal. */
  begin();
  action(0);
  talk(12, 3);
  action(0);
  action(1);
  talk(3, 3);
  talk(12, 3);
  g.hero.pills = 0;
  action(1);
  CHECK(g.stage == H2_XIANXIA_RPG_STAGE_C8_INJURY &&
        (g.facts & H2_XIANXIA_RPG_FACT_C8_COAL));
  action(0);
  CHECK(g.stage == H2_XIANXIA_RPG_STAGE_C8_GUARD && g.hero.pills == 0);
}
int main(void) {
  snow_frame_checkpoint();
  small_menu();
  clear_ownership();
  rescue(0, false);
  rescue(1, true);
  opaque();
  puts(
      "chapter8: both rescue orders, snow, supplies, dual lamps, retry and replay passed");
  return 0;
}
