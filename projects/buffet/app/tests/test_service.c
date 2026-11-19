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
  const bf_world_t stops[] = {
      {10, 2.03f}, {2.15f, 7}, {17.82f, 7}, {10, 6}, {10, 11}};
  for (unsigned i = 0; i < sizeof(stops) / sizeof(stops[0]); i++) {
    bf_point_t target = bf_project(stops[i], 0);
    bf_walk_to(&g, target);
    for (int k = 0; k < 280 && g.route_at < g.route_len; k++) {
      bf_step(&g, 33, 0, 0);
      CHECK(bf_walkable(g.player.pos));
    }
    CHECK(hypotf(target.x - g.player.pos.x, target.y - g.player.pos.y) < 1);
  }
  CHECK(!bf_walkable(bf_project((bf_world_t){6, 2.55f}, 0)));
  CHECK(bf_walkable(bf_project((bf_world_t){10, 2.55f}, 0)));
}
int main(void) {
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
  bf_step(&g, 40000, 0, 0);
  for (int t = 0; t < BF_TABLES; t++)
    CHECK(stock(&g, t) == bf_tables[t].slots && g.service[t].next_at_ms < 0);
  CHECK(bf_next_tables(&g, order) == 0);
  bf_setting_t occupied[BF_SLOTS];
  memcpy(occupied, g.settings, sizeof(occupied));
  bf_step(&g, 30000, 0, 0);
  CHECK(!memcmp(occupied, g.settings, sizeof(occupied)));
  g.player.pos = bf_place(&g, slot).station;
  bf_place_t plate = bf_place(&g, slot);
  bf_tap(&g, plate.pos.x, plate.pos.y - 4);
  CHECK(g.eaten == 1 && g.settings[slot].dish < 0 && g.returned == 1);
  CHECK(bf_next_tables(&g, order) == 1 && order[0] == 1);
  CHECK(g.service[1].next_at_ms == g.elapsed_ms + bf_tables[1].period_ms);
  dish = bf_next_dish(&g, 1);
  bf_step(&g, bf_tables[1].period_ms - 1, 0, 0);
  CHECK(g.settings[slot].dish < 0);
  bf_step(&g, 1, 0, 0);
  CHECK(g.settings[slot].dish == dish && bf_next_tables(&g, order) == 0);
  for (int i = 0; i < BF_SLOTS; i++)
    if (i != slot) CHECK(g.settings[i].arrived_ms == occupied[i].arrived_ms);
  g.elapsed_ms = bf_rules.duration_ms - 100;
  bf_service_clear(&g, slot);
  CHECK(!bf_next_tables(&g, order));
  bf_step(&g, 100, 0, 0);
  CHECK(g.phase == BF_CLOSING && g.settings[slot].dish < 0);
  puts(
      "two complete laps, fixed single-file pass, four routes, staggered deadlines, stock holds and closing passed");
  return 0;
}
