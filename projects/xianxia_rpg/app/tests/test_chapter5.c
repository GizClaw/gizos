/* Chapter 5 paths use real pointer interactions, including route failures. */
#include "h2_xianxia_rpg_game.h"
#include "h2_xianxia_rpg_story.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c)                                                           \
  do {                                                                     \
    if (!(c)) {                                                            \
      fprintf(stderr, "%s:%d: %s (map=%d stage=%d at=%d,%d flags=%x)\n",   \
              __FILE__, __LINE__, #c, (int)g.map, (int)g.stage, (int)g.tx, \
              (int)g.ty, (unsigned)g.flags);                               \
      exit(1);                                                             \
    }                                                                      \
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
  for (int i = 0; i < 200 && g.dialogue.active; ++i) {
    step(80);
    tap(320, 200);
  }
  CHECK(!g.dialogue.active);
}
static void go(int x, int y) {
  tap((float)g.layout.map_x + ((float)x + .5f) * (float)g.layout.tile,
      (float)g.layout.map_y + ((float)y + .5f) * (float)g.layout.tile);
  for (int i = 0; i < 800 && !g.dialogue.active &&
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
  (void)h2_xianxia_rpg_game_take_writes(&g, &w);
  const h2_xianxia_rpg_records_t r = g.records;
  h2_xianxia_rpg_game_init(&g, 640, 480, 83, &r);
  step(300);
  box(&g.layout.title_buttons[1]);
  step(300);
  CHECK(h2_xianxia_rpg_walkable(&g, g.map, g.tx, g.ty));
}
static void wait_command(void) {
  for (int i = 0; i < 400 && g.battle.state != H2_XIANXIA_RPG_BATTLE_COMMAND &&
                  g.battle.state != H2_XIANXIA_RPG_BATTLE_VICTORY &&
                  g.battle.state != H2_XIANXIA_RPG_BATTLE_DEFEAT;
       ++i)
    step(50);
}
static void cmd(h2_xianxia_rpg_command_t c) {
  CHECK(g.battle.state == H2_XIANXIA_RPG_BATTLE_COMMAND);
  box(&g.layout.commands[c]);
  wait_command();
}
static void begin(uint32_t seed) {
  h2_xianxia_rpg_game_init(&g, 640, 480, seed, NULL);
  step(300);
  box(&g.layout.title_buttons[2]);
  step(300);
  box(&g.layout.chapter_rows[4]);
  CHECK(g.phase == H2_XIANXIA_RPG_PHASE_PROLOGUE && !g.prologue_carry);
  step(1000);
  tap(320, 200);
  dialog();
  CHECK(g.chapter == 5u && g.hero.level == 8u && g.hero.pills == 3u);
  talk(7, 3);
  CHECK(g.stage == H2_XIANXIA_RPG_STAGE_C5_PAGE);
}
static void tower(bool give) {
  go(14, 4);
  CHECK(g.map == H2_XIANXIA_RPG_MAP_OLD_CITY);
  CHECK(h2_xianxia_rpg_walkable(&g, g.map, 4, 6));
  CHECK(!h2_xianxia_rpg_walkable(&g, g.map, 6, 2));
  go(14, 2);
  CHECK(g.map == H2_XIANXIA_RPG_MAP_OLD_CITY);
  talk(11, 7);
  CHECK(g.flags & H2_XIANXIA_RPG_FLAG_C5_PAGE);
  reload();
  CHECK(g.stage == H2_XIANXIA_RPG_STAGE_C5_TOWER);
  go(3, 3);
  CHECK(g.dialogue.active && (g.flags & H2_XIANXIA_RPG_FLAG_C5_HIGH));
  reload();
  CHECK(g.tx == 3 && g.ty == 4);
  CHECK(!h2_xianxia_rpg_walkable(&g, g.map, 4, 6));
  CHECK(h2_xianxia_rpg_walkable(&g, g.map, 6, 2));
  go(14, 2);
  CHECK(g.map == H2_XIANXIA_RPG_MAP_WEN_TOWER);
  talk(7, 3);
  CHECK(g.stage == H2_XIANXIA_RPG_STAGE_C5_CHOICE);
  reload();
  CHECK(g.stage == H2_XIANXIA_RPG_STAGE_C5_CHOICE);
  box(&g.layout.escort_actions[give ? 1 : 0]);
  CHECK(((g.flags & H2_XIANXIA_RPG_FLAG_C5_RETURN_LIGHT) != 0) == give);
  dialog();
  CHECK(g.stage == H2_XIANXIA_RPG_STAGE_C5_DUEL);
  reload();
  talk(11, 3);
  wait_command();
  CHECK(g.battle.enemy == H2_XIANXIA_RPG_ENEMY_WENZHOU);
}
static void win(bool attack) {
  for (int i = 0; i < 7 && g.battle.state == H2_XIANXIA_RPG_BATTLE_COMMAND;
       ++i) {
    const uint8_t stance = g.battle.sword_stance;
    cmd(attack && stance == 1u ? H2_XIANXIA_RPG_COMMAND_SKILL
                               : H2_XIANXIA_RPG_COMMAND_GUARD);
  }
  if (g.battle.state != H2_XIANXIA_RPG_BATTLE_VICTORY)
    fprintf(stderr, "duel attack=%d state=%d turn=%u hp=%u enemy=%u mp=%u\n",
            attack, (int)g.battle.state, (unsigned)g.battle.turn,
            (unsigned)g.hero.hp, (unsigned)g.battle.enemy_hp,
            (unsigned)g.hero.mp);
  CHECK(g.battle.state == H2_XIANXIA_RPG_BATTLE_VICTORY);
  CHECK(g.battle.enemy_hp >= g.battle.enemy_hp_max / 2u);
  CHECK(attack ? g.battle.turn < 6u : g.battle.turn == 6u);
  CHECK(g.stage == H2_XIANXIA_RPG_STAGE_C5_RESCUE);
  reload();
  CHECK(g.map == H2_XIANXIA_RPG_MAP_OLD_CITY && g.tx == 3 && g.ty == 4);
  CHECK(g.flags & H2_XIANXIA_RPG_FLAG_C5_HIGH);
}
static void rescue(void) {
  talk(3, 3);
  CHECK(!(g.flags & H2_XIANXIA_RPG_FLAG_C5_HIGH));
  CHECK(!h2_xianxia_rpg_walkable(&g, g.map, 1, 4));
  go(11, 6);
  CHECK(g.dialogue.active && g.stage == H2_XIANXIA_RPG_STAGE_C5_ESCORT);
  reload();
  CHECK(g.stage == H2_XIANXIA_RPG_STAGE_C5_ESCORT);
  const uint32_t flags = g.flags;
  step(60000);
  CHECK(g.flags == flags);
  /* Deliberate wandering exhausts the step budget, never a real timer. */
  for (int i = 0; i < 20 && g.stage == H2_XIANXIA_RPG_STAGE_C5_ESCORT; ++i) {
    go(12, 6);
    go(5, 6);
  }
  CHECK(g.stage == H2_XIANXIA_RPG_STAGE_C5_RESCUE && g.dialogue.active);
  reload();
  CHECK(g.stage == H2_XIANXIA_RPG_STAGE_C5_RESCUE &&
        (g.flags & H2_XIANXIA_RPG_FLAG_C5_HIGH));
  CHECK(!(g.flags & H2_XIANXIA_RPG_FLAG_C5_SAVED));
  talk(3, 3);
  talk(11, 6);
  talk(3, 3);
  CHECK(g.stage == H2_XIANXIA_RPG_STAGE_C5_ESCORT &&
        (g.flags & H2_XIANXIA_RPG_FLAG_C5_HIGH));
  reload();
  CHECK(h2_xianxia_rpg_walkable(&g, g.map, 1, 4));
  go(0, 4);
  CHECK(g.map == H2_XIANXIA_RPG_MAP_WATER_MARKET &&
        g.stage == H2_XIANXIA_RPG_STAGE_C5_RETURN);
  CHECK(g.flags & H2_XIANXIA_RPG_FLAG_C5_SAVED);
  reload();
  talk(7, 3);
  CHECK(g.phase == H2_XIANXIA_RPG_PHASE_ENDING &&
        (g.records.progress.cleared & 16u));
  reload();
  CHECK(g.phase == H2_XIANXIA_RPG_PHASE_ENDING);
}
static void tests(void) {
  begin(83);
  g.records.progress.cleared = 8u | (1u << 8);
  g.records.progress.chapters[3] = (h2_xianxia_rpg_clear_t){
      .level = 8, .pills = 3, .flags = H2_XIANXIA_RPG_FLAG_C4_BOOK};
  g.records.progress.chapters[8] =
      (h2_xianxia_rpg_clear_t){.level = 20, .pills = 8, .flags = 0xDEADBEEFu};
  g.records.progress.reserved[8] = 0xA7;
  uint8_t before[H2_XIANXIA_RPG_PROGRESS_BYTES],
      after[H2_XIANXIA_RPG_PROGRESS_BYTES];
  h2_xianxia_rpg_progress_encode(&g.records.progress, before);
  tower(true);
  /* Low-health defeat is an explicit fixture, not natural-play evidence. */
  g.hero.hp = 1;
  cmd(H2_XIANXIA_RPG_COMMAND_ATTACK);
  CHECK(g.battle.state == H2_XIANXIA_RPG_BATTLE_DEFEAT);
  step(600);
  tap(320, 200);
  dialog();
  CHECK(g.stage == H2_XIANXIA_RPG_STAGE_C5_DUEL);
  CHECK(g.hero.hp == g.hero.hp_max);
  talk(11, 3);
  wait_command();
  win(false);
  rescue();
  h2_xianxia_rpg_progress_encode(&g.records.progress, after);
  CHECK(memcmp(before + 4, after + 4, 4u * H2_XIANXIA_RPG_CLEAR_BYTES) == 0);
  CHECK(memcmp(before + 4 + 5u * H2_XIANXIA_RPG_CLEAR_BYTES,
               after + 4 + 5u * H2_XIANXIA_RPG_CLEAR_BYTES,
               5u * H2_XIANXIA_RPG_CLEAR_BYTES) == 0);
  h2_xianxia_rpg_progress_t decoded;
  CHECK(h2_xianxia_rpg_progress_decode(after, sizeof(after), &decoded) &&
        decoded.reserved[8] == 0xA7);
  /* A future unreadable completion table remains opaque at this ending. */
  h2_xianxia_rpg_records_t opaque = g.records;
  opaque.progress_unreadable = true;
  opaque.journey5.stage = H2_XIANXIA_RPG_STAGE_C5_RETURN;
  h2_xianxia_rpg_game_init(&g, 640, 480, 1, &opaque);
  step(300);
  box(&g.layout.title_buttons[1]);
  step(300);
  talk(7, 3);
  h2_xianxia_rpg_writes_t writes;
  CHECK(h2_xianxia_rpg_game_take_writes(&g, &writes) && writes.journey5 &&
        !writes.progress);
  for (uint32_t seed = 1; seed <= 8; ++seed) {
    begin(seed);
    tower(false);
    win(true);
  }
  /* Tide-only tiles and invalid saved landings repair to a permanent dock. */
  h2_xianxia_rpg_records_t r = g.records;
  r.journey5.stage = H2_XIANXIA_RPG_STAGE_C5_TOWER;
  r.journey5.map = H2_XIANXIA_RPG_MAP_OLD_CITY;
  r.journey5.x = 4;
  r.journey5.y = 6;
  r.journey5.flags |= H2_XIANXIA_RPG_FLAG_C5_HIGH;
  r.journey5.rest_map = r.journey5.map;
  r.journey5.rest_x = 4;
  r.journey5.rest_y = 6;
  h2_xianxia_rpg_game_init(&g, 640, 480, 1, &r);
  step(300);
  box(&g.layout.title_buttons[1]);
  CHECK(g.map == H2_XIANXIA_RPG_MAP_WATER_MARKET &&
        h2_xianxia_rpg_walkable(&g, g.map, g.tx, g.ty));
  /* A changed v0.4 journey wins Continue; its fifth-chapter checkpoint stays.
   */
  r.has_journey4 = true;
  r.journey4_latest = true;
  r.journey5_latest = false;
  r.journey4 =
      (h2_xianxia_rpg_save_t){.chapter = 4,
                              .stage = H2_XIANXIA_RPG_STAGE_C4_ESCORT,
                              .map = H2_XIANXIA_RPG_MAP_NIGHT_VILLAGE,
                              .x = 8,
                              .y = 5,
                              .rest_map = H2_XIANXIA_RPG_MAP_NIGHT_VILLAGE,
                              .rest_x = 8,
                              .rest_y = 5,
                              .level = 7,
                              .pills = 2};
  h2_xianxia_rpg_game_init(&g, 640, 480, 1, &r);
  CHECK(h2_xianxia_rpg_continue_save(&g)->chapter == 4u);
  CHECK(h2_xianxia_rpg_unfinished(&g, 5) != NULL);
  /* The same optional water foe is exactly 20 percent stronger at high tide. */
  begin(7);
  go(14, 4);
  reload();
  box(&g.layout.escort_actions[0]);
  wait_command();
  CHECK(g.battle.enemy == H2_XIANXIA_RPG_ENEMY_TIDE_OTTER &&
        g.battle.enemy_attack == 34u);
  reload();
  go(14, 4);
  talk(3, 3);
  box(&g.layout.escort_actions[0]);
  wait_command();
  CHECK(g.battle.enemy_attack == 40u);
  /* Fifth-chapter dialogue pages retain the full text on the smallest screen.
   */
  h2_xianxia_rpg_game_init(&g, 320, 240, 1, NULL);
  step(300);
  box(&g.layout.title_buttons[2]);
  step(300);
  box(&g.layout.chapter_rows[4]);
  step(1000);
  tap(160, 120);
  dialog();
  go(7, 3);
  while (g.dialogue.index < 3u) {
    step(3000);
    tap(160, 120);
  }
  CHECK(g.dialogue.active &&
        g.dialogue.total <
            h2_xianxia_rpg_utf8_length(g.dialogue.script->lines[3].text));
  step(3000);
  tap(160, 120);
  CHECK(g.dialogue.index == 3u && g.dialogue.page_offset > 0u);
  CHECK(g.layout.chapter_back.y + g.layout.chapter_back.h <= 240);
}
int main(void) {
  tests();
  puts("xianxia_rpg chapter5 tests passed");
  return 0;
}
