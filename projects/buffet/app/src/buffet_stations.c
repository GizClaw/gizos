#include "buffet_rotary.h"
#include <math.h>

const bf_table_t bf_tables[BF_TABLES] = {
    {0, BF_SETTINGS, 5100, 5100, {"潮轮", "Wave"}},
    {BF_SETTINGS, BF_SETTINGS, 5400, 3000, {"日轮", "Sun"}},
    {BF_FIRST_PASS, BF_PASS_SLOTS, 8100, 7600, {"叶台", "Leaf"}},
    {BF_FIRST_HOT, BF_SIDE_SLOTS, 11600, 6200, {"汤面台", "Ramen"}},
    {BF_FIRST_CUSTARD, BF_SIDE_SLOTS, 9600, 4300, {"蒸物台", "Custard"}}};
static const int menu_order[BF_TABLES][4] = {
    {0, 2, 4, 3}, {1, 3, 5, 2}, {5, 4, 1, 0}, {6, 6, 6, 6}, {7, 7, 7, 7}};
/* A service round gives alternating sections first choice. Each carrier
 * retains its identity, including while it travels empty. */
static const int loading_order[BF_SETTINGS] = {4, 0, 6, 2, 5, 1, 7, 3};

static float distance_sq(bf_point_t a, bf_point_t b) {
  float x = a.x - b.x, y = (a.y - b.y) / .75f;
  return x * x + y * y;
}
bool bf_walkable(bf_point_t p) {
  bf_world_t w = bf_unproject(p);
  const float sole = .23f;
  if (w.u < sole || w.u > BF_COLS - sole || w.v < sole || w.v > BF_ROWS - sole)
    return false;
  if (fabsf(w.v - 2.55f) < .11f && (w.u < 8.5f + sole || w.u > 11.5f - sole))
    return false;
  for (int i = 0; i < BF_LOOPS; i++)
    if (bf_inside(w, bf_islands[i], sole)) return false;
  for (unsigned i = 0; i < bf_counter_count; i++)
    if (bf_inside(w, bf_counters[i], sole)) return false;
  return true;
}
int bf_station_of(int slot) {
  for (int t = BF_TABLES - 1; t > 0; t--)
    if (slot >= bf_tables[t].first_slot) return t;
  return 0;
}
bf_place_t bf_place(const bf_game_t* g, int slot) {
  int t = bf_station_of(slot), i = slot - bf_tables[t].first_slot;
  if (t == BF_HOT || t == BF_CUSTARD) {
    float v = 3.7f + 1.5f * i;
    return (bf_place_t){
        bf_project((bf_world_t){t == BF_HOT ? .85f : 19.15f, v}, 18),
        bf_project((bf_world_t){t == BF_HOT ? 1.86f : 18.14f, v}, 0)};
  }
  if (t == BF_LEAF) {
    float u = 8.5f + 1.5f * i;
    return (bf_place_t){bf_project((bf_world_t){u, .85f}, 26),
                        bf_project((bf_world_t){u, 1.88f}, 0)};
  }
  bf_mark_t mark =
      bf_perimeter(bf_inset(bf_islands[t], .47f),
                   bf_loop_phase(t, g->elapsed_ms) + (float)i / BF_SETTINGS);
  bf_world_t reach = {mark.point.u + .89f * mark.normal.u,
                      mark.point.v + .89f * mark.normal.v};
  return (bf_place_t){bf_project(mark.point, BF_TABLE_RISE),
                      bf_project(reach, 0)};
}
static float reach_cost(const bf_game_t* g, int slot) {
  return distance_sq(g->player.pos, bf_place(g, slot).station);
}
int bf_nearest(const bf_game_t* g) {
  float best = 28.f * 28;
  int selected = -1;
  for (int i = 0; i < BF_SLOTS; i++)
    if (g->settings[i].dish >= 0) {
      float d = reach_cost(g, i);
      if (d <= best) {
        best = d;
        selected = i;
      }
    }
  return selected;
}
bool bf_can_take(const bf_game_t* g, int slot) {
  return slot >= 0 && slot < BF_SLOTS && g->settings[slot].dish >= 0 &&
         reach_cost(g, slot) <= 28.f * 28;
}
int bf_next_dish(const bf_game_t* g, int t) {
  return menu_order[t][g->service[t].course % 4];
}
int bf_next_setting(const bf_game_t* g, int t) {
  int n = bf_tables[t].slots;
  for (int i = 0; i < n; i++) {
    int index = (g->service[t].cursor + i) % n;
    int slot =
        bf_tables[t].first_slot + (t < BF_LOOPS ? loading_order[index] : index);
    if (g->settings[slot].dish < 0) return slot;
  }
  return -1;
}
static void serve(bf_game_t* g, int t, int slot, int at) {
  g->settings[slot] = (bf_setting_t){bf_next_dish(g, t), at};
  g->service[t].course++;
  g->service[t].cursor = (g->service[t].cursor + 1) % bf_tables[t].slots;
  g->served++;
}
void bf_service_open(bf_game_t* g) {
  for (int i = 0; i < BF_SLOTS; i++) g->settings[i] = (bf_setting_t){-1, -1};
  for (int t = 0; t < BF_TABLES; t++)
    g->service[t] = (bf_service_t){.next_at_ms = bf_tables[t].first_ms};
  serve(g, 0, 4, -1000);
  serve(g, 1, BF_SETTINGS + 6, -1000);
  serve(g, BF_HOT, BF_FIRST_HOT + 2, -1000);
  serve(g, BF_CUSTARD, BF_FIRST_CUSTARD + 2, -1000);
}
void bf_service_update(bf_game_t* g) {
  for (int t = 0; t < BF_TABLES; t++) {
    bf_service_t* q = &g->service[t];
    while (q->next_at_ms >= 0 && q->next_at_ms <= g->elapsed_ms &&
           q->next_at_ms < bf_rules.duration_ms) {
      int slot = bf_next_setting(g, t);
      if (slot < 0) {
        q->next_at_ms = -1;
        break;
      }
      int at = q->next_at_ms;
      serve(g, t, slot, at);
      q->next_at_ms =
          bf_next_setting(g, t) < 0 ? -1 : at + bf_tables[t].period_ms;
    }
  }
}
void bf_service_clear(bf_game_t* g, int slot) {
  int t = bf_station_of(slot);
  g->settings[slot].dish = -1;
  g->returned++;
  if (g->service[t].next_at_ms < 0)
    g->service[t].next_at_ms = g->elapsed_ms + bf_tables[t].period_ms;
}
int bf_next_tables(const bf_game_t* g, int order[BF_TABLES]) {
  int n = 0;
  for (int t = 0; t < BF_TABLES; t++) {
    int due = g->service[t].next_at_ms;
    if (due < 0 || due >= bf_rules.duration_ms) continue;
    int j = n++;
    while (j && due < g->service[order[j - 1]].next_at_ms) {
      order[j] = order[j - 1];
      j--;
    }
    order[j] = t;
  }
  return n;
}

static bool clear_line(bf_point_t a, bf_point_t b) {
  int steps = (int)(sqrtf(distance_sq(a, b)) / 2.8f) + 1;
  for (int i = 1; i <= steps; i++) {
    float t = (float)i / steps;
    if (!bf_walkable(
            (bf_point_t){a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}))
      return false;
  }
  return true;
}
/* The visibility graph contains the four outer approaches of each loop and
 * the stair landings. Direct travel and every edge use the same sole test. */
void bf_walk_to(bf_game_t* g, bf_point_t dest) {
  enum { N = 2 + 4 * BF_LOOPS + 6 };
  if (!bf_walkable(dest)) return;
  if (clear_line(g->player.pos, dest)) {
    g->route[0] = dest;
    g->route_at = 0;
    g->route_len = 1;
    return;
  }
  bf_point_t node[N] = {g->player.pos, dest};
  for (int loop = 0; loop < BF_LOOPS; loop++) {
    bf_footprint_t r = bf_islands[loop];
    for (int j = 0; j < 4; j++)
      node[2 + loop * 4 + j] =
          bf_project((bf_world_t){j & 1 ? r.u + r.w + .40f : r.u - .40f,
                                  j & 2 ? r.v + r.h + .40f : r.v - .40f},
                     0);
  }
  const bf_world_t gates[] = {{10, 3.45f}, {10, 2.01f},     {2, 3.25f},
                              {18, 3.25f}, {2.15f, 11.17f}, {17.85f, 11.17f}};
  for (int i = 0; i < 6; i++) node[N - 6 + i] = bf_project(gates[i], 0);
  float best[N];
  bool done[N] = {0};
  int prev[N];
  for (int i = 0; i < N; i++) {
    best[i] = 1e8f;
    prev[i] = -1;
  }
  best[0] = 0;
  for (int i = 0; i < N; i++) {
    int at = -1;
    for (int j = 0; j < N; j++)
      if (!done[j] && (at < 0 || best[j] < best[at])) at = j;
    if (at < 0 || at == 1 || best[at] >= 1e8f) break;
    done[at] = true;
    for (int j = 0; j < N; j++)
      if (!done[j]) {
        float d = best[at] + sqrtf(distance_sq(node[at], node[j]));
        if (d < best[j] && clear_line(node[at], node[j])) {
          prev[j] = at;
          best[j] = d;
        }
      }
  }
  if (prev[1] < 0) return;
  int reverse[N], n = 0;
  for (int j = 1; j > 0; j = prev[j]) reverse[n++] = j;
  g->route_at = 0;
  g->route_len = n;
  for (int i = 0; i < n; i++) g->route[i] = node[reverse[n - 1 - i]];
}
bf_face_t bf_facing(float dx, float dy) {
  return fabsf(dx) > fabsf(dy) * 1.2f ? (dx < 0 ? BF_LEFT : BF_RIGHT)
                                      : (dy < 0 ? BF_UP : BF_DOWN);
}
void bf_walk(bf_game_t* g, int dt, int dx, int dy) {
  g->player.walking = false;
  if (g->bite_ms) return;
  bool route = !dx && !dy;
  float vx = dx, vy = dy * .75f;
  if (!route) {
    g->route_at = g->route_len;
    g->follow_slot = -1;
  } else {
    if (g->follow_slot >= 0) {
      if (bf_can_take(g, g->follow_slot)) {
        g->follow_slot = -1;
        g->route_at = g->route_len;
        return;
      }
      if (g->elapsed_ms >= g->follow_at_ms) {
        bf_walk_to(g, bf_place(g, g->follow_slot).station);
        g->follow_at_ms = g->elapsed_ms + 220;
      }
    }
    if (g->route_at >= g->route_len) return;
    vx = g->route[g->route_at].x - g->player.pos.x;
    vy = g->route[g->route_at].y - g->player.pos.y;
  }
  float length = hypotf(vx, vy / .75f);
  if (length <= .1f) {
    if (route) g->route_at++;
    return;
  }
  bf_face_t face = bf_facing(vx, vy);
  if (face != g->player.face) g->turn_ms = 115;
  g->player.face = face;
  float delta = dt * .173f;
  if (route) delta = fminf(delta, length);
  bf_point_t old = g->player.pos,
             next = {old.x + vx * delta / length, old.y + vy * delta / length};
  if (clear_line(old, next))
    g->player.pos = next;
  else if (!route) {
    bf_point_t x = {next.x, old.y}, y = {old.x, next.y};
    if (clear_line(old, x))
      g->player.pos = x;
    else if (clear_line(old, y))
      g->player.pos = y;
  }
  float moved = sqrtf(distance_sq(old, g->player.pos));
  g->player.walking = moved > .03f;
  g->player.stride += moved * .061f;
  if (route && distance_sq(g->player.pos, g->route[g->route_at]) < .2f)
    g->route_at++;
}
