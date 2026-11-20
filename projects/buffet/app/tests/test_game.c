#include "buffet_draw.h"
#include "buffet_rotary.h"
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
  CHECK(bf_nearest(&g) < 0);
  bf_place_t p = bf_place(&g, 4);
  bf_tap(&g, p.pos.x, p.pos.y);
  CHECK(g.eaten == 0 && g.route_len > 0);
  for (int i = 0; i < 220 && (g.route_at < g.route_len || g.follow_slot >= 0);
       i++) {
    bf_step(&g, 33, 0, 0);
    CHECK(bf_walkable(g.player.pos));
  }
  CHECK(bf_can_take(&g, 4) && g.eaten == 0);
  p = bf_place(&g, 4);
  bf_tap(&g, p.pos.x, p.pos.y);
  CHECK(g.eaten == 1 && g.returned == 1 && g.settings[4].dish < 0);
  CHECK(g.value == bf_menu[0].coins &&
        g.fullness_milli == bf_menu[0].fullness * 1000);
  bf_action(&g, H2_BUFFET_CONFIRM);
  CHECK(g.eaten == 1);
  bf_step(&g, 1000, 0, 0);
  g.player.pos = bf_project((bf_world_t){6, 10.35f}, 0);
  for (int i = 0; i < 24; i++) bf_step(&g, 33, 0, -1);
  CHECK(bf_unproject(g.player.pos).v >= 10.22f && bf_walkable(g.player.pos));
  /* Tap the visible floor just above the raised rear lip. */
  bf_point_t rear = bf_project((bf_world_t){6, 3.10f}, 0);
  bf_tap(&g, rear.x, rear.y);
  CHECK(g.route_len >= 2);
  for (int i = 0; i < 220 && g.route_at < g.route_len; i++) {
    bf_step(&g, 33, 0, 0);
    CHECK(bf_walkable(g.player.pos));
  }
  CHECK(hypotf(g.player.pos.x - rear.x, g.player.pos.y - rear.y) < 1);
  bf_point_t before = g.player.pos;
  bf_step(&g, 60, 1, 0);
  CHECK(g.player.pos.x > before.x && g.player.face == BF_RIGHT);
  bf_step(&g, bf_rules.duration_ms, 0, 0);
  CHECK(g.phase == BF_CLOSING);
  bf_step(&g, 700, 0, 0);
  bf_action(&g, H2_BUFFET_CONFIRM);
  CHECK(g.phase == BF_TICKET && g.purse == bf_rules.purse);
  bf_action(&g, H2_BUFFET_LANGUAGE | H2_BUFFET_GOAL);
  bf_action(&g, H2_BUFFET_CONFIRM);
  CHECK(g.en && g.tasting && g.eaten == 0 && g.elapsed_ms == 0 &&
        g.purse == bf_rules.purse - bf_rules.ticket);
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
  for (int t = 0; t < BF_TABLES; t++) {
    CHECK(bf_font_covers(false, BF_SMALL, bf_tables[t].name.zh));
    for (int i = 0; i < 4; i++) {
      g.service[t].course = i;
      int d = bf_next_dish(&g, t);
      CHECK(d >= 0 && d < BF_DISHES);
      seen |= 1u << d;
    }
  }
  for (int i = 0; i < BF_SLOTS; i++)
    CHECK(bf_walkable(bf_place(&g, i).station));
  CHECK(seen == (1u << BF_DISHES) - 1);
}
int main(void) {
  clock_and_stomach();
  play_and_route();
  fonts_and_servings();
  puts("buffet rules, routes, recycling, replay, menu and fonts passed");
  return 0;
}
