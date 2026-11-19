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
                             .digest_milli_per_ms = 2};
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
                bf_ticket_button = {502, 356, 237, 54};
const bf_rect_t bf_album_button = {601, 9, 96, 38},
                bf_round_cards = {24, 322, 419, 107},
                bf_album_back = {663, 26, 105, 48},
                bf_album_tab = {264, 27, 255, 41};

static int min_int(int a, int b) { return a < b ? a : b; }
static int countdown(int value, int elapsed) {
  return value > elapsed ? value - elapsed : 0;
}
bool bf_in(bf_rect_t r, float x, float y) {
  return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}
bool bf_has_goal(const bf_game_t* g) {
  return g->tasting ? g->varieties == BF_DISHES : g->value >= bf_rules.ticket;
}
int bf_card_count(unsigned mask) {
  int count = 0;
  for (int i = 0; i < BF_CARD_COUNT; i++)
    if (mask & (1u << i)) ++count;
  return count;
}
int bf_reveal_duration(const bf_game_t* g) {
  return bf_card_count(g->album.fresh) * BF_REVEAL_MS;
}
static int first_card(unsigned mask) {
  for (int i = 0; i < BF_CARD_COUNT; i++)
    if (mask & (1u << i)) return i;
  return 0;
}
void bf_award(bf_game_t* g) {
  unsigned earned = 0;
  if (g->phase == BF_HOSPITAL) earned = 4u;
  if (g->phase == BF_CLOSING) earned = g->eaten ? 3u : 2u;
  g->album.awaiting = earned & ~g->album.owned;
  g->album.owned |= earned;
}
void bf_confirm_cards(bf_album_t* album, unsigned new_in_store) {
  unsigned fresh = album->awaiting & new_in_store;
  album->awaiting = 0;
  if (!fresh) return;
  album->fresh = fresh;
  album->selected = first_card(fresh);
  album->reveal_ms = (album->reduced_motion || album->open)
                         ? bf_card_count(fresh) * BF_REVEAL_MS
                         : 0;
}
static void open_album(bf_game_t* g, bool this_meal) {
  g->album.open = true;
  g->album.this_meal = this_meal;
  g->album.selected = first_card(this_meal ? g->album.fresh : g->album.owned);
  g->album.reveal_ms = bf_reveal_duration(g);
}
unsigned bf_visible_cards(const bf_game_t* g) {
  return g->album.this_meal ? g->album.fresh : (3u | g->album.owned);
}
int bf_card_position(const bf_game_t* g, int i) {
  unsigned mask = bf_visible_cards(g);
  return 400 + bf_card_count(mask & ((1u << i) - 1)) * 222 -
         (bf_card_count(mask) - 1) * 111;
}
static void select_card(bf_game_t* g, int direction) {
  unsigned visible = bf_visible_cards(g);
  for (int i = 0; i < BF_CARD_COUNT; i++) {
    g->album.selected =
        (g->album.selected + BF_CARD_COUNT + direction) % BF_CARD_COUNT;
    if (visible & (1u << g->album.selected)) return;
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
  bool en = g->en, tasting = g->tasting, muted = g->muted;
  int cover = g->cover;
  bf_album_t album = g->album;
  bf_init(g, g->keyboard);
  g->album.owned = album.owned;
  g->album.reduced_motion = album.reduced_motion;
  g->album.unsaved = album.unsaved;
  g->en = en;
  g->tasting = tasting;
  g->muted = muted;
  g->cover = cover;
  g->phase = BF_DINING;
  g->purse -= bf_rules.ticket;
  g->player = (bf_actor_t){.pos = {400, 378}, .face = BF_DOWN};
  bf_service_open(g);
}
static void eat(bf_game_t* g, int slot) {
  if (g->phase != BF_DINING || g->bite_ms || !bf_can_take(g, slot)) return;
  int dish = g->settings[slot].dish;
  const bf_dish_t* serving = &bf_menu[dish];
  g->bite_source = bf_place(g, slot).pos;
  g->player.face =
      bf_facing(g->bite_source.x - g->player.pos.x,
                g->bite_source.y + BF_TABLE_RISE - g->player.pos.y);
  bf_service_clear(g, slot);
  if (g->bites[dish]++ == 0) g->varieties++;
  g->value += serving->coins;
  const int line = bf_rules.red_line * 1000;
  bool was_safe = g->fullness_milli <= line;
  g->fullness_milli += serving->fullness * 1000;
  if (was_safe && g->fullness_milli > line) g->red_entries++;
  g->eaten++;
  g->last_dish = dish;
  g->route_at = g->route_len;
  g->player.walking = false;
  g->bite_ms = 680;
  g->feedback_ms = 850;
}
void bf_action(bf_game_t* g, unsigned actions) {
  if (actions & H2_BUFFET_SOUND) g->muted = !g->muted;
  if (g->album.open) {
    if (actions & (H2_BUFFET_CONFIRM | H2_BUFFET_ALBUM)) g->album.open = false;
    if (actions & H2_BUFFET_NEXT) select_card(g, 1);
    if (actions & H2_BUFFET_PREVIOUS) select_card(g, -1);
    if ((actions & H2_BUFFET_GOAL) && g->phase != BF_TICKET)
      open_album(g, !g->album.this_meal);
    return;
  }
  if ((actions & H2_BUFFET_ALBUM) && g->phase != BF_DINING) {
    open_album(g, g->phase != BF_TICKET);
    return;
  }
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
    else if (g->album.reveal_ms < bf_reveal_duration(g))
      g->album.reveal_ms = bf_reveal_duration(g);
    else if (g->receipt_ms >= 650) {
      g->phase = BF_TICKET;
      g->album.awaiting = 0;
      g->purse = bf_rules.purse;
    }
  }
}
bf_rect_t bf_sound_bounds(const bf_game_t* g) {
  return g->phase == BF_DINING ? (bf_rect_t){745, 5, 45, 42}
                               : (bf_rect_t){523, 9, 74, 38};
}
bf_rect_t bf_eat_bounds(const bf_game_t* g, int slot) {
  (void)g;
  bf_rect_t table = bf_table_footprint(slot / BF_SETTINGS);
  return (bf_rect_t){table.x + table.w - 58, table.y + table.h - 22, 64, 44};
}
static void dining_tap(bf_game_t* g, float x, float y) {
  int near = bf_nearest(g);
  if (near >= 0 && bf_in(bf_eat_bounds(g, near), x, y)) {
    eat(g, near);
    return;
  }
  for (int i = 0; i < BF_SLOTS; i++) {
    if (g->settings[i].dish < 0) continue;
    bf_place_t p = bf_place(g, i);
    float dx = (x - p.pos.x) / 21.f, dy = (y - p.pos.y + 4) / 16.f;
    if (dx * dx + dy * dy <= 1) {
      if (bf_can_take(g, i))
        eat(g, i);
      else
        bf_walk_to(g, p.station);
      return;
    }
  }
  /* A tap on an empty setting still walks to that portion of the table. */
  for (int i = 0; i < BF_TABLES; i++) {
    bf_rect_t top = bf_table_footprint(i);
    top.y -= BF_TABLE_RISE;
    top.h += BF_TABLE_RISE;
    if (bf_in(top, x, y)) {
      int index = (int)((x - top.x) * 3 / top.w);
      bf_walk_to(g, bf_place(g, i * BF_SETTINGS + index).station);
      return;
    }
  }
  if (y < BF_FLOOR_Y || y > BF_FLOOR_Y + BF_ROWS * BF_TILE_H) return;
  bf_walk_to(g, (bf_point_t){x, y});
}
void bf_tap(bf_game_t* g, float x, float y) {
  if (bf_in(bf_sound_bounds(g), x, y)) {
    bf_action(g, H2_BUFFET_SOUND);
    return;
  }
  if (g->album.open) {
    if (bf_in(bf_album_back, x, y))
      g->album.open = false;
    else if (bf_in(bf_album_tab, x, y) && g->phase != BF_TICKET)
      open_album(g, !g->album.this_meal);
    else if (y >= 102 && y <= 332) {
      for (int i = 0; i < BF_CARD_COUNT; i++)
        if ((bf_visible_cards(g) & (1u << i)) &&
            fabsf(x - bf_card_position(g, i)) < 98)
          g->album.selected = i;
    }
    return;
  }
  if (g->phase == BF_TICKET) {
    if (bf_in(bf_album_button, x, y))
      open_album(g, false);
    else if (bf_in(bf_language_button, x, y))
      bf_action(g, H2_BUFFET_LANGUAGE);
    else if (bf_in(bf_goal_button, x, y))
      bf_action(g, H2_BUFFET_GOAL);
    else if (bf_in(bf_ticket_button, x, y))
      bf_action(g, H2_BUFFET_CONFIRM);
    else if (bf_in((bf_rect_t){24, 389, 420, 65}, x, y))
      g->cover = (int)(x - 24) / 70;
  } else if (g->phase == BF_DINING)
    dining_tap(g, x, y);
  else if (bf_in(bf_round_cards, x, y))
    open_album(g, true);
  else if (bf_in(bf_album_button, x, y))
    open_album(g, false);
  else if (g->album.reveal_ms < bf_reveal_duration(g))
    g->album.reveal_ms = bf_reveal_duration(g);
  else if (bf_in(bf_ticket_button, x, y))
    bf_action(g, H2_BUFFET_CONFIRM);
}
void bf_step(bf_game_t* g, int dt_ms, int dx, int dy) {
  if (dt_ms <= 0) return;
  g->ambient_ms = (g->ambient_ms + dt_ms % 360000) % 360000;
  if (g->phase != BF_DINING) {
    if (g->phase != BF_TICKET && g->album.reveal_ms < bf_reveal_duration(g))
      g->album.reveal_ms +=
          min_int(dt_ms, bf_reveal_duration(g) - g->album.reveal_ms);
    if (g->phase != BF_TICKET && g->receipt_ms < 650)
      g->receipt_ms += min_int(dt_ms, 650);
    return;
  }
  int remaining = bf_rules.duration_ms - g->elapsed_ms,
      dt = min_int(dt_ms, remaining);
  int excess = g->fullness_milli - bf_rules.red_line * 1000;
  g->recovered_ms = countdown(g->recovered_ms, dt);
  if (excess > 0) {
    int to_safe = (excess + bf_rules.digest_milli_per_ms - 1) /
                  bf_rules.digest_milli_per_ms;
    int to_fee = bf_rules.tolerance_ms - g->red_ms;
    if (to_fee <= dt && to_fee < to_safe && to_fee < remaining) {
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
      countdown(g->fullness_milli, dt * bf_rules.digest_milli_per_ms);
  g->bite_ms = countdown(g->bite_ms, dt);
  g->turn_ms = countdown(g->turn_ms, dt);
  g->feedback_ms = countdown(g->feedback_ms, dt);
  float gap = g->fullness_milli - g->gauge_milli;
  if (gap > 0)
    g->gauge_milli += gap * fminf(1.f, (float)dt / 90.f);
  else
    g->gauge_milli = (float)g->fullness_milli;
  if (g->elapsed_ms == bf_rules.duration_ms) g->phase = BF_CLOSING;
  if (g->phase != BF_DINING)
    bf_award(g);
  else {
    bf_service_update(g);
    bf_walk(g, min_int(dt, 100), dx, dy);
  }
}
