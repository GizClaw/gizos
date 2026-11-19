#include "buffet_game.h"

#include <math.h>
#include <string.h>

const bf_rules_t bf_rules = {.duration_ms = 150000,
                             .purse = 80,
                             .ticket = 48,
                             .fee = 24,
                             .capacity = 100,
                             .red_line = 120,
                             .tolerance_ms = 7000,
                             .digest_milli_per_ms = 2,
                             .lap_ms = 27000};
const bf_dish_t bf_menu[BF_DISHES] = {
    {"sushi-seared-salmon",
     {"炙烧鲑鱼握", "Seared Salmon Nigiri"},
     {"油脂间留着细细的焦痕。", "A ribbon of fat under a toasted edge."},
     19,
     23},
    {"sushi-tamago",
     {"厚烧玉子握", "Tamagoyaki Nigiri"},
     {"层层蛋卷，束一圈海苔。", "Folded egg, tied with a strip of nori."},
     12,
     20},
    {"sushi-tuna",
     {"金枪鱼握", "Tuna Nigiri"},
     {"薄切鱼肉上有浅色纹理。", "Fine pale seams in a deep red cut."},
     17,
     18},
    {"sushi-shrimp",
     {"熟虾握", "Cooked Shrimp Nigiri"},
     {"弯弯虾身，尾尖留红。", "A curled prawn with a coral tail."},
     15,
     17},
    {"sushi-cucumber-roll",
     {"黄瓜细卷", "Cucumber Maki"},
     {"海苔卷着脆嫩瓜心。", "Cool cucumber inside a crisp nori wrap."},
     8,
     10},
    {"sushi-inari",
     {"豆皮寿司", "Inari Sushi"},
     {"豆皮袋里点上几粒芝麻。", "A sweet tofu pocket finished with sesame."},
     11,
     25},
};
const bf_rect_t bf_language_button = {710, 12, 74, 34},
                bf_goal_button = {502, 224, 237, 42},
                bf_ticket_button = {502, 356, 237, 54},
                bf_eat_button = {625, 391, 154, 66};
static const float pi = 3.14159265f;
static const int order[BF_SLOTS] = {0, 4, 2, 1, 3, 5, 0, 3, 4, 1};
static float sq(float v) { return v * v; }
static int lower(int a, int b) { return a < b ? a : b; }
static int down_timer(int a, int dt) { return a > dt ? a - dt : 0; }
static float clampf(float x, float lo, float hi) {
  return fminf(hi, fmaxf(lo, x));
}
static float distance(bf_point_t a, bf_point_t b) {
  return hypotf(a.x - b.x, a.y - b.y);
}
static float distance_sq(bf_point_t a, bf_point_t b) {
  return sq(a.x - b.x) + sq(a.y - b.y);
}
bool bf_in(bf_rect_t b, float x, float y) {
  return x >= b.x && y >= b.y && x < b.x + b.w && y < b.y + b.h;
}

/* Parameterize the two straight runs and two half turns separately. The
 * same moving sample owns its picture, grab radius and walk-to station. */
bf_place_t bf_belt(float lap) {
  float d = (lap - floorf(lap)) * 1010.f;
  bf_point_t p, n;
  if (d < 368) {
    p = (bf_point_t){216 + d, 194};
    n = (bf_point_t){0, -1};
  } else if (d < 505) {
    float a = (d - 368) * pi / 137 - pi / 2;
    n = (bf_point_t){cosf(a), sinf(a)};
    p = (bf_point_t){584 + 51 * n.x, 225 + 31 * n.y};
  } else if (d < 873) {
    p = (bf_point_t){584 - (d - 505), 256};
    n = (bf_point_t){0, 1};
  } else {
    float a = (d - 873) * pi / 137 + pi / 2;
    n = (bf_point_t){cosf(a), sinf(a)};
    p = (bf_point_t){216 + 51 * n.x, 225 + 31 * n.y};
  }
  return (bf_place_t){p, {p.x + n.x * 39, p.y + 13 + n.y * 40}, n};
}
/* One turn crosses the kitchen pass on the rear run. Each dirty saucer goes
 * back exactly there. It may be inspected in transit, but never eaten twice. */
static float slot_lap(int ms, int i) {
  return .675f + (float)i / BF_SLOTS + (float)ms / bf_rules.lap_ms;
}
bf_place_t bf_place(const bf_game_t* g, int i) {
  return bf_belt(slot_lap(g->elapsed_ms, i));
}
int bf_serving(const bf_game_t* g, int i) {
  int lap = (int)floorf(slot_lap(g->elapsed_ms, i) - .18f);
  return (order[i] + lap + 6) % BF_DISHES;
}
static float reach_cost(const bf_game_t* g, int i) {
  bf_point_t t = bf_place(g, i).station;
  return sq((g->player.pos.x - t.x) / 44.f) +
         sq((g->player.pos.y - t.y) / 29.f);
}
int bf_nearest(const bf_game_t* g) {
  int best = -1;
  float cost = 1.f;
  for (int i = 0; i < BF_SLOTS; i++) {
    float c = reach_cost(g, i);
    if (!g->empty[i] && c <= cost) {
      best = i;
      cost = c;
    }
  }
  return best;
}
bool bf_at_belt(const bf_game_t* g) {
  for (int i = 0; i < 48; i++) {
    bf_point_t p = bf_belt((float)i / 48.f).station;
    if (sq((p.x - g->player.pos.x) / 40.f) +
            sq((p.y - g->player.pos.y) / 26.f) <=
        1)
      return true;
  }
  return false;
}
bool bf_has_goal(const bf_game_t* g) {
  return g->tasting ? g->varieties == BF_DISHES : g->value >= bf_rules.ticket;
}

bool bf_walkable(bf_point_t p) {
  if (p.x < 40 || p.x > 760 || p.y < 150 || p.y > 361) return false;
  if ((p.x < 127 || p.x > 676) && p.y < 157) return false;
  if ((p.x < 89 || p.x > 711) &&
      ((p.y > 174 && p.y < 256) || (p.y > 284 && p.y < 361)))
    return false;
  float dx = p.x - clampf(p.x, 215, 585);
  return sq(dx / 78.f) + sq((p.y - 234) / 60.f) >= 1;
}
static bf_face_t facing(float dx, float dy) {
  return fabsf(dx) > fabsf(dy) * 1.4f ? (dx < 0 ? BF_LEFT : BF_RIGHT)
                                      : (dy < 0 ? BF_UP : BF_DOWN);
}
/* Sight graph around the expanded counter. The two sides remain reachable
 * while other guests occupy an aisle. Guest collisions resolve during the walk.
 */
static bool clear_line(bf_point_t a, bf_point_t b) {
  int steps = (int)(distance(a, b) / 6) + 1;
  for (int i = 1; i <= steps; i++) {
    float t = (float)i / steps;
    if (!bf_walkable(
            (bf_point_t){a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}))
      return false;
  }
  return true;
}
static void walk_to(bf_game_t* g, bf_point_t dest) {
  enum { N = 8 };
  bf_point_t node[N] = {g->player.pos, {0, 0},     {141, 168}, {659, 168},
                        {681, 234},    {659, 302}, {141, 302}, {119, 234}};
  dest.x = clampf(dest.x, 41, 759);
  dest.y = clampf(dest.y, 153, 360);
  if (!bf_walkable(dest)) return;
  node[1] = dest;
  float best[N];
  int from[N];
  bool done[N] = {0};
  for (int i = 0; i < N; i++) {
    best[i] = 1e8f;
    from[i] = -1;
  }
  best[0] = 0;
  for (int run = 0; run < N; run++) {
    int at = -1;
    for (int j = 0; j < N; j++)
      if (!done[j] && (at < 0 || best[j] < best[at])) at = j;
    if (at < 0 || best[at] >= 1e8f || at == 1) break;
    done[at] = true;
    for (int j = 0; j < N; j++)
      if (!done[j]) {
        float cost = best[at] + distance(node[at], node[j]);
        if (cost < best[j] && clear_line(node[at], node[j])) {
          best[j] = cost;
          from[j] = at;
        }
      }
  }
  if (from[1] < 0) return;
  int reverse[N], len = 0;
  for (int j = 1; j > 0; j = from[j]) reverse[len++] = j;
  g->route_len = len;
  g->route_at = 0;
  for (int i = 0; i < len; i++) g->route[i] = node[reverse[len - 1 - i]];
}

static void diner(bf_actor_t* a, int ms, int who) {
  static const bf_point_t points[][6] = {
      {{95, 177}, {315, 162}, {610, 164}, {684, 178}, {694, 303}, {671, 336}},
      {{688, 325}, {534, 316}, {250, 329}, {110, 319}, {101, 243}, {120, 184}}};
  /* Pace between seats and the far end, then retrace. A short look at the
   * belt at each waypoint naturally opens a gap at the one just passed. */
  const bf_point_t* path = points[who];
  const float speed = who ? 49.f : 44.f;
  int lift = who ? 2350 : 1130;
  int spans[10], cycle = 0;
  for (int j = 0; j < 10; j++) {
    int k = j < 5 ? j : 10 - j, l = j < 5 ? j + 1 : 9 - j;
    spans[j] = (int)(distance(path[k], path[l]) * 1000 / speed);
    cycle += spans[j] + lift;
  }
  int time = (ms + (who ? 4200 : 3000)) % cycle;
  for (int j = 0; j < 10; j++) {
    if (time >= spans[j] + lift) {
      time -= spans[j] + lift;
      continue;
    }
    int k = j < 5 ? j : 10 - j, l = j < 5 ? j + 1 : 9 - j;
    float t = clampf((float)time / spans[j], 0, 1);
    a->pos = (bf_point_t){path[k].x + (path[l].x - path[k].x) * t,
                          path[k].y + (path[l].y - path[k].y) * t};
    a->walking = time < spans[j];
    a->face = a->walking ? facing(path[l].x - path[k].x, path[l].y - path[k].y)
                         : (a->pos.y < 230 ? BF_DOWN : BF_UP);
    a->stride = (float)time * .0052f + who;
    break;
  }
}
void bf_init(bf_game_t* g, bool keyboard) {
  memset(g, 0, sizeof(*g));
  g->phase = BF_TICKET;
  g->keyboard = keyboard;
  g->purse = bf_rules.purse;
  g->last_dish = -1;
}
static void buy(bf_game_t* g) {
  bool en = g->en, tasting = g->tasting;
  int cover = g->cover;
  bf_init(g, g->keyboard);
  g->en = en;
  g->tasting = tasting;
  g->cover = cover;
  g->phase = BF_DINING;
  g->purse -= bf_rules.ticket;
  g->player = (bf_actor_t){{409, 337}, BF_DOWN, 0, false};
  for (int i = 0; i < 2; i++) diner(&g->diners[i], 0, i);
}
static void eat(bf_game_t* g, int slot) {
  if (g->phase != BF_DINING || g->bite_ms || slot < 0 || g->empty[slot] ||
      reach_cost(g, slot) > 1)
    return;
  int dish = bf_serving(g, slot);
  const bf_dish_t* d = &bf_menu[dish];
  g->empty[slot] = true;
  if (g->bites[dish]++ == 0) g->varieties++;
  g->value += d->coins;
  g->fullness_milli += d->fullness * 1000;
  g->eaten++;
  g->last_dish = dish;
  g->route_at = g->route_len;
  g->player.walking = false;
  g->bite_ms = 680;
  g->feedback_ms = 1020;
  bf_point_t p = bf_place(g, slot).pos;
  g->player.face = facing(p.x - g->player.pos.x, p.y - g->player.pos.y);
}
void bf_action(bf_game_t* g, unsigned actions) {
  if (g->phase == BF_TICKET) {
    if (actions & H2_BUFFET_LANGUAGE) g->en = !g->en;
    if (actions & H2_BUFFET_GOAL) g->tasting = !g->tasting;
    if (actions & H2_BUFFET_NEXT) g->cover = (g->cover + 1) % BF_DISHES;
    if (actions & H2_BUFFET_PREVIOUS)
      g->cover = (g->cover + BF_DISHES - 1) % BF_DISHES;
    if (actions & H2_BUFFET_CONFIRM) buy(g);
  } else if (actions & H2_BUFFET_CONFIRM) {
    if (g->phase == BF_DINING)
      eat(g, bf_nearest(g));
    else if (g->receipt_ms >= 650) {
      g->phase = BF_TICKET;
      g->purse = bf_rules.purse;
    }
  }
}
void bf_tap(bf_game_t* g, float x, float y) {
  if (g->phase == BF_TICKET) {
    if (bf_in(bf_language_button, x, y))
      bf_action(g, H2_BUFFET_LANGUAGE);
    else if (bf_in(bf_goal_button, x, y))
      bf_action(g, H2_BUFFET_GOAL);
    else if (bf_in(bf_ticket_button, x, y))
      bf_action(g, H2_BUFFET_CONFIRM);
    else if (bf_in((bf_rect_t){24, 389, 420, 65}, x, y))
      g->cover = (int)(x - 24) / 70;
    return;
  }
  if (g->phase != BF_DINING) {
    if (bf_in(bf_ticket_button, x, y)) bf_action(g, H2_BUFFET_CONFIRM);
    return;
  }
  if (bf_in(bf_eat_button, x, y)) {
    eat(g, bf_nearest(g));
    return;
  }
  if (y < 132 || y > 374) return;
  float cost = 1;
  int pick = -1;
  for (int i = 0; i < BF_SLOTS; i++) {
    bf_place_t p = bf_place(g, i);
    float hit = sq((x - p.pos.x) / 32.f) + sq((y - (p.pos.y - 6)) / 19.f);
    if (hit <= cost) {
      cost = hit;
      pick = i;
    }
  }
  if (pick >= 0) {
    if (!g->empty[pick] && reach_cost(g, pick) <= 1)
      eat(g, pick);
    else
      walk_to(g, bf_place(g, pick).station);
  } else
    walk_to(g, (bf_point_t){x, y});
}
static bool motion_allowed(bf_game_t* g, bf_point_t next) {
  if (!bf_walkable(next)) return false;
  for (int i = 0; i < 2; i++) {
    bf_point_t n = g->diners[i].pos;
    float c = sq((next.x - n.x) / 24.f) + sq((next.y - n.y) / 14.f);
    float old =
        sq((g->player.pos.x - n.x) / 24.f) + sq((g->player.pos.y - n.y) / 14.f);
    if (c < 1 && c < old) {
      g->obstructed = true;
      return false;
    }
  }
  return true;
}
static void walk(bf_game_t* g, int dt, int dx, int dy) {
  g->player.walking = false;
  g->obstructed = false;
  if (g->bite_ms) return;
  float vx = dx, vy = dy;
  bool pointer = dx == 0 && dy == 0;
  if (!pointer)
    g->route_at = g->route_len;
  else if (g->route_at < g->route_len) {
    vx = g->route[g->route_at].x - g->player.pos.x;
    vy = g->route[g->route_at].y - g->player.pos.y;
  } else
    return;
  float length = hypotf(vx, vy);
  if (length <= .1f) {
    if (pointer) g->route_at++;
    return;
  }
  bf_face_t face = facing(vx, vy);
  if (face != g->player.face) g->turn_ms = 115;
  g->player.face = face;
  float delta = dt * .152f;
  if (pointer) delta = fminf(delta, length);
  bf_point_t at = g->player.pos,
             next = {at.x + delta * vx / length, at.y + delta * vy / length};
  if (motion_allowed(g, next))
    g->player.pos = next;
  else if (!pointer) {
    bf_point_t x = {next.x, at.y}, y = {at.x, next.y};
    if (motion_allowed(g, x))
      g->player.pos = x;
    else if (motion_allowed(g, y))
      g->player.pos = y;
  }
  float moved = distance(g->player.pos, at);
  g->player.walking = moved > .03f;
  g->player.stride += moved * .076f;
  if (pointer && distance_sq(g->player.pos, g->route[g->route_at]) < .16f)
    g->route_at++;
}
void bf_step(bf_game_t* g, int dt_ms, int dx, int dy) {
  if (dt_ms <= 0) return;
  g->ambient_ms = (g->ambient_ms + dt_ms % 360000) % 360000;
  if (g->phase != BF_DINING) {
    if (g->phase != BF_TICKET && g->receipt_ms < 650)
      g->receipt_ms += lower(dt_ms, 650);
    return;
  }
  int left = bf_rules.duration_ms - g->elapsed_ms;
  int dt = lower(dt_ms, left), start = g->elapsed_ms;
  int excess = g->fullness_milli - bf_rules.red_line * 1000;
  g->recovered_ms = down_timer(g->recovered_ms, dt);
  if (excess > 0) {
    int to_safe = (excess + bf_rules.digest_milli_per_ms - 1) /
                  bf_rules.digest_milli_per_ms;
    int to_fee = bf_rules.tolerance_ms - g->red_ms;
    /* Both ties belong to safety: the stomach must still be beyond the line
     * and the room must still be open at the instant of admission. */
    if (to_fee <= dt && to_fee < to_safe && to_fee < left) {
      dt = to_fee;
      g->phase = BF_HOSPITAL;
      g->purse -= bf_rules.fee;
    }
    if (dt >= to_safe) g->recovered_ms = 2200;
    g->red_ms = dt >= to_safe ? 0 : g->red_ms + dt;
  } else
    g->red_ms = 0;
  g->elapsed_ms += dt;
  g->fullness_milli =
      down_timer(g->fullness_milli, dt * bf_rules.digest_milli_per_ms);
  for (int i = 0; i < BF_SLOTS; i++)
    if (g->empty[i] && floorf(slot_lap(start, i) - .18f) !=
                           floorf(slot_lap(g->elapsed_ms, i) - .18f)) {
      g->empty[i] = false;
      g->returned++;
    }
  g->bite_ms = down_timer(g->bite_ms, dt);
  g->turn_ms = down_timer(g->turn_ms, dt);
  g->feedback_ms = down_timer(g->feedback_ms, dt);
  if (g->elapsed_ms == bf_rules.duration_ms) g->phase = BF_CLOSING;
  for (int i = 0; i < 2; i++) diner(&g->diners[i], g->elapsed_ms, i);
  if (g->phase == BF_DINING) walk(g, lower(dt, 100), dx, dy);
}
