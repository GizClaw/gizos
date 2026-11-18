/* Chapter 7 uses the shipping pointer routes, accepted battle commands and
 * codecs: paired-era reachability, feedback replay, retry and rollback. */
#include "h2_xianxia_rpg_game.h"
#include "h2_xianxia_rpg_story.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c)                                                                   \
  do {                                                                             \
    if (!(c)) {                                                                    \
      fprintf(                                                                     \
          stderr,                                                                  \
          "%s:%d %s stage=%d map=%d pos=%d,%d flags=%x facts=%x hp=%u enemy=%u\n", \
          __FILE__, __LINE__, #c, (int)g.stage, (int)g.map, (int)g.tx,             \
          (int)g.ty, (unsigned)g.flags, (unsigned)g.facts,                         \
          (unsigned)g.hero.hp, (unsigned)g.battle.enemy_hp);                       \
      exit(1);                                                                     \
    }                                                                              \
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
static void reload(bool feedback) {
  h2_xianxia_rpg_writes_t w;
  CHECK(h2_xianxia_rpg_game_take_writes(&g, &w));
  CHECK(w.journey7 && !w.journey6 && !w.journey5 && !w.chapter1 && !w.journey);
  h2_xianxia_rpg_records_t r = g.records;
  uint8_t bytes[H2_XIANXIA_RPG_JOURNEY7_BYTES];
  h2_xianxia_rpg_journey7_encode(&w.journey7_save, 123u, bytes);
  uint32_t tag = 0;
  CHECK(
      h2_xianxia_rpg_journey7_decode(bytes, sizeof(bytes), &r.journey7, &tag) &&
      tag == 123u);
  CHECK(
      !h2_xianxia_rpg_journey6_decode(bytes, sizeof(bytes), &r.journey6, NULL));
  h2_xianxia_rpg_game_init(&g, 640, 480, 7, &r);
  step(300);
  box(&g.layout.title_buttons[1]);
  step(300);
  CHECK(g.chapter == 7 && h2_xianxia_rpg_walkable(&g, g.map, g.tx, g.ty));
  CHECK(g.dialogue.active == feedback);
}
static void begin(void) {
  h2_xianxia_rpg_game_init(&g, 640, 480, 7, NULL);
  step(300);
  box(&g.layout.title_buttons[2]);
  step(300);
  box(&g.layout.chapter_rows[6]);
  CHECK(g.phase == H2_XIANXIA_RPG_PHASE_PROLOGUE && !g.prologue_carry);
  step(1000);
  tap(320, 200);
  dialog();
  CHECK(g.chapter == 7 && g.hero.level == 9 &&
        h2_xianxia_rpg_rapport_max(&g) == 4u);
}
static void routes(void) {
  CHECK(!h2_xianxia_rpg_walkable(&g, g.map, 7, 4));
  go(10, 4);
  CHECK(g.tx == 3); /* Present bridge is truly impassable. */
  talk(3, 3);
  CHECK(g.flags & H2_XIANXIA_RPG_FLAG_C7_PAST);
  reload(false);
  CHECK(h2_xianxia_rpg_walkable(&g, g.map, 7, 4));
  CHECK(!h2_xianxia_rpg_walkable(&g, g.map, 7, 6));
  talk(11, 3);
  CHECK(g.facts & H2_XIANXIA_RPG_FACT_C7_LAMP);
  reload(false);
  talk(11, 5);
  CHECK(!(g.flags & H2_XIANXIA_RPG_FLAG_C7_PAST));
  CHECK(h2_xianxia_rpg_walkable(&g, g.map, 7, 6));
  go(3, 6);
  CHECK(g.tx == 3 && g.ty == 6); /* Return over lit present route. */
  talk(14, 6);
  CHECK(g.map == H2_XIANXIA_RPG_MAP_MIRROR_HALL);
  reload(false);
  talk(3, 3);
  CHECK(!h2_xianxia_rpg_walkable(&g, g.map, 7, 2));
  go(11, 2);
  CHECK(g.tx == 3); /* Past ford waits for present water wheel. */
  talk(3, 3);
  talk(5, 3);
  CHECK(g.facts & H2_XIANXIA_RPG_FACT_C7_WATER);
  reload(false);
  talk(3, 3);
  CHECK(h2_xianxia_rpg_walkable(&g, g.map, 7, 2));
  talk(11, 2);
  CHECK(g.stage == H2_XIANXIA_RPG_STAGE_C7_QUESTION1);
  reload(false);
}
static void answers(unsigned bits) {
  for (unsigned q = 0; q < 3; ++q) {
    const unsigned choice = (bits >> q) & 1u;
    const uint32_t before = g.trust;
    box(&g.layout.escort_actions[choice]);
    CHECK(g.dialogue.active && (g.flags & H2_XIANXIA_RPG_FLAG_C7_FEEDBACK));
    CHECK(g.dialogue.script ==
          h2_xianxia_rpg_script(
              (h2_xianxia_rpg_script_id_t)(H2_XIANXIA_RPG_SCRIPT_C7_ANSWER1A +
                                           2u * q + choice)));
    CHECK(g.trust == (choice ? (before < 3u ? before + 1 : 3u)
                             : (before ? before - 1 : 0)));
    const uint32_t trust = g.trust;
    reload(true);
    CHECK(g.trust == trust);
    dialog();
    CHECK(!(g.flags & H2_XIANXIA_RPG_FLAG_C7_FEEDBACK));
    reload(false);
  }
  CHECK(g.stage == H2_XIANXIA_RPG_STAGE_C7_DUEL &&
        ((g.facts >> 8) & 7u) == bits);
  talk(11, 5); /* Both eras are usable after choices; saved safe landing. */
  reload(false);
}
static void fight(void) {
  talk(13, 5);
  CHECK(g.phase == H2_XIANXIA_RPG_PHASE_BATTLE);
  step(1000);
}
static void command(unsigned cmd) {
  CHECK(g.battle.state == H2_XIANXIA_RPG_BATTLE_COMMAND);
  box(&g.layout.commands[cmd]);
  step(5000);
}
static void battle(void) {
  fight();
  CHECK(g.battle.mirror_command == H2_XIANXIA_RPG_COMMAND_GUARD);
  const uint32_t turns = g.turns_taken;
  command(4);
  CHECK(g.turns_taken == turns && g.battle.mirror_command == 3u);
  command(0);
  CHECK(g.battle.mirror_command == 0u && g.hero.hp == g.hero.hp_max);
  const uint32_t before = g.hero.hp;
  command(3);
  CHECK(g.hero.hp < before && g.battle.mirror_command == 3u);
  command(2);
  CHECK(g.battle.mirror_command == 2u && g.hero.pills == 2u);
  const uint32_t enemy = g.battle.enemy_hp;
  command(3);
  CHECK(g.battle.enemy_hp > enemy && g.hero.pills == 2u);
  /* Attack recklessly until a real defeat; replay preserves puzzle/answers. */
  for (int i = 0; i < 30 && g.battle.state == H2_XIANXIA_RPG_BATTLE_COMMAND;
       ++i)
    command(0);
  CHECK(g.battle.state == H2_XIANXIA_RPG_BATTLE_DEFEAT);
  step(600);
  tap(320, 200);
  dialog();
  CHECK(g.phase == H2_XIANXIA_RPG_PHASE_EXPLORE && g.tx == 11 && g.ty == 4);
  reload(false);
  fight();
  CHECK(g.battle.mirror_command == 3u);
  /* No consumable required. Guard always yields another safe attack window. */
  g.hero.pills = 0;
  for (int i = 0; i < 80 && g.battle.state == H2_XIANXIA_RPG_BATTLE_COMMAND;
       ++i) {
    const bool safe = g.battle.mirror_command == 3u;
    command(safe && g.hero.mp >= 7u ? 1u : 3u);
  }
  CHECK(g.battle.state == H2_XIANXIA_RPG_BATTLE_VICTORY);
  reload(true);
  CHECK(g.stage == H2_XIANXIA_RPG_STAGE_C7_REVEAL);
  dialog();
  CHECK(g.phase == H2_XIANXIA_RPG_PHASE_ENDING &&
        (g.records.progress.cleared & 64u));
  reload(false);
  CHECK(g.phase == H2_XIANXIA_RPG_PHASE_ENDING);
}
int main(void) {
  begin();
  routes();
  answers(7u);
  battle();
  CHECK(g.records.progress.reserved[6] == 15u);
  begin();
  routes();
  answers(0u);
  CHECK(g.trust == 0u);
  battle(); /* The least trusting default party also needs no pills. */
  h2_xianxia_rpg_records_t r = g.records;
  r.journey7.stage = H2_XIANXIA_RPG_STAGE_C7_REVEAL;
  for (unsigned i = 0; i < H2_XIANXIA_RPG_CHAPTER_MAX; ++i)
    if (i != 6u) r.progress.reserved[i] = (uint8_t)(160u + i);
  r.progress.reserved[5] = 30u;
  uint8_t before[H2_XIANXIA_RPG_PROGRESS_BYTES],
      after[H2_XIANXIA_RPG_PROGRESS_BYTES];
  h2_xianxia_rpg_progress_encode(&r.progress, before);
  h2_xianxia_rpg_game_init(&g, 640, 480, 7, &r);
  step(300);
  box(&g.layout.title_buttons[1]);
  dialog();
  h2_xianxia_rpg_writes_t clear;
  CHECK(h2_xianxia_rpg_game_take_writes(&g, &clear) && clear.progress &&
        clear.journey7 && !clear.journey6);
  h2_xianxia_rpg_progress_encode(&clear.progress_record, after);
  for (unsigned i = 0; i < H2_XIANXIA_RPG_CHAPTER_MAX; ++i)
    if (i != 6u)
      CHECK(memcmp(before + 4 + 16 * i, after + 4 + 16 * i, 16) == 0);
  r.progress_unreadable = true;
  h2_xianxia_rpg_game_init(&g, 640, 480, 7, &r);
  step(300);
  box(&g.layout.title_buttons[1]);
  dialog();
  CHECK(h2_xianxia_rpg_game_take_writes(&g, &clear) && !clear.progress &&
        clear.journey7);
  h2_xianxia_rpg_writes_t w;
  g.save_pending = true;
  CHECK(h2_xianxia_rpg_game_take_writes(&g, &w));
  uint8_t bytes[36];
  w.journey7_save.facts |= 1u << 20;
  h2_xianxia_rpg_journey7_encode(&w.journey7_save, 0, bytes);
  CHECK(!h2_xianxia_rpg_journey7_decode(bytes, 36, &w.journey7_save, NULL));
  puts(
      "chapter7: era routes, choices, echo commands, retry and checkpoints passed");
  return 0;
}
