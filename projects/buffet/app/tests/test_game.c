#include "buffet_draw.h"
#include "buffet_game.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); \
      exit(1);                                                \
    }                                                         \
  } while (0)
static bf_game_t paid(void) {
  bf_game_t g;
  bf_init(&g, false);
  bf_tap(&g, 610, 383);
  CHECK(g.phase == BF_DINING);
  CHECK(g.purse == bf_rules.purse - bf_rules.ticket);
  return g;
}
static void clock_and_stomach(void) {
  bf_game_t g = paid();
  g.fullness_milli = bf_rules.capacity * 1000 + 1;
  bf_step(&g, 1, 0, 0);
  CHECK(g.red_ms == 0);
  g.fullness_milli = bf_rules.red_line * 1000;
  bf_step(&g, 100, 0, 0);
  CHECK(g.red_ms == 0);
  g.fullness_milli = bf_rules.red_line * 1000 + 2000;
  bf_step(&g, 999, 0, 0);
  CHECK(g.red_ms == 999);
  bf_step(&g, 1, 0, 0);
  CHECK(g.red_ms == 0 && g.fullness_milli == bf_rules.red_line * 1000);
  g.fullness_milli += 50000;
  bf_step(&g, bf_rules.tolerance_ms - 1, 0, 0);
  CHECK(g.phase == BF_DINING && g.red_ms == bf_rules.tolerance_ms - 1);
  int now = g.elapsed_ms;
  bf_step(&g, 400, 0, 0);
  CHECK(g.phase == BF_HOSPITAL && g.elapsed_ms == now + 1);
  CHECK(g.purse == bf_rules.purse - bf_rules.ticket - bf_rules.fee);
  bf_step(&g, 5000, 0, 0);
  CHECK(g.elapsed_ms == now + 1);
  g = paid();
  g.fullness_milli = bf_rules.red_line * 1000 +
                     bf_rules.tolerance_ms * bf_rules.digest_milli_per_ms;
  bf_step(&g, bf_rules.tolerance_ms, 0, 0);
  CHECK(g.phase == BF_DINING && g.red_ms == 0);
  g = paid();
  g.elapsed_ms = bf_rules.duration_ms - bf_rules.tolerance_ms;
  g.fullness_milli = 200000;
  bf_step(&g, 20000, 0, 0);
  CHECK(g.phase == BF_CLOSING && g.elapsed_ms == bf_rules.duration_ms &&
        g.fullness_milli > bf_rules.red_line * 1000);
  CHECK(g.purse == bf_rules.purse - bf_rules.ticket);
  bf_step(&g, 999999, 0, 0);
  CHECK(g.phase == BF_CLOSING);
  g = paid();
  g.elapsed_ms = bf_rules.duration_ms - bf_rules.tolerance_ms - 1;
  g.fullness_milli = 200000;
  bf_step(&g, 20000, 0, 0);
  CHECK(g.phase == BF_HOSPITAL && g.elapsed_ms == bf_rules.duration_ms - 1);
}
static void play_and_route(void) {
  bf_game_t g = paid();
  bf_point_t spawn = g.player.pos;
  int slot = 0;
  bf_place_t p = bf_place(&g, slot);
  CHECK(bf_walkable(p.station));
  g.player.pos = (bf_point_t){109, 349};
  bf_action(&g, H2_BUFFET_CONFIRM);
  CHECK(!g.eaten);
  bf_tap(&g, p.pos.x, p.pos.y - 6);
  CHECK(g.eaten == 0 && g.route_len > 0);
  g.player.pos = p.station;
  int dish = bf_serving(&g, slot);
  bf_tap(&g, p.pos.x, p.pos.y - 6);
  CHECK(g.eaten == 1 && g.empty[slot] && g.varieties == 1);
  CHECK(g.value == bf_menu[dish].coins &&
        g.fullness_milli == bf_menu[dish].fullness * 1000);
  bf_action(&g, H2_BUFFET_CONFIRM);
  CHECK(g.eaten == 1);
  bf_step(&g, bf_rules.lap_ms, 0, 0);
  CHECK(!g.empty[slot] && g.returned == 1 && g.fullness_milli == 0);
  g.player.pos = spawn;
  bf_tap(&g, 402, 159);
  CHECK(g.route_len >= 2);
  for (int i = 0; i < 650 && g.route_at < g.route_len; i++) {
    bf_step(&g, 33, 0, 0);
    CHECK(bf_walkable(g.player.pos));
  }
  CHECK(hypotf(g.player.pos.x - 402, g.player.pos.y - 159) < 1);
  bf_point_t before = g.player.pos;
  bf_step(&g, 60, 1, 0);
  CHECK(g.player.pos.x > before.x + 5 && g.player.face == BF_RIGHT);
  /* A guest occupies the aisle momentarily and the player can move out
   * again. Advancing time also opens the original route. */
  bf_step(&g, 1, 0, 0);
  bf_point_t npc = g.diners[0].pos;
  g.player.pos = (bf_point_t){npc.x - 24, npc.y};
  bf_step(&g, 3, 1, 0);
  CHECK(g.obstructed);
  bf_step(&g, 40, -1, 0);
  CHECK(g.player.walking);
  bf_step(&g, bf_rules.duration_ms, 0, 0);
  CHECK(g.phase == BF_CLOSING);
  bf_action(&g, H2_BUFFET_CONFIRM);
  CHECK(g.phase == BF_CLOSING);
  bf_step(&g, 700, 0, 0);
  bf_action(&g, H2_BUFFET_CONFIRM);
  CHECK(g.phase == BF_TICKET && g.purse == bf_rules.purse);
  bf_action(&g, H2_BUFFET_LANGUAGE | H2_BUFFET_GOAL);
  bf_action(&g, H2_BUFFET_CONFIRM);
  CHECK(g.tasting && g.en && g.elapsed_ms == 0 && g.value == 0 &&
        g.varieties == 0 && g.purse == bf_rules.purse - bf_rules.ticket);
}
static void fonts_and_servings(void) {
  bf_game_t g = paid();
  for (int i = 0; i < BF_DISHES; i++) {
    CHECK(bf_menu[i].id && bf_menu[i].coins > 0 && bf_menu[i].fullness > 0);
    for (int j = 0; j < i; j++)
      CHECK(strcmp(bf_menu[i].id, bf_menu[j].id) != 0);
    for (int size = 0; size < 4; size++) {
      CHECK(bf_font_covers(false, size, bf_menu[i].name.zh));
      CHECK(bf_font_covers(false, size, bf_menu[i].detail.zh));
      CHECK(bf_font_covers(true, size, bf_menu[i].name.en));
      CHECK(bf_font_covers(true, size, "中文"));
    }
    CHECK(bf_width(true, BF_HEAD, bf_menu[i].name.en) <= 430);
  }
  unsigned seen = 0;
  for (int ms = 0; ms < 2 * bf_rules.lap_ms; ms += 47) {
    g.elapsed_ms = ms;
    for (int i = 0; i < BF_SLOTS; i++) {
      int d = bf_serving(&g, i);
      CHECK(d >= 0 && d < BF_DISHES);
      seen |= 1u << d;
      CHECK(bf_walkable(bf_place(&g, i).station));
    }
  }
  CHECK(seen == (1u << BF_DISHES) - 1);
}
int main(void) {
  clock_and_stomach();
  play_and_route();
  fonts_and_servings();
  puts("buffet rules, routes, recycling, replay, menu and fonts passed");
  return 0;
}
