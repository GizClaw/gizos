/* Final chapter production pointer flow, checkpoints, zero-item inherited
 * play and independently bounded storage formats. */
#include "h2_xianxia_rpg_game.h"
#include "h2_xianxia_rpg_story.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static h2_xianxia_rpg_game_t g;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s stage=%u phase=%u map=%u hp=%u step=%u round=%u flags=%x\n", __LINE__, #c, (unsigned)g.stage, (unsigned)g.phase, (unsigned)g.map, (unsigned)g.hero.hp, (unsigned)g.battle.formation_step, (unsigned)g.battle.formation_round, (unsigned)g.flags); exit(1); } } while (0)
static void step(uint32_t ms) { h2_xianxia_rpg_game_step(&g, ms); }
static void tap(float x, float y) { h2_xianxia_rpg_game_pointer(&g, H2_XIANXIA_RPG_POINTER_DOWN, x, y); }
static void box(const h2_xianxia_rpg_rect_t* r) { tap((float)(r->x + r->w / 2), (float)(r->y + r->h / 2)); }
static void dialog(void) {
  for (unsigned i = 0; i < 1200 && g.dialogue.active; ++i) { step(80); tap(160, 100); }
  CHECK(!g.dialogue.active);
}
static void go(int x, int y) {
  tap((float)g.layout.map_x + ((float)x + .5f) * (float)g.layout.tile,
      (float)g.layout.map_y + ((float)y + .5f) * (float)g.layout.tile);
  for (unsigned i = 0; i < 1000 && !g.dialogue.active && (g.moving || g.path_pos < g.path_len || g.target_x >= 0); ++i) step(20);
}
static void checkpoint(void) {
  h2_xianxia_rpg_writes_t w;
  if (!h2_xianxia_rpg_game_take_writes(&g, &w)) return;
  CHECK(w.journey10 && !w.journey9 && !w.journey8 && !w.journey7 && !w.journey6 && !w.journey5 && !w.journey4 && !w.journey3 && !w.journey && !w.chapter1);
  uint8_t bytes[36]; uint32_t tag = 0; h2_xianxia_rpg_save_t copy;
  h2_xianxia_rpg_journey10_encode(&w.journey10_save, 42u, bytes);
  CHECK(h2_xianxia_rpg_journey10_decode(bytes, sizeof(bytes), &copy, &tag) && tag == 42u);
  CHECK(!h2_xianxia_rpg_journey9_decode(bytes, sizeof(bytes), &copy, NULL));
  CHECK(copy.facts == g.facts);
}
static void reload(void) {
  checkpoint();
  const h2_xianxia_rpg_records_t r = g.records;
  int w = g.width, h = g.height;
  h2_xianxia_rpg_game_init(&g, w, h, 10u, &r);
  step(300); box(&g.layout.title_buttons[1]); step(300);
  CHECK(g.chapter == 10u && h2_xianxia_rpg_walkable(&g, g.map, g.tx, g.ty));
}
static void begin(int w, int h, const h2_xianxia_rpg_records_t* r) {
  h2_xianxia_rpg_game_init(&g, w, h, 10u, r);
  step(300); box(&g.layout.title_buttons[2]); step(300);
  if (h < 400) box(&g.layout.chapter_more);
  box(&g.layout.chapter_rows[9]); CHECK(g.phase == H2_XIANXIA_RPG_PHASE_PROLOGUE);
  step(1000); tap(160, 100); dialog();
  CHECK(g.chapter == 10 && g.stage == H2_XIANXIA_RPG_STAGE_C10_ARRANGE);
}
static void arrange(void) {
  go(13, 4); dialog(); CHECK(g.stage == H2_XIANXIA_RPG_STAGE_C10_ARRANGE);
  const int xs[] = {4,6,9,11};
  for (unsigned i = 0; i < 4; ++i) {
    go(xs[i], 3); CHECK(g.glyph_view == 1 && g.node_room == i);
    box(&g.layout.glyph_choices[i % 3u]);
    CHECK(!g.glyph_view); reload();
  }
  CHECK(h2_xianxia_rpg_c10_arranged(g.flags));
  CHECK(h2_xianxia_rpg_c10_support(g.flags, 0u) == 2u);
  go(13, 4); CHECK(g.stage == H2_XIANXIA_RPG_STAGE_C10_SHRINE);
}
static void cmd(unsigned c) {
  CHECK(g.battle.state == H2_XIANXIA_RPG_BATTLE_COMMAND);
  const uint32_t turns = g.turns_taken;
  if (c == 5u) box(&g.layout.party[1]); else box(&g.layout.commands[c]);
  step(5000); CHECK(g.turns_taken == turns + 1u);
}
static void fight(bool fail) {
  const h2_xianxia_rpg_stage_t stage = g.stage;
  go(7, 3); dialog(); step(1000);
  CHECK(g.battle.finale && g.phase == H2_XIANXIA_RPG_PHASE_BATTLE);
  cmd(5); reload(); CHECK(g.stage == stage && g.phase == H2_XIANXIA_RPG_PHASE_EXPLORE);
  go(7, 3); dialog(); step(1000);
  if (fail) {
    for (unsigned i = 0; i < 30 && g.battle.state == H2_XIANXIA_RPG_BATTLE_COMMAND; ++i) cmd(0);
    CHECK(g.battle.state == H2_XIANXIA_RPG_BATTLE_DEFEAT);
    tap(160, 100); dialog(); CHECK(g.stage == stage && g.hero.hp == g.hero.hp_max);
    reload(); go(7, 3); dialog(); step(1000);
  }
  for (unsigned i = 0; i < 15 && g.battle.state == H2_XIANXIA_RPG_BATTLE_COMMAND; ++i)
    cmd(h2_xianxia_rpg_c10_instruction(&g));
  CHECK(g.battle.state == H2_XIANXIA_RPG_BATTLE_VICTORY);
  CHECK(g.stage == stage + 1 && g.battle.formation_round == 2u);
  reload();
  if (g.dialogue.active) dialog();
}
static void complete(unsigned choice) {
  CHECK(g.stage == H2_XIANXIA_RPG_STAGE_C10_CHOICE && g.glyph_view == 2u);
  box(&g.layout.glyph_choices[choice]); dialog();
  CHECK(g.facts & H2_XIANXIA_RPG_FACT_C10_CHOSEN);
  CHECK(((g.facts & H2_XIANXIA_RPG_FACT_C10_KEEP_LIGHT) != 0u) == (choice == 1u));
  reload();
  for (unsigned i = 0; i < 3; ++i) { go(7, 3); dialog(); }
  CHECK(g.phase == H2_XIANXIA_RPG_PHASE_ENDING && g.stage == H2_XIANXIA_RPG_STAGE_C10_CLEARED);
  CHECK(g.records.progress.cleared & 512u);
  const uint32_t turns = g.turns_taken;
  reload(); CHECK(g.phase == H2_XIANXIA_RPG_PHASE_ENDING && g.turns_taken == turns);
  step(1600); tap(160, 100); CHECK(g.phase == H2_XIANXIA_RPG_PHASE_TITLE);
}
static void playthrough(int w, int h, unsigned choice) {
  begin(w, h, NULL); arrange();
  for (unsigned i = 0; i < 3; ++i) fight(true);
  complete(choice);
}
static void codec(void) {
  h2_xianxia_rpg_save_t s = g.records.journey10, out;
  uint8_t bytes[36];
  h2_xianxia_rpg_journey10_encode(&s, 0, bytes);
  CHECK(h2_xianxia_rpg_journey10_decode(bytes, 36, &out, NULL));
  for (size_t n = 0; n < 36; ++n) CHECK(!h2_xianxia_rpg_journey10_decode(bytes, n, &out, NULL));
  CHECK(!h2_xianxia_rpg_journey10_decode(NULL, 36, &out, NULL));
  bytes[0] = 4; CHECK(!h2_xianxia_rpg_journey10_decode(bytes, 36, &out, NULL));
  s.facts |= 0x80000000u;
  h2_xianxia_rpg_journey10_encode(&s, 0, bytes); CHECK(!h2_xianxia_rpg_journey10_decode(bytes, 36, &out, NULL));
  s = g.records.journey10; s.flags &= ~3u;
  h2_xianxia_rpg_journey10_encode(&s, 0, bytes); CHECK(!h2_xianxia_rpg_journey10_decode(bytes, 36, &out, NULL));
  s = g.records.journey10; s.facts &= ~H2_XIANXIA_RPG_FACT_C10_CHOSEN;
  h2_xianxia_rpg_journey10_encode(&s, 0, bytes); CHECK(!h2_xianxia_rpg_journey10_decode(bytes, 36, &out, NULL));
  s = g.records.journey10; s.chapter = 9;
  h2_xianxia_rpg_journey10_encode(&s, 0, bytes); CHECK(!h2_xianxia_rpg_journey10_decode(bytes, 36, &out, NULL));
}
static void inheritance(void) {
  h2_xianxia_rpg_records_t r = {0};
  r.has_journey9 = r.journey9_latest = true;
  r.journey9 = (h2_xianxia_rpg_save_t){.chapter=9, .stage=H2_XIANXIA_RPG_STAGE_C9_CLEARED,
    .level=1, .pills=0, .map=H2_XIANXIA_RPG_MAP_LANTERN_CORE, .x=7, .y=5,
    .rest_map=H2_XIANXIA_RPG_MAP_LANTERN_CORE, .rest_x=7, .rest_y=5, .flags=255,
    .facts=0x0fffffffu, .trust=3};
  r.progress_unreadable = true;
  begin(320, 240, &r);
  CHECK(g.hero.level == 1u && g.hero.pills == 0u);
  CHECK((g.facts & 0x0fffffffu) == r.journey9.facts);
  arrange();
  for (unsigned i=0; i<3; ++i) fight(false);
  complete(0);
  CHECK(g.records.journey9.facts == r.journey9.facts);
  CHECK(g.hero.pills == 0u);
  CHECK(!g.progress_pending);
}
int main(void) {
  playthrough(640, 480, 0); playthrough(320, 240, 1); codec(); inheritance();
  puts("chapter 10: production clicks, retries, ending, zero-item carry and codecs PASS");
  return 0;
}
