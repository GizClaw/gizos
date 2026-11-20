#include "buffet_rotary.h"
#include <math.h>
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
  for (int i = 0; i < bf_tables[table].slots; i++)
    n += g->settings[bf_tables[table].first_slot + i].dish >= 0;
  return n;
}
static void single_file(bf_game_t g) {
  /* Sample every bend and both directions over a full lap. Distances between
   * rendered serving centers leave clearance for the 35x24 visible plate. */
  for (int t = 0; t < BF_LOOPS; t++) {
    for (int at = 0; at < bf_laps[t]; at += 120) {
      g.elapsed_ms = at;
      for (int i = 0; i < BF_SETTINGS; i++) {
        bf_place_t p = bf_place(&g, t * BF_SETTINGS + i),
                   q = bf_place(&g, t * BF_SETTINGS + (i + 1) % BF_SETTINGS);
        CHECK(bf_walkable(p.station));
        CHECK(hypotf((p.pos.x - q.pos.x) / 35, (p.pos.y - q.pos.y) / 24) >
              1.50f);
      }
    }
    g.elapsed_ms = 0;
    bf_place_t a = bf_place(&g, t * BF_SETTINGS);
    g.elapsed_ms = bf_laps[t];
    bf_place_t b = bf_place(&g, t * BF_SETTINGS);
    CHECK(hypotf(b.pos.x - a.pos.x, b.pos.y - a.pos.y) < .001f);
  }
  for (int i = 0; i < BF_PASS_SLOTS; i++) {
    bf_place_t p = bf_place(&g, BF_LOOPS * BF_SETTINGS + i);
    bf_world_t center = bf_unproject((bf_point_t){p.pos.x, p.pos.y + 26});
    CHECK(fabsf(center.v - .85f) < .001f &&
          fabsf(center.u - 8.5f - i * 1.5f) < .001f);
    CHECK(bf_walkable(p.station));
  }
}
static void access(bf_game_t g) {
  /* The landmarks remain accessible while the island and stair lip block
   * direct movement. Test a full pass through the central stair opening. */
  const bf_world_t stops[] = {
      {10, 2.03f}, {2.15f, 7}, {17.82f, 7}, {10, 6}, {10, 11}};
  for (unsigned i = 0; i < sizeof(stops) / sizeof(stops[0]); i++)
    CHECK(bf_walkable(bf_project(stops[i], 0)));
  g.player.pos = bf_project((bf_world_t){10, 3.6f}, 0);
  for (int i = 0; i < 12 && bf_unproject(g.player.pos).v > 2.15f; i++) {
    bf_step(&g, 33, 0, -1);
    CHECK(bf_walkable(g.player.pos));
  }
  CHECK(bf_unproject(g.player.pos).v <= 2.15f);
  CHECK(!bf_walkable(bf_project((bf_world_t){6, 2.55f}, 0)));
  CHECK(bf_walkable(bf_project((bf_world_t){10, 2.55f}, 0)));
}
static void expiry_queue(void) {
  bf_game_t g;
  bf_init(&g, false);
  bf_action(&g, H2_BUFFET_CONFIRM);
  int ramen = BF_FIRST_HOT + 2, salmon = 4, order[BF_FRESH_QUEUE];
  int tuna = bf_next_setting(&g, 0);
  CHECK(g.settings[ramen].arrived_ms == 0 &&
        g.settings[ramen].expires_ms == 26000);
  CHECK(g.settings[salmon].arrived_ms == 0 &&
        g.settings[salmon].expires_ms == 28000);
  CHECK(g.settings[ramen].portion_id &&
        g.settings[ramen].portion_id != g.settings[salmon].portion_id);
  bf_step(&g, 17999, 0, 0);
  CHECK(!bf_expiring(&g, order));
  bf_step(&g, 1, 0, 0);
  CHECK(bf_expiring(&g, order) == 1 && order[0] == ramen);
  bf_step(&g, 3100, 0, 0);
  CHECK(bf_expiring(&g, order) == 3 && order[0] == ramen &&
        order[1] == salmon && order[2] == tuna);
  /* The second bowl of the same recipe has its own deadline and identity. */
  int later = BF_FIRST_HOT + 1;
  CHECK(g.settings[later].dish == g.settings[ramen].dish &&
        g.settings[later].arrived_ms == 6200);
  CHECK(g.settings[later].expires_ms == 32200 &&
        g.settings[later].portion_id != g.settings[ramen].portion_id);
  bf_step(&g, 4899, 0, 0);
  CHECK(bf_fresh_left(&g, ramen) == 1);
  int due = g.service[BF_HOT].next_at_ms;
  bf_game_t eaten = g;
  eaten.player.pos = bf_place(&eaten, ramen).station;
  bf_action(&eaten, H2_BUFFET_CONFIRM);
  CHECK(eaten.eaten == 1 && eaten.value == 24 &&
        eaten.fullness_milli == 32000 && eaten.bite_ms == 680);
  bf_step(&eaten, 1, 0, 0);
  CHECK(eaten.eaten == 1 && eaten.value == 24 && eaten.bite_ms == 679 &&
        eaten.returned == 1);
  bf_step(&g, 1, 0, 0);
  g.player.pos = bf_place(&g, ramen).station;
  CHECK(g.settings[ramen].dish < 0 && !bf_fresh_left(&g, ramen) &&
        !bf_can_take(&g, ramen));
  CHECK(bf_eat_target(&g) != ramen && !g.eaten && !g.value && !g.returned &&
        !g.fullness_milli);
  CHECK(g.service[BF_HOT].next_at_ms == due &&
        g.settings[later].expires_ms == 32200);
  CHECK(bf_expiring(&g, order) == 3 && order[0] == salmon);
  /* Queue ties select ascending slot, with four portions competing for three
   * rows. */
  for (int i = 0; i < BF_SLOTS; i++) g.settings[i] = (bf_setting_t){.dish = -1};
  for (int i = 0; i < 4; i++)
    g.settings[i] = (bf_setting_t){
        .dish = 0, .expires_ms = g.elapsed_ms + 2400, .portion_id = 100u + i};
  CHECK(bf_expiring(&g, order) == 3 && order[0] == 0 && order[1] == 1 &&
        order[2] == 2);
  g.settings[3].expires_ms--;
  CHECK(bf_expiring(&g, order) == 3 && order[0] == 3 && order[1] == 0 &&
        order[2] == 1);
}

static void replace_at_expiry(void) {
  bf_game_t g;
  bf_init(&g, false);
  bf_action(&g, H2_BUFFET_CONFIRM);
  int slot = BF_FIRST_HOT;
  for (int i = 0; i < BF_SLOTS; ++i) g.settings[i] = (bf_setting_t){.dish = -1};
  for (int t = 0; t < BF_TABLES; ++t) g.service[t].next_at_ms = -1;
  g.settings[slot] = (bf_setting_t){.dish = 6,
                                    .arrived_ms = 0,
                                    .expires_ms = 26000,
                                    .portion_id = ++g.served};
  g.service[BF_HOT] = (bf_service_t){.next_at_ms = 26000};
  g.next_stock_event_ms = 26000;
  unsigned old_id = g.settings[slot].portion_id;
  bf_step(&g, 26000, 0, 0);
  CHECK(g.settings[slot].dish == 6 &&
        g.settings[slot].portion_id == old_id + 1);
  CHECK(g.settings[slot].arrived_ms == 26000 &&
        bf_fresh_left(&g, slot) == 26000);
  CHECK(!g.value && !g.eaten && !g.returned);
}

static void compare_steps(void) {
  bf_game_t fine;
  bf_init(&fine, false);
  bf_action(&fine, H2_BUFFET_CONFIRM);
  bf_game_t coarse = fine;
  bf_step(&coarse, 81000, 0, 0);
  for (int t = 0; t < 81000; t += 30) bf_step(&fine, 30, 0, 0);
  CHECK(!memcmp(fine.settings, coarse.settings, sizeof(fine.settings)));
  CHECK(!memcmp(fine.service, coarse.service, sizeof(fine.service)) &&
        fine.served == coarse.served);
  for (int i = 0; i < BF_SLOTS; i++)
    if (fine.settings[i].dish >= 0) {
      const bf_setting_t* serving = &fine.settings[i];
      CHECK(serving->expires_ms > fine.elapsed_ms &&
            serving->expires_ms - serving->arrived_ms ==
                bf_menu[serving->dish].fresh_seconds * 1000);
      for (int j = 0; j < i; j++)
        CHECK(serving->portion_id != fine.settings[j].portion_id);
    }
}

int main(void) {
  expiry_queue();
  replace_at_expiry();
  compare_steps();
  bf_game_t g;
  bf_init(&g, false);
  bf_action(&g, H2_BUFFET_CONFIRM);
  single_file(g);
  access(g);
  CHECK(stock(&g, 0) == 1 && stock(&g, 1) == 1 && stock(&g, 2) == 0);
  CHECK(stock(&g, BF_HOT) == 1 && stock(&g, BF_CUSTARD) == 1);
  for (int t = BF_HOT; t < BF_TABLES; t++) {
    for (int i = 0; i < BF_SIDE_SLOTS; i++) {
      bf_place_t serving = bf_place(&g, bf_tables[t].first_slot + i);
      bf_world_t approach = bf_unproject(serving.station),
                 center = bf_unproject(
                     (bf_point_t){serving.pos.x, serving.pos.y + 18});
      CHECK(bf_walkable(serving.station) &&
            fabsf(approach.v - center.v) < .001f);
      CHECK(fabsf(center.u - (t == BF_HOT ? .85f : 19.15f)) < .001f &&
            fabsf(center.v - 3.7f - 1.5f * i) < .001f);
    }
  }
  int order[BF_TABLES];
  CHECK(bf_next_tables(&g, order) == 5 && order[0] == 1 &&
        order[1] == BF_CUSTARD && order[2] == 0 && order[3] == BF_HOT &&
        order[4] == BF_LEAF);
  int slot = bf_next_setting(&g, 1), dish = bf_next_dish(&g, 1);
  bf_step(&g, 2999, 0, 0);
  CHECK(g.settings[slot].dish < 0 &&
        g.service[1].next_at_ms - g.elapsed_ms == 1);
  bf_step(&g, 1, 0, 0);
  CHECK(g.settings[slot].dish == dish && g.settings[slot].arrived_ms == 3000);
  CHECK(bf_next_tables(&g, order) == BF_TABLES && order[0] == BF_CUSTARD);
  /* Leaf's three settings fill before the first expiry. A vacancy in a
   * stopped row restarts service one period later, even when it was a timeout.
   */
  bf_step(&g, 20800, 0, 0);
  int pass = BF_FIRST_PASS;
  CHECK(stock(&g, BF_LEAF) == 3 && g.service[BF_LEAF].next_at_ms < 0);
  bf_game_t taken = g;
  bf_setting_t other = taken.settings[pass + 1];
  taken.player.pos = bf_place(&taken, pass).station;
  bf_action(&taken, H2_BUFFET_CONFIRM);
  CHECK(taken.settings[pass].dish == -1 && taken.returned == 1 &&
        taken.eaten == 1);
  CHECK(taken.service[BF_LEAF].next_at_ms ==
        taken.elapsed_ms + bf_tables[BF_LEAF].period_ms);
  unsigned first_id = g.settings[pass].portion_id;
  bf_step(&taken, bf_tables[BF_LEAF].period_ms - 1, 0, 0);
  CHECK(taken.settings[pass].dish < 0);
  bf_step(&taken, 1, 0, 0);
  CHECK(taken.settings[pass].portion_id > first_id &&
        taken.settings[pass].arrived_ms == 31900);
  CHECK(!memcmp(&other, &taken.settings[pass + 1], sizeof(other)));
  int expiry = g.settings[pass].expires_ms;
  bf_step(&g, expiry - g.elapsed_ms - 1, 0, 0);
  CHECK(g.settings[pass].portion_id == first_id &&
        bf_fresh_left(&g, pass) == 1);
  bf_step(&g, 1, 0, 0);
  CHECK(g.settings[pass].dish < 0 && !g.returned && !g.eaten);
  CHECK(g.service[BF_LEAF].next_at_ms == expiry + bf_tables[BF_LEAF].period_ms);
  bf_step(&g, bf_rules.duration_ms - g.elapsed_ms, 0, 0);
  CHECK(g.phase == BF_CLOSING && !g.eaten);
  puts(
      "two complete laps, service and expiry boundaries, independent portions, ranked last calls, delayed ticks and refills passed");
  return 0;
}
