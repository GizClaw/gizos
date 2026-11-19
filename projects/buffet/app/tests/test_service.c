#include "buffet_game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr)                                              \
  do {                                                           \
    if (!(expr)) {                                               \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
      exit(1);                                                   \
    }                                                            \
  } while (0)

static int stock(const bf_game_t* g, int table) {
  int n = 0;
  for (int i = 0; i < BF_SETTINGS; i++)
    n += g->settings[BF_SETTINGS * table + i].dish >= 0;
  return n;
}

int main(void) {
  bf_game_t g;
  bf_init(&g, false);
  bf_action(&g, H2_BUFFET_CONFIRM);
  CHECK(stock(&g, 0) == 1 && stock(&g, 1) == 1 && stock(&g, 2) == 0);
  int order[BF_TABLES];
  CHECK(bf_next_tables(&g, order) == 3 && order[0] == 2 && order[1] == 0 &&
        order[2] == 1);
  int sun_slot = bf_next_setting(&g, 2), next_dish = bf_next_dish(&g, 2);
  bf_step(&g, 2999, 0, 0);
  CHECK(g.settings[sun_slot].dish < 0 &&
        g.service[2].next_at_ms - g.elapsed_ms == 1);
  bf_step(&g, 1, 0, 0);
  CHECK(g.settings[sun_slot].dish == next_dish &&
        g.settings[sun_slot].arrived_ms == 3000);
  CHECK(bf_next_tables(&g, order) == 3 && order[0] == 0);
  bf_step(&g, 30000, 0, 0);
  for (int t = 0; t < BF_TABLES; t++)
    CHECK(stock(&g, t) == BF_SETTINGS && g.service[t].next_at_ms < 0);
  CHECK(bf_next_tables(&g, order) == 0);
  bf_setting_t occupied[BF_SLOTS];
  memcpy(occupied, g.settings, sizeof(occupied));
  bf_step(&g, 30000, 0, 0);
  CHECK(memcmp(occupied, g.settings, sizeof(occupied)) == 0);
  g.player.pos = bf_place(&g, sun_slot).station;
  bf_place_t place = bf_place(&g, sun_slot);
  bf_tap(&g, place.pos.x, place.pos.y - 4);
  CHECK(g.eaten == 1 && g.settings[sun_slot].dish < 0 && g.returned == 1);
  CHECK(bf_next_tables(&g, order) == 1 && order[0] == 2);
  CHECK(g.service[2].next_at_ms == g.elapsed_ms + bf_tables[2].period_ms);
  next_dish = bf_next_dish(&g, 2);
  bf_step(&g, bf_tables[2].period_ms - 1, 0, 0);
  CHECK(g.settings[sun_slot].dish < 0);
  bf_step(&g, 1, 0, 0);
  CHECK(g.settings[sun_slot].dish == next_dish &&
        bf_next_tables(&g, order) == 0);
  for (int i = 0; i < BF_SLOTS; i++)
    if (i != sun_slot)
      CHECK(g.settings[i].arrived_ms == occupied[i].arrived_ms);
  g.elapsed_ms = bf_rules.duration_ms - 100;
  bf_service_clear(&g, sun_slot);
  CHECK(!bf_next_tables(&g, order));
  bf_step(&g, 100, 0, 0);
  CHECK(g.phase == BF_CLOSING && g.settings[sun_slot].dish < 0);
  puts(
      "staggered deadlines, table cues, full-table holds, single-slot refill and closing passed");
  return 0;
}
