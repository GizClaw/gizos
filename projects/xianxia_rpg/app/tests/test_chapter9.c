/* Production pointer transitions cover no-save discovery, comparison errors,
 * all lessons, four-phase switching, checkpoint replay and record ownership. */
#include "h2_xianxia_rpg_game.h"
#include "h2_xianxia_rpg_story.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static h2_xianxia_rpg_game_t g;
#define CHECK(c)                                                                                           \
  do {                                                                                                     \
    if (!(c)) {                                                                                            \
      fprintf(                                                                                             \
          stderr,                                                                                          \
          "%d: %s chapter=%u stage=%u map=%u pos=%d,%d flags=%x facts=%x hp=%u foe=%u phase=%u turn=%u\n", \
          __LINE__, #c, (unsigned)g.chapter, (unsigned)g.stage,                                            \
          (unsigned)g.map, (int)g.tx, (int)g.ty, (unsigned)g.flags,                                        \
          (unsigned)g.facts, (unsigned)g.hero.hp, (unsigned)g.battle.enemy_hp,                             \
          (unsigned)g.battle.node_phase, (unsigned)g.battle.turn);                                         \
      exit(1);                                                                                             \
    }                                                                                                      \
  } while (0)
static void step(uint32_t ms) { h2_xianxia_rpg_game_step(&g, ms); }
static void tap(float x, float y) {
  h2_xianxia_rpg_game_pointer(&g, H2_XIANXIA_RPG_POINTER_DOWN, x, y);
}
static void box(const h2_xianxia_rpg_rect_t* r) {
  tap((float)(r->x + r->w / 2), (float)(r->y + r->h / 2));
}
static void dialog(void) {
  for (int i = 0; i < 400 && g.dialogue.active; ++i) {
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
static void begin(int w, int h) {
  h2_xianxia_rpg_game_init(&g, w, h, 9u, NULL);
  step(300);
  box(&g.layout.title_buttons[2]);
  step(300);
  if (h < 400) box(&g.layout.chapter_more);
  box(&g.layout.chapter_rows[8]);
  CHECK(g.phase == H2_XIANXIA_RPG_PHASE_PROLOGUE);
  step(1000);
  tap(160, 100);
  dialog();
  CHECK(g.chapter == 9 && g.hero.level == 11 && g.hero.pills == 3);
}
static void checkpoint(void) {
  h2_xianxia_rpg_writes_t w;
  if (!h2_xianxia_rpg_game_take_writes(&g, &w)) return;
  CHECK(w.journey9 && !w.journey8 && !w.journey7 && !w.journey6 &&
        !w.journey5 && !w.journey4 && !w.journey3 && !w.journey && !w.chapter1);
  uint8_t bytes[36];
  uint32_t tag = 0;
  h2_xianxia_rpg_save_t copy;
  h2_xianxia_rpg_journey9_encode(&w.journey9_save, 875u, bytes);
  CHECK(h2_xianxia_rpg_journey9_decode(bytes, sizeof(bytes), &copy, &tag) &&
        tag == 875u);
  CHECK(!h2_xianxia_rpg_journey8_decode(bytes, sizeof(bytes), &copy, NULL));
  CHECK(!h2_xianxia_rpg_journey7_decode(bytes, sizeof(bytes), &copy, NULL));
}
static void reload(void) {
  checkpoint();
  h2_xianxia_rpg_records_t r = g.records;
  int w = g.width, h = g.height;
  h2_xianxia_rpg_game_init(&g, w, h, 9u, &r);
  step(300);
  box(&g.layout.title_buttons[1]);
  step(300);
  CHECK(g.chapter == 9 && h2_xianxia_rpg_walkable(&g, g.map, g.tx, g.ty));
}
static void command(unsigned c) {
  CHECK(g.battle.state == H2_XIANXIA_RPG_BATTLE_COMMAND);
  uint32_t before = g.turns_taken;
  box(&g.layout.commands[c]);
  step(5000);
  CHECK(g.turns_taken == before + 1u);
}
static void win(void) {
  unsigned seen = 0, swaps = 0;
  for (unsigned i = 0;
       i < 100 && g.battle.state == H2_XIANXIA_RPG_BATTLE_COMMAND; ++i) {
    uint32_t p = h2_xianxia_rpg_c9_phase(&g);
    bool want_wen = p == 1u || p == 3u;
    seen |= 1u << p;
    if (g.hero.hp < 65u && g.hero.pills)
      command(2);
    else if (h2_xianxia_rpg_c9_wenzhou(&g) != want_wen) {
      command(4);
      ++swaps;
    } else if (g.battle.veiled ||
               (p == 3u && (g.battle.mirror_command == 0u ||
                            g.battle.mirror_command == 1u)) ||
               g.hero.mp < 7u)
      command(3);
    else
      command(1);
  }
  CHECK(g.battle.state == H2_XIANXIA_RPG_BATTLE_VICTORY);
  if (g.battle.enemy == H2_XIANXIA_RPG_ENEMY_CORE_SHADE)
    CHECK(seen == 15u && swaps >= 3u);
}
static void enter_hall(void) {
  talk(7, 6);
  CHECK(g.map == H2_XIANXIA_RPG_MAP_SHAFT_UPPER); /* Needs local clue. */
  talk(3, 3);
  CHECK(g.facts & H2_XIANXIA_RPG_FACT_C9_BOOK);
  reload();
  box(&g.layout.escort_actions[0]);
  CHECK(g.glyph_view == 1u);
  box(&g.layout.glyph_back);
  CHECK(!g.glyph_view);
  talk(7, 6);
  CHECK(g.map == H2_XIANXIA_RPG_MAP_SHAFT_LOWER);
  reload();
  talk(12, 6);
  CHECK(g.map == H2_XIANXIA_RPG_MAP_ECHO_HALL);
  reload();
  CHECK(g.stage == H2_XIANXIA_RPG_STAGE_C9_ROOMS);
}
static void rooms(void) {
  static const int xy[4][2] = {{3, 2}, {9, 2}, {3, 6}, {9, 6}};
  for (unsigned r = 0; r < 4; ++r) {
    go(xy[r][0], xy[r][1]);
    CHECK(g.glyph_view == 2u && g.node_room == r);
    unsigned bad = 0, good = 0;
    for (unsigned c = 0; c < 3; ++c) {
      if (h2_xianxia_rpg_c9_wrong_glyph(r, c))
        good = c;
      else
        bad = c;
    }
    uint32_t pills = g.hero.pills, flags = g.flags;
    box(&g.layout.glyph_choices[bad]);
    CHECK(g.glyph_wrong && g.flags == flags && g.hero.pills == pills);
    reload();
    CHECK(!g.glyph_view && g.flags == flags);
    go(xy[r][0], xy[r][1]);
    CHECK(g.glyph_view == 2u);
    box(&g.layout.glyph_choices[good]);
    CHECK(g.flags & (1u << r));
    checkpoint(); /* Opened door and unopened trial are independently valid. */
    dialog();
    step(1000);
    CHECK(g.battle.node_battle && g.battle.node_phase == r);
    win();
    reload();
    CHECK(g.phase == H2_XIANXIA_RPG_PHASE_EXPLORE);
    uint32_t exp = g.hero.exp;
    talk(xy[r][0], xy[r][1]);
    CHECK(g.phase == H2_XIANXIA_RPG_PHASE_EXPLORE && g.hero.exp == exp);
  }
  CHECK(g.stage == H2_XIANXIA_RPG_STAGE_C9_FATHER && (g.flags & 255u) == 255u);
}
static void playthrough(int w, int h) {
  begin(w, h);
  enter_hall();
  rooms();
  go(13, 5);
  CHECK(g.map == H2_XIANXIA_RPG_MAP_LANTERN_CORE);
  talk(7, 2);
  CHECK(g.stage == H2_XIANXIA_RPG_STAGE_C9_BATTLE &&
        (g.facts & H2_XIANXIA_RPG_FACT_C9_FATHER));
  reload();
  talk(7, 2);
  step(1000);
  checkpoint();
  command(4);
  reload();
  CHECK(g.phase == H2_XIANXIA_RPG_PHASE_EXPLORE);
  talk(7, 2);
  step(1000);
  /* No healing and repeated blind attacks eventually lose; retry retains doors.
   */
  for (unsigned i = 0;
       i < 100 && g.battle.state == H2_XIANXIA_RPG_BATTLE_COMMAND; ++i)
    command(0);
  CHECK(g.battle.state == H2_XIANXIA_RPG_BATTLE_DEFEAT);
  tap(160, 100);
  dialog();
  CHECK((g.flags & 255u) == 255u && g.hero.hp == g.hero.hp_max);
  reload();
  talk(7, 2);
  step(1000);
  win();
  reload();
  CHECK(g.dialogue.active && g.stage == H2_XIANXIA_RPG_STAGE_C9_REVEAL);
  dialog();
  CHECK(g.phase == H2_XIANXIA_RPG_PHASE_ENDING);
  reload();
  CHECK(g.phase == H2_XIANXIA_RPG_PHASE_ENDING &&
        (g.records.progress.cleared & 256u));
  CHECK((g.records.progress.reserved[8] & 11u) == 11u);
}
static void ownership(void) {
  playthrough(640, 480);
  h2_xianxia_rpg_records_t r = g.records;
  h2_xianxia_rpg_save_t s = r.journey9, out;
  uint8_t bytes[36];
  s.facts |= 1u << 28;
  h2_xianxia_rpg_journey9_encode(&s, 0, bytes);
  CHECK(!h2_xianxia_rpg_journey9_decode(bytes, 36, &out, NULL));
  s = r.journey9;
  s.flags &= ~1u;
  h2_xianxia_rpg_journey9_encode(&s, 0, bytes);
  CHECK(!h2_xianxia_rpg_journey9_decode(bytes, 36, &out, NULL));
  s = r.journey9;
  s.facts &= ~H2_XIANXIA_RPG_FACT_C9_FATHER;
  h2_xianxia_rpg_journey9_encode(&s, 0, bytes);
  CHECK(!h2_xianxia_rpg_journey9_decode(bytes, 36, &out, NULL));
  r.has_journey8 = r.journey8_latest = true;
  r.journey8 = (h2_xianxia_rpg_save_t){.chapter = 8,
                                       .stage = H2_XIANXIA_RPG_STAGE_C8_CLEARED,
                                       .level = 12,
                                       .pills = 2,
                                       .trust = 3,
                                       .exp = 1200,
                                       .flags = 0xC00000CCu,
                                       .facts = 0xFFFFFFu,
                                       .map = H2_XIANXIA_RPG_MAP_SNOW_CAMP,
                                       .x = 7,
                                       .y = 3,
                                       .rest_map = H2_XIANXIA_RPG_MAP_SNOW_CAMP,
                                       .rest_x = 7,
                                       .rest_y = 3};
  r.journey9_latest = false;
  h2_xianxia_rpg_game_init(&g, 640, 480, 9, &r);
  step(300);
  box(&g.layout.title_buttons[1]);
  CHECK(g.chapter == 8);
  step(1600);
  tap(160, 100);
  CHECK(g.chapter == 9 && g.stage == H2_XIANXIA_RPG_STAGE_C9_CLEARED);
  r.has_journey9 = false;
  r.journey9_unreadable = true;
  h2_xianxia_rpg_game_init(&g, 640, 480, 9, &r);
  step(300);
  box(&g.layout.title_buttons[1]);
  step(1600);
  tap(160, 100);
  CHECK(g.phase == H2_XIANXIA_RPG_PHASE_TITLE);
  h2_xianxia_rpg_writes_t writes;
  CHECK(!h2_xianxia_rpg_game_take_writes(&g, &writes));
  r.journey9_unreadable = false;
  r.progress.cleared |= 128u;
  r.progress.chapters[7] = (h2_xianxia_rpg_clear_t){
      .level = 12, .exp = 1200, .pills = 2, .trust = 3, .flags = 0xC00000CCu};
  h2_xianxia_rpg_game_init(&g, 640, 480, 9, &r);
  step(300);
  box(&g.layout.title_buttons[1]);
  step(1600);
  tap(160, 100);
  step(1000);
  tap(160, 100);
  dialog();
  CHECK(g.chapter == 9 && (g.facts & 0xFFFFFFu) == 0xFFFFFFu &&
        g.hero.pills == 2 && g.hero.level == 12 && g.trust == 3);
  CHECK(g.facts & H2_XIANXIA_RPG_FACT_C9_ACTIVE_WEN);
  checkpoint();
  CHECK(memcmp(&g.records.journey8, &r.journey8, sizeof(r.journey8)) == 0);
}
int main(void) {
  ownership();
  playthrough(320, 240);
  puts(
      "chapter9: glyph recovery, all trials, phases, switching, defeat/reload, carry and opaque records passed");
  return 0;
}
