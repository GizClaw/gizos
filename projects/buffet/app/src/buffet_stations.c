#include "buffet_game.h"
#include <math.h>

/* Floor positions project 48x36 from square world tiles. Tables, service
 * approach points and path finding all share these footprints. */
const bf_table_t bf_tables[BF_TABLES] = {{2, 2, 6800, 5100, {"潮桌", "Wave"}},
                                         {6, 5, 6100, 7600, {"叶桌", "Leaf"}},
                                         {10, 2, 7400, 3000, {"日桌", "Sun"}}};
static const int menu_order[BF_TABLES][4] = {
    {0, 2, 3, 0}, {1, 4, 5, 1}, {3, 5, 2, 4}};
static const int next_setting[3] = {1, 0, 2};

bf_rect_t bf_table_footprint(int table) {
  const bf_table_t* t = &bf_tables[table];
  return (bf_rect_t){BF_FLOOR_X + t->col * BF_TILE_W,
                     BF_FLOOR_Y + t->row * BF_TILE_H, 3 * BF_TILE_W,
                     2 * BF_TILE_H};
}
static float squared(float x) { return x * x; }
static float distance_sq(bf_point_t a, bf_point_t b) {
  return squared(a.x - b.x) + squared((a.y - b.y) * BF_TILE_W / BF_TILE_H);
}
static float bounded(float x, float low, float high) {
  return fminf(high, fmaxf(low, x));
}
bool bf_walkable(bf_point_t p) {
  if (p.x < BF_FLOOR_X + 8 || p.x > BF_FLOOR_X + BF_TILE_W * BF_COLS - 8 ||
      p.y < BF_FLOOR_Y + 6 || p.y > BF_FLOOR_Y + BF_TILE_H * BF_ROWS - 6)
    return false;
  for (int t = 0; t < BF_TABLES; t++) {
    bf_rect_t r = bf_table_footprint(t);
    if (p.x > r.x - 8 && p.x < r.x + r.w + 8 && p.y > r.y - 6 &&
        p.y < r.y + r.h + 6)
      return false;
  }
  return true;
}
bf_place_t bf_place(const bf_game_t* g, int slot) {
  int table = slot / BF_SETTINGS, index = slot % BF_SETTINGS;
  bf_rect_t r = bf_table_footprint(table);
  float x = r.x + r.w * (.2f + .3f * index);
  bf_place_t p = {.pos = {x, r.y - BF_TABLE_RISE + r.h * .6f},
                  .station = {x, r.y + r.h + 9}};
  bf_point_t options[2] = {
      {x, r.y - 9}, {index == 0 ? r.x - 12 : r.x + r.w + 12, r.y + r.h * .57f}};
  for (int i = 0; i < (index == 1 ? 1 : 2); i++)
    if (distance_sq(g->player.pos, options[i]) <
        distance_sq(g->player.pos, p.station))
      p.station = options[i];
  return p;
}
static float reach_cost(const bf_game_t* g, int slot) {
  bf_point_t edge = bf_place(g, slot).station;
  return squared((g->player.pos.x - edge.x) / 33.f) +
         squared((g->player.pos.y - edge.y) / 24.f);
}
int bf_nearest(const bf_game_t* g) {
  float best = 1.f;
  int selected = -1;
  for (int slot = 0; slot < BF_SLOTS; slot++)
    if (g->settings[slot].dish >= 0) {
      float cost = reach_cost(g, slot);
      if (cost <= best) {
        selected = slot;
        best = cost;
      }
    }
  return selected;
}
bool bf_can_take(const bf_game_t* g, int slot) {
  return slot >= 0 && slot < BF_SLOTS && g->settings[slot].dish >= 0 &&
         reach_cost(g, slot) <= 1.f;
}
int bf_next_dish(const bf_game_t* g, int t) {
  return menu_order[t][g->service[t].course % 4];
}
int bf_next_setting(const bf_game_t* g, int t) {
  for (int i = 0; i < BF_SETTINGS; i++) {
    int slot = t * BF_SETTINGS +
               next_setting[(g->service[t].cursor + i) % BF_SETTINGS];
    if (g->settings[slot].dish < 0) return slot;
  }
  return -1;
}
static void serve(bf_game_t* g, int t, int slot, int at) {
  g->settings[slot] = (bf_setting_t){bf_next_dish(g, t), at};
  g->service[t].course++;
  g->service[t].cursor = (g->service[t].cursor + 1) % BF_SETTINGS;
}
void bf_service_open(bf_game_t* g) {
  for (int i = 0; i < BF_SLOTS; i++) g->settings[i] = (bf_setting_t){-1, -1};
  for (int t = 0; t < BF_TABLES; t++)
    g->service[t] = (bf_service_t){.next_at_ms = bf_tables[t].first_ms};
  serve(g, 0, 1, -1000);
  serve(g, 1, 4, -1000);
}
void bf_service_update(bf_game_t* g) {
  for (int t = 0; t < BF_TABLES; t++) {
    bf_service_t* q = &g->service[t];
    /* Even a long elapsed step can fill at most three settings. A full table
     * takes no more orders until a setting becomes free. */
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
  int t = slot / BF_SETTINGS;
  g->settings[slot].dish = -1;
  /* Staff clear the setting. The existing tickets count returned plates. */
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
  int steps = (int)(sqrtf(distance_sq(a, b)) / 4) + 1;
  for (int i = 1; i <= steps; i++) {
    float t = (float)i / steps;
    if (!bf_walkable(
            (bf_point_t){a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}))
      return false;
  }
  return true;
}
void bf_walk_to(bf_game_t* g, bf_point_t dest) {
  enum { N = 2 + 4 * BF_TABLES };
  bf_point_t node[N] = {g->player.pos};
  dest.x =
      bounded(dest.x, BF_FLOOR_X + 8, BF_FLOOR_X + BF_COLS * BF_TILE_W - 8);
  dest.y =
      bounded(dest.y, BF_FLOOR_Y + 6, BF_FLOOR_Y + BF_ROWS * BF_TILE_H - 6);
  if (!bf_walkable(dest)) return;
  node[1] = dest;
  for (int t = 0; t < BF_TABLES; t++) {
    bf_rect_t r = bf_table_footprint(t);
    for (int k = 0; k < 4; k++)
      node[2 + 4 * t + k] = (bf_point_t){k & 1 ? r.x + r.w + 11 : r.x - 11,
                                         k & 2 ? r.y + r.h + 8 : r.y - 8};
  }
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
  g->route_len = n;
  g->route_at = 0;
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
  if (!route)
    g->route_at = g->route_len;
  else if (g->route_at < g->route_len) {
    vx = g->route[g->route_at].x - g->player.pos.x;
    vy = g->route[g->route_at].y - g->player.pos.y;
  } else
    return;
  float length = hypotf(vx, vy / 0.75f);
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
  if (bf_walkable(next))
    g->player.pos = next;
  else if (!route) {
    bf_point_t x = {next.x, old.y}, y = {old.x, next.y};
    if (bf_walkable(x))
      g->player.pos = x;
    else if (bf_walkable(y))
      g->player.pos = y;
  }
  float moved = sqrtf(distance_sq(old, g->player.pos));
  g->player.walking = moved > .03f;
  g->player.stride += moved * .061f;
  if (route && distance_sq(g->player.pos, g->route[g->route_at]) < .2f)
    g->route_at++;
}
