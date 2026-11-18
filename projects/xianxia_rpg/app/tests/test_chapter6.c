/* Pointer-driven chapter-6 regression: routes, thermal retreat, sliding,
 * ritual failure/retry and independent per-chapter records. */
#include "h2_xianxia_rpg_game.h"
#include "h2_xianxia_rpg_story.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "%s:%d %s map=%d stage=%d at=%d,%d flags=%x facts=%x\n", \
              __FILE__, __LINE__, #c, (int)g.map, (int)g.stage, (int)g.tx,     \
              (int)g.ty, (unsigned)g.flags, (unsigned)g.facts);                \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)
static h2_xianxia_rpg_game_t g;
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
  tap((float)g.layout.map_x + ((float)x + .5f) * (float)g.layout.tile,
      (float)g.layout.map_y + ((float)y + .5f) * (float)g.layout.tile);
  for (int i = 0; i < 1000 && !g.dialogue.active &&
                  (g.moving || g.path_pos < g.path_len || g.target_x >= 0);
       ++i)
    step(20);
}
static void talk(int x, int y) {
  go(x, y);
  CHECK(g.dialogue.active);
  dialog();
}
static void reload(void) {
  h2_xianxia_rpg_writes_t w;
  CHECK(h2_xianxia_rpg_game_take_writes(&g, &w));
  CHECK(w.journey6 && !w.journey5 && !w.journey4 && !w.journey3 && !w.journey &&
        !w.chapter1);
  uint8_t bytes[H2_XIANXIA_RPG_JOURNEY6_BYTES];
  h2_xianxia_rpg_journey6_encode(&w.journey6_save, 123, bytes);
  h2_xianxia_rpg_records_t r = g.records;
  uint32_t tag = 0;
  CHECK(
      h2_xianxia_rpg_journey6_decode(bytes, sizeof(bytes), &r.journey6, &tag) &&
      tag == 123);
  h2_xianxia_rpg_game_init(&g, 640, 480, 7, &r);
  step(300);
  box(&g.layout.title_buttons[1]);
  step(300);
  CHECK(g.chapter == 6 && h2_xianxia_rpg_walkable(&g, g.map, g.tx, g.ty));
  if (g.dialogue.active) dialog();
}
static void begin(void) {
  h2_xianxia_rpg_game_init(&g, 640, 480, 7, NULL);
  step(300);
  box(&g.layout.title_buttons[2]);
  step(300);
  box(&g.layout.chapter_rows[5]);
  CHECK(g.phase == H2_XIANXIA_RPG_PHASE_PROLOGUE && !g.prologue_carry);
  step(1000);
  tap(320, 200);
  dialog();
  CHECK(g.hero.level == 9 && g.hero.pills == 3);
  talk(7, 3);
  CHECK(g.stage == H2_XIANXIA_RPG_STAGE_C6_HERBS &&
        !h2_xianxia_rpg_ally_joined(&g));
}
static void route(bool cold) {
  box(&g.layout.escort_actions[cold ? 1 : 0]);
  dialog();
  reload();
}
static void back(void) {
  box(&g.layout.escort_actions[0]);
  dialog();
  CHECK(g.map == H2_XIANXIA_RPG_MAP_HERB_HUT);
}
static void hot(void) {
  route(false);
  go(4, 4);
  CHECK(g.dialogue.active && g.tx == 1 && (g.flags & 15u) == 4);
  dialog();
  reload();
  go(3, 4);
  CHECK((g.flags & 15u) == 6);
  talk(3, 3);
  CHECK((g.flags & 15u) == 4);
  reload();
  go(6, 4);
  CHECK((g.flags & 15u) == 6);
  talk(6, 3);
  reload();
  talk(12, 2);
  CHECK(g.facts & H2_XIANXIA_RPG_FACT_C6_HOT);
  reload();
  back();
}
static void cold(void) {
  route(true);
  go(2, 4);
  CHECK(g.tx == 5 && g.ty == 4 && (g.flags & 15u) == 3);
  reload();
  const uint32_t flags = g.flags;
  step(60000);
  CHECK(g.flags == flags);
  go(11, 1);
  CHECK(g.tx == 5 && g.ty == 4); /* No diagonal auto-path. */
  go(5, 3);
  CHECK(g.tx == 5 && g.ty == 1 && (g.flags & 15u) == 2);
  reload();
  go(6, 1);
  CHECK(g.tx == 11 && g.ty == 1 && (g.flags & 15u) == 1);
  go(11, 2);
  CHECK(g.dialogue.active && g.tx == 1 && g.ty == 4);
  dialog();
  reload();
  go(5, 4);
  go(5, 1);
  go(11, 1);
  talk(11, 0);
  CHECK((g.flags & 15u) == 4);
  reload();
  go(11, 6);
  CHECK(g.ty == 6);
  talk(11, 7);
  reload();
  go(13, 6);
  go(13, 2);
  talk(12, 2);
  CHECK(g.facts & H2_XIANXIA_RPG_FACT_C6_COLD);
  reload();
  back();
}
static void ritual(bool fail) {
  talk(7, 1);
  CHECK(g.stage == H2_XIANXIA_RPG_STAGE_C6_RITUAL);
  reload();
  const uint32_t before = g.flags;
  step(60000);
  CHECK(g.flags == before);
  box(&g.layout.commands[3]);
  CHECK(g.stage == H2_XIANXIA_RPG_STAGE_C6_RITUAL && g.flags == before);
  reload();
  if (fail) {
    for (int i = 0; i < 12 && g.stage == H2_XIANXIA_RPG_STAGE_C6_RITUAL; ++i)
      box(&g.layout.commands[h2_xianxia_rpg_c6_breath(&g) > 0 ? 1 : 0]);
    CHECK(g.stage == H2_XIANXIA_RPG_STAGE_C6_READY && g.dialogue.active);
    reload();
    CHECK((g.flags & 255u) == 4 && (g.facts & 3u));
    talk(7, 1);
  }
  for (int i = 0; i < 5; ++i) {
    box(&g.layout.commands[g.turns_taken % 6u == 2u           ? 2
                           : h2_xianxia_rpg_c6_breath(&g) > 0 ? 0
                                                              : 1]);
    CHECK(((g.flags >> 4) & 7u) == (uint32_t)i + 1u);
    reload();
  }
  CHECK(g.stage == H2_XIANXIA_RPG_STAGE_C6_RECOVERED &&
        h2_xianxia_rpg_ally_joined(&g));
  CHECK(h2_xianxia_rpg_rapport_max(&g) == 4);
  talk(7, 3);
  CHECK(g.phase == H2_XIANXIA_RPG_PHASE_ENDING);
  CHECK((g.records.progress.cleared & 32u) &&
        g.records.progress.reserved[5] == g.facts);
  reload();
  CHECK(g.phase == H2_XIANXIA_RPG_PHASE_ENDING && g.hero.pills == 3);
}
int main(void) {
  begin();
  hot();
  CHECK((g.facts & 3u) == 1u);
  ritual(true);
  begin();
  cold();
  CHECK((g.facts & 3u) == 2u && (g.facts & H2_XIANXIA_RPG_FACT_C6_FIRST_COLD));
  ritual(false);
  begin();
  cold();
  hot();
  CHECK((g.facts & 3u) == 3u);
  ritual(false);
  /* A v0.5 active save takes Continue, while chapter 6 keeps its checkpoint. */
  h2_xianxia_rpg_records_t r = g.records;
  r.journey6.stage = H2_XIANXIA_RPG_STAGE_C6_READY;
  r.has_journey5 = r.journey5_latest = true;
  r.journey6_latest = false;
  r.journey5 =
      (h2_xianxia_rpg_save_t){.chapter = 5,
                              .stage = H2_XIANXIA_RPG_STAGE_C5_ARRIVE,
                              .level = 8,
                              .pills = 2,
                              .map = H2_XIANXIA_RPG_MAP_WATER_MARKET,
                              .x = 6,
                              .y = 4,
                              .rest_map = H2_XIANXIA_RPG_MAP_WATER_MARKET,
                              .rest_x = 6,
                              .rest_y = 4};
  h2_xianxia_rpg_game_init(&g, 640, 480, 7, &r);
  CHECK(h2_xianxia_rpg_continue_save(&g)->chapter == 5 &&
        h2_xianxia_rpg_unfinished(&g, 6));
  /* No completion-table replacement when a future table is opaque. */
  r.journey6_latest = true;
  r.progress_unreadable = true;
  r.journey6.stage = H2_XIANXIA_RPG_STAGE_C6_RECOVERED;
  h2_xianxia_rpg_game_init(&g, 640, 480, 7, &r);
  step(300);
  box(&g.layout.title_buttons[1]);
  step(300);
  dialog();
  talk(7, 3);
  h2_xianxia_rpg_writes_t w;
  CHECK(h2_xianxia_rpg_game_take_writes(&g, &w) && w.journey6 && !w.progress);
  /* Unknown facts / malformed bytes are never silently accepted. */
  uint8_t bytes[H2_XIANXIA_RPG_JOURNEY6_BYTES];
  h2_xianxia_rpg_save_t decoded;
  w.journey6_save.facts |= 1u << 16;
  h2_xianxia_rpg_journey6_encode(&w.journey6_save, 0, bytes);
  CHECK(!h2_xianxia_rpg_journey6_decode(bytes, sizeof(bytes), &decoded, NULL));
  puts("xianxia_rpg chapter6 tests passed");
  return 0;
}
