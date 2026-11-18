/* Chapter 4 regression paths through the production pointer/model boundary. */
#include "h2_xianxia_rpg_game.h"
#include "h2_xianxia_rpg_story.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c)                                              \
  do {                                                        \
    if (!(c)) {                                               \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); \
      exit(1);                                                \
    }                                                         \
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
  for (int i = 0; i < 160 && g.dialogue.active; ++i) {
    step(80);
    tap(320, 200);
  }
  CHECK(!g.dialogue.active);
}
static void go(int x, int y) {
  tap((float)g.layout.map_x + ((float)x + 0.5f) * (float)g.layout.tile,
      (float)g.layout.map_y + ((float)y + 0.5f) * (float)g.layout.tile);
  for (int i = 0; i < 800 && !g.dialogue.active &&
                  (g.moving || g.path_pos < g.path_len || g.target_x >= 0);
       ++i)
    step(20);
}
static void talk(int x, int y) {
  go(x, y);
  if (!g.dialogue.active)
    fprintf(stderr, "talk %d,%d map=%d stage=%d hero=%d,%d\n", x, y, (int)g.map,
            (int)g.stage, g.tx, g.ty);
  CHECK(g.dialogue.active);
  dialog();
}
static void reload(void) {
  h2_xianxia_rpg_writes_t w;
  (void)h2_xianxia_rpg_game_take_writes(&g, &w);
  const h2_xianxia_rpg_records_t records = g.records;
  h2_xianxia_rpg_game_init(&g, 640, 480, 83, &records);
  step(300);
  box(&g.layout.title_buttons[1]);
  step(300);
}
static void command(h2_xianxia_rpg_command_t c) {
  CHECK(g.battle.state == H2_XIANXIA_RPG_BATTLE_COMMAND);
  box(&g.layout.commands[c]);
}
static void wait_command(void) {
  for (int i = 0; i < 300 && g.battle.state != H2_XIANXIA_RPG_BATTLE_COMMAND &&
                  g.battle.state != H2_XIANXIA_RPG_BATTLE_VICTORY &&
                  g.battle.state != H2_XIANXIA_RPG_BATTLE_DEFEAT;
       ++i)
    step(50);
}
static void retry(void) {
  CHECK(g.battle.state == H2_XIANXIA_RPG_BATTLE_DEFEAT);
  step(600);
  tap(320, 200);
  dialog();
  CHECK(g.map == g.rest_map && g.tx == g.rest_x && g.ty == g.rest_y);
  CHECK(g.hero.hp == g.hero.hp_max && g.ally_hp == g.ally_hp_max);
  CHECK((g.flags & H2_XIANXIA_RPG_C4_LAMPS) == 7u);
  h2_xianxia_rpg_writes_t w;
  CHECK(!h2_xianxia_rpg_game_take_writes(&g, &w));
  talk(7, 3);
  wait_command();
  CHECK(g.battle.wave == 1u && g.battle.wave_turn == 0u &&
        g.battle.lamp_hp == 80u && g.battle.enemy_hp == 150u);
}
static h2_xianxia_rpg_records_t escort(void) {
  h2_xianxia_rpg_game_init(&g, 640, 480, 83, NULL);
  step(300);
  box(&g.layout.title_buttons[2]);
  step(300);
  box(&g.layout.chapter_rows[3]);
  CHECK(g.phase == H2_XIANXIA_RPG_PHASE_PROLOGUE && !g.prologue_carry);
  step(1000);
  tap(320, 200);
  dialog();
  CHECK(g.chapter == 4u && g.hero.level == 7u && g.hero.pills == 3u);
  talk(4, 3);
  CHECK(g.flags & H2_XIANXIA_RPG_FLAG_C4_JOINED);
  go(8, 4);
  CHECK(g.dialogue.active && (g.flags & H2_XIANXIA_RPG_FLAG_C4_FIRE));
  h2_xianxia_rpg_writes_t fire;
  CHECK(h2_xianxia_rpg_game_take_writes(&g, &fire) && fire.journey4);
  CHECK(fire.journey4_save.flags & H2_XIANXIA_RPG_FLAG_C4_FIRE);
  dialog();
  CHECK(!h2_xianxia_rpg_game_take_writes(&g, &fire));
  CHECK(g.stage == H2_XIANXIA_RPG_STAGE_C4_ESCORT);
  const int rx = g.rest_x, ry = g.rest_y;
  go(14, 4);
  CHECK(g.map == H2_XIANXIA_RPG_MAP_EAST_RIDGE);
  /* Waiting does not advance wind, consume a fire or change a step. */
  const uint32_t walked = g.walk_steps, flags = g.flags;
  step(60000);
  CHECK(g.walk_steps == walked && g.flags == flags);
  go(11, 3);
  CHECK(g.dialogue.active && g.map == H2_XIANXIA_RPG_MAP_NIGHT_VILLAGE);
  CHECK(g.tx == rx && g.ty == ry && h2_xianxia_rpg_c4_lamps(g.flags) == 0u);
  dialog();
  box(&g.layout.escort_actions[0]);
  dialog();
  CHECK(g.flags & H2_XIANXIA_RPG_FLAG_C4_COVER);
  go(14, 4);
  go(11, 3);
  CHECK(g.tx == 6 && g.dialogue.active);
  dialog();
  go(11, 3);
  CHECK(g.dialogue.active && h2_xianxia_rpg_c4_lamps(g.flags) == 1u);
  CHECK(g.rest_map == H2_XIANXIA_RPG_MAP_EAST_RIDGE);
  /* Reload during the news must already find the lit lamp checkpoint. */
  reload();
  CHECK(h2_xianxia_rpg_c4_lamps(g.flags) == 1u &&
        (g.flags & H2_XIANXIA_RPG_FLAG_C4_COVER));
  box(&g.layout.escort_actions[0]);
  dialog();
  go(14, 4);
  CHECK(g.map == H2_XIANXIA_RPG_MAP_WEST_RIDGE);
  go(11, 3);
  CHECK(g.map == H2_XIANXIA_RPG_MAP_EAST_RIDGE && g.dialogue.active);
  dialog();
  CHECK(h2_xianxia_rpg_c4_lamps(g.flags) == 1u);
  box(&g.layout.escort_actions[0]);
  dialog();
  go(14, 4);
  go(11, 3);
  dialog();
  talk(11, 3);
  CHECK(h2_xianxia_rpg_c4_lamps(g.flags) == 2u);
  go(14, 4);
  CHECK(g.map == H2_XIANXIA_RPG_MAP_LANTERN_ALTAR);
  go(7, 3);
  dialog();
  talk(7, 3);
  CHECK(g.stage == H2_XIANXIA_RPG_STAGE_C4_GUARD &&
        g.rest_map == H2_XIANXIA_RPG_MAP_LANTERN_ALTAR);
  reload();
  CHECK(g.stage == H2_XIANXIA_RPG_STAGE_C4_GUARD);
  return g.records;
}
static bool damage_popup(uint32_t value, uint8_t target) {
  for (size_t i = 0; i < H2_XIANXIA_RPG_MAX_POPUPS; ++i) {
    const h2_xianxia_rpg_popup_t* p = &g.battle.popups[i];
    if (p->active && !p->on_enemy && !p->heal && p->value == value &&
        p->target == target)
      return true;
  }
  return false;
}

static void battle_and_ending(const h2_xianxia_rpg_records_t* checkpoint) {
  talk(7, 3);
  wait_command();
  CHECK(g.battle.lamp_battle &&
        h2_xianxia_rpg_command_available(&g, H2_XIANXIA_RPG_COMMAND_FLEE));
  /* Ordinary guarding protects the party, not the objective: ignoring the
   * lamp loses despite a living hero, then resets all three waves. */
  for (int i = 0; i < 12 && g.battle.state != H2_XIANXIA_RPG_BATTLE_DEFEAT;
       ++i) {
    command(H2_XIANXIA_RPG_COMMAND_GUARD);
    wait_command();
  }
  CHECK(g.battle.lamp_hp == 0u && g.hero.hp > 0u);
  retry();
  /* Fixture: critically wounded hero. The actual guard action and incoming
   * lamp damage must use the defeat path even when lamp health remains. */
  g.hero.hp = 1u;
  command(H2_XIANXIA_RPG_COMMAND_FLEE);
  wait_command();
  CHECK(g.battle.lamp_hp == 80u && g.hero.hp == 0u);
  retry();
  /* Chosen ally action does not spend a turn. Repair has an observable
   * amount; pill targeting consumes neither item nor turn before selection. */
  command(H2_XIANXIA_RPG_COMMAND_ATTACK);
  wait_command();
  CHECK(g.battle.lamp_hp == 60u && h2_xianxia_rpg_lamp_target(&g) == 1u);
  CHECK(damage_popup(20u, 2u));
  const uint32_t turns = g.turns_taken;
  box(&g.layout.party[1]);
  CHECK(g.battle.ally_mend && g.turns_taken == turns);
  g.ally_hp = 1u;
  command(H2_XIANXIA_RPG_COMMAND_ATTACK);
  wait_command();
  CHECK(g.battle.lamp_hp == 72u && g.ally_hp == 0u);
  CHECK(damage_popup(38u, 1u));
  const uint32_t pills = g.hero.pills, before = g.turns_taken;
  command(H2_XIANXIA_RPG_COMMAND_PILL);
  CHECK(g.battle.state == H2_XIANXIA_RPG_BATTLE_HEAL_TARGET &&
        g.hero.pills == pills && g.turns_taken == before);
  box(&g.layout.party[1]);
  CHECK(g.ally_hp > 0u && g.hero.pills == pills - 1u);
  wait_command();
  CHECK(g.battle.wave == 2u && g.battle.lamp_hp == 40u);
  /* Hero-targeted medicine is separate from the companion target. */
  g.hero.hp = 40u;
  const uint32_t ally = g.ally_hp;
  command(H2_XIANXIA_RPG_COMMAND_PILL);
  box(&g.layout.party[0]);
  CHECK(g.hero.hp > 40u && g.ally_hp == ally);
  wait_command();
  CHECK(g.battle.lamp_hp == 32u);
  /* Reopen saved checkpoint to test a resource-independent winning route. */
  for (uint32_t seed = 1; seed <= 8; ++seed) {
    h2_xianxia_rpg_game_init(&g, 640, 480, seed, checkpoint);
    step(300);
    box(&g.layout.title_buttons[1]);
    step(300);
    g.hero.pills = 0;
    talk(7, 3);
    wait_command();
    for (int i = 0; i < 12 && g.battle.state != H2_XIANXIA_RPG_BATTLE_VICTORY;
         ++i) {
      CHECK(g.battle.state != H2_XIANXIA_RPG_BATTLE_DEFEAT);
      command(h2_xianxia_rpg_lamp_target(&g) == 2u
                  ? H2_XIANXIA_RPG_COMMAND_FLEE
                  : H2_XIANXIA_RPG_COMMAND_GUARD);
      wait_command();
    }
    CHECK(g.battle.state == H2_XIANXIA_RPG_BATTLE_VICTORY &&
          g.battle.lamp_hp == 80u);
    CHECK(g.stage == H2_XIANXIA_RPG_STAGE_C4_RETURN && g.battles_won == 1u &&
          g.turns_taken == 9u);
  }
  reload();
  CHECK(g.stage == H2_XIANXIA_RPG_STAGE_C4_RETURN && g.battles_won == 1u);
  talk(7, 3);
  CHECK(g.map == H2_XIANXIA_RPG_MAP_NIGHT_VILLAGE &&
        (g.flags & H2_XIANXIA_RPG_FLAG_C4_FAINT));
  talk(4, 3);
  CHECK(g.phase == H2_XIANXIA_RPG_PHASE_ENDING);
  CHECK(g.flags & H2_XIANXIA_RPG_FLAG_C4_BOOK);
  CHECK(g.flags & H2_XIANXIA_RPG_FLAG_C4_GLYPH);
  h2_xianxia_rpg_writes_t w;
  CHECK(h2_xianxia_rpg_game_take_writes(&g, &w));
  CHECK(w.journey4 && w.progress && !w.journey3 && !w.journey && !w.chapter1 &&
        w.progress_record.cleared == 8u);
  reload();
  CHECK(g.phase == H2_XIANXIA_RPG_PHASE_ENDING && g.turns_taken == 9u);
  CHECK(!h2_xianxia_rpg_game_take_writes(&g, &w));
  step(1600);
  tap(320, 200);
  CHECK(g.phase == H2_XIANXIA_RPG_PHASE_TITLE);
}
static void early_wave_kill(const h2_xianxia_rpg_records_t* checkpoint) {
  h2_xianxia_rpg_game_init(&g, 640, 480, 91, checkpoint);
  step(300);
  box(&g.layout.title_buttons[1]);
  step(300);
  talk(7, 3);
  wait_command();
  /* A strong carried hero can disperse waves early, but cannot skip the
   * remaining waves or receive three chapter clear rewards. */
  g.hero.attack = 300u;
  for (uint32_t wave = 1; wave <= 3; ++wave) {
    CHECK(g.battle.wave == wave);
    command(H2_XIANXIA_RPG_COMMAND_ATTACK);
    wait_command();
  }
  CHECK(g.battle.state == H2_XIANXIA_RPG_BATTLE_VICTORY &&
        g.battles_won == 1u && g.turns_taken == 3u);
}
static void fallen_companion_can_finish(
    const h2_xianxia_rpg_records_t* checkpoint) {
  h2_xianxia_rpg_game_init(&g, 640, 480, 37, checkpoint);
  step(300);
  box(&g.layout.title_buttons[1]);
  step(300);
  talk(7, 3);
  wait_command();
  bool fell = false;
  for (int i = 0; i < 12 && g.battle.state != H2_XIANXIA_RPG_BATTLE_VICTORY;
       ++i) {
    CHECK(g.battle.state != H2_XIANXIA_RPG_BATTLE_DEFEAT);
    command(h2_xianxia_rpg_lamp_target(&g) == 2u
                ? H2_XIANXIA_RPG_COMMAND_FLEE
                : H2_XIANXIA_RPG_COMMAND_ATTACK);
    wait_command();
    if (g.ally_hp == 0u) {
      fell = true;
      CHECK(!h2_xianxia_rpg_joint_ready(&g));
    }
  }
  CHECK(fell && g.battle.state == H2_XIANXIA_RPG_BATTLE_VICTORY &&
        g.ally_hp == 1u);
}

static void carry_and_latest(void) {
  h2_xianxia_rpg_records_t r = {0};
  r.has_journey3 = r.journey3_latest = true;
  r.journey3 = (h2_xianxia_rpg_save_t){
      .chapter = 3,
      .stage = H2_XIANXIA_RPG_STAGE_C3_CLEARED,
      .level = 8,
      .exp = 444,
      .pills = 2,
      .flags = H2_XIANXIA_RPG_FLAG_C3_GLYPH | H2_XIANXIA_RPG_FLAG_C2_RUMOR,
      .trust = 2,
      .map = H2_XIANXIA_RPG_MAP_SWORD_HALL,
      .x = 3,
      .y = 3,
      .rest_map = H2_XIANXIA_RPG_MAP_SWORD_HALL,
      .rest_x = 3,
      .rest_y = 7};
  r.progress.cleared = 4u;
  r.progress.chapters[2] = (h2_xianxia_rpg_clear_t){.level = 8,
                                                    .exp = 444,
                                                    .pills = 2,
                                                    .flags = r.journey3.flags,
                                                    .trust = 2};
  h2_xianxia_rpg_game_init(&g, 640, 480, 3, &r);
  step(300);
  box(&g.layout.title_buttons[1]);
  step(1600);
  tap(320, 200);
  CHECK(g.prologue_chapter == 4u && g.prologue_carry);
  step(1000);
  tap(320, 200);
  dialog();
  CHECK(g.hero.level == 8u && g.hero.exp == 444u && g.hero.pills == 2u &&
        g.trust == 2u && g.flags == r.journey3.flags);
  h2_xianxia_rpg_writes_t w;
  CHECK(h2_xianxia_rpg_game_take_writes(&g, &w) && w.journey4 && !w.journey3 &&
        !w.progress);
  CHECK(memcmp(&r.journey3, &g.records.journey3, sizeof(r.journey3)) == 0);
  r = g.records;
  r.journey4_latest = false;
  h2_xianxia_rpg_game_init(&g, 640, 480, 3, &r);
  CHECK(h2_xianxia_rpg_continue_save(&g)->chapter == 3u &&
        h2_xianxia_rpg_unfinished(&g, 4u));
  r.journey4_latest = true;
  h2_xianxia_rpg_game_init(&g, 640, 480, 3, &r);
  CHECK(h2_xianxia_rpg_continue_save(&g)->chapter == 4u);
  uint8_t bytes[H2_XIANXIA_RPG_JOURNEY_BYTES];
  h2_xianxia_rpg_save_t decoded;
  h2_xianxia_rpg_journey_encode(&r.journey4, 123u, bytes);
  CHECK(h2_xianxia_rpg_journey_decode(bytes, sizeof(bytes), &decoded, NULL));
  CHECK(decoded.chapter == 4u && decoded.level == 8u);
  /* Startup never replaces an existing earlier completion with an older
   * version's different chapter-1 snapshot. */
  r.has_chapter1 = true;
  r.chapter1 = (h2_xianxia_rpg_save_t){.chapter = 1,
                                       .stage = 4,
                                       .level = 4,
                                       .pills = 1,
                                       .map = 0,
                                       .x = 7,
                                       .y = 5};
  r.progress.cleared |= 1u;
  r.progress.chapters[0] =
      (h2_xianxia_rpg_clear_t){.level = 5, .pills = 3, .turns = 40};
  h2_xianxia_rpg_game_init(&g, 640, 480, 3, &r);
  CHECK(g.records.progress.chapters[0].level == 5u &&
        !h2_xianxia_rpg_game_take_writes(&g, &w));
}
int main(void) {
  const h2_xianxia_rpg_records_t checkpoint = escort();
  battle_and_ending(&checkpoint);
  early_wave_kill(&checkpoint);
  fallen_companion_can_finish(&checkpoint);
  carry_and_latest();
  puts("chapter 4 escort, battle, checkpoints and carry passed");
  return 0;
}
