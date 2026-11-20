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
enum { REACH_SQUARED = 28 * 28 };
static float reach_cost(const bf_game_t* g, int slot) {
  if (bf_fresh_left(g, slot) == 0) return REACH_SQUARED + 1;
  return distance_sq(g->player.pos, bf_place(g, slot).station);
}
int bf_fresh_left(const bf_game_t* g, int slot) {
  if (slot < 0 || slot >= BF_SLOTS) return 0;
  const bf_setting_t* serving = &g->settings[slot];
  return serving->dish >= 0 && serving->expires_ms > g->elapsed_ms
             ? serving->expires_ms - g->elapsed_ms
             : 0;
}

/* A bounded insertion keeps at most three live portions. Visiting slots in
 * ascending order also resolves equal deadlines consistently. */
int bf_expiring(const bf_game_t* g, int order[BF_FRESH_QUEUE]) {
  int count = 0;
  for (int slot = 0; slot < BF_SLOTS; ++slot) {
    int left = bf_fresh_left(g, slot);
    if (left == 0 || left > BF_FRESH_SOON_MS) continue;
    int pos = count;
    while (pos > 0 && g->settings[order[pos - 1]].expires_ms >
                          g->settings[slot].expires_ms)
      --pos;
    if (pos == BF_FRESH_QUEUE) continue;
    if (count < BF_FRESH_QUEUE) ++count;
    for (int j = count - 1; j > pos; --j) order[j] = order[j - 1];
    order[pos] = slot;
  }
  return count;
}

int bf_nearest(const bf_game_t* g) {
  float best = REACH_SQUARED;
  int selected = -1;
  for (int i = 0; i < BF_SLOTS; i++) {
    float cost = reach_cost(g, i);
    if (cost <= best) {
      best = cost;
      selected = i;
    }
  }
  return selected;
}
bool bf_can_take(const bf_game_t* g, int slot) {
  return reach_cost(g, slot) <= REACH_SQUARED;
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
  int dish = bf_next_dish(g, t);
  g->settings[slot] =
      (bf_setting_t){.dish = dish,
                     .arrived_ms = at,
                     .expires_ms = at + 1000 * bf_menu[dish].fresh_seconds,
                     .portion_id = ++g->served};
  g->service[t].course++;
  g->service[t].cursor = (g->service[t].cursor + 1) % bf_tables[t].slots;
}

static void schedule_next_event(bf_game_t* g) {
  int next = bf_rules.duration_ms;
  for (int t = 0; t < BF_TABLES; ++t) {
    int at = g->service[t].next_at_ms;
    if (at >= 0 && at < next) next = at;
  }
  for (int slot = 0; slot < BF_SLOTS; ++slot) {
    const bf_setting_t* serving = &g->settings[slot];
    if (serving->dish >= 0 && serving->expires_ms < next)
      next = serving->expires_ms;
  }
  g->next_stock_event_ms = next;
}

static void vacate(bf_game_t* g, int slot, int at) {
  int t = bf_station_of(slot);
  g->settings[slot] = (bf_setting_t){.dish = -1};
  if (g->service[t].next_at_ms < 0)
    g->service[t].next_at_ms = at + bf_tables[t].period_ms;
}

void bf_service_open(bf_game_t* g) {
  g->served = 0;
  for (int i = 0; i < BF_SLOTS; i++)
    g->settings[i] = (bf_setting_t){.dish = -1};
  for (int t = 0; t < BF_TABLES; t++)
    g->service[t] = (bf_service_t){.next_at_ms = bf_tables[t].first_ms};
  serve(g, 0, 4, 0);
  serve(g, 1, BF_SETTINGS + 6, 0);
  serve(g, BF_HOT, BF_FIRST_HOT + 2, 0);
  serve(g, BF_CUSTARD, BF_FIRST_CUSTARD + 2, 0);
  schedule_next_event(g);
}
void bf_service_update(bf_game_t* g) {
  /* Advance stock at exact meal-clock deadlines, including a delayed frame
   * spanning several courses. An expiry frees its setting before service at
   * the same instant. Frames between deadlines cost one integer comparison. */
  while (g->next_stock_event_ms <= g->elapsed_ms &&
         g->next_stock_event_ms < bf_rules.duration_ms) {
    int at = g->next_stock_event_ms;
    for (int slot = 0; slot < BF_SLOTS; ++slot) {
      const bf_setting_t* serving = &g->settings[slot];
      if (serving->dish >= 0 && serving->expires_ms == at) vacate(g, slot, at);
    }
    for (int t = 0; t < BF_TABLES; ++t) {
      bf_service_t* service = &g->service[t];
      if (service->next_at_ms != at) continue;
      int slot = bf_next_setting(g, t);
      if (slot >= 0) serve(g, t, slot, at);
      service->next_at_ms =
          bf_next_setting(g, t) < 0 ? -1 : at + bf_tables[t].period_ms;
    }
    schedule_next_event(g);
  }
}
void bf_service_clear(bf_game_t* g, int slot) {
  if (bf_fresh_left(g, slot) == 0) return;
  vacate(g, slot, g->elapsed_ms);
  ++g->returned;
  schedule_next_event(g);
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
bf_face_t bf_facing(float dx, float dy) {
  return fabsf(dx) > fabsf(dy) * 1.2f ? (dx < 0 ? BF_LEFT : BF_RIGHT)
                                      : (dy < 0 ? BF_UP : BF_DOWN);
}
void bf_walk(bf_game_t* g, int dt, float dx, float dy) {
  g->player.walking = false;
  if (g->bite_ms || (!dx && !dy)) return;
  float vx = dx, vy = dy * .75f;
  float length = hypotf(dx, dy);
  bf_face_t face = bf_facing(vx, vy);
  if (face != g->player.face) g->turn_ms = 115;
  g->player.face = face;
  float delta = dt * .173f / fmaxf(1.f, length);
  bf_point_t old = g->player.pos,
             next = {old.x + vx * delta, old.y + vy * delta};
  if (clear_line(old, next))
    g->player.pos = next;
  else {
    bf_point_t x = {next.x, old.y}, y = {old.x, next.y};
    if (clear_line(old, x))
      g->player.pos = x;
    else if (clear_line(old, y))
      g->player.pos = y;
  }
  float moved = sqrtf(distance_sq(old, g->player.pos));
  g->player.walking = moved > .03f;
  g->player.stride += moved * .061f;
}
