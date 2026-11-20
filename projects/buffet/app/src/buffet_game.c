#include "buffet_rotary.h"

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
     23,
     .fresh_seconds = 28},
    {"sushi-tamago",
     {"厚烧玉子握", "Tamagoyaki Nigiri"},
     {"层层蛋卷，束一圈海苔。", "Folded egg, tied with a strip of nori."},
     12,
     20,
     .fresh_seconds = 42},
    {"sushi-tuna",
     {"金枪鱼握", "Tuna Nigiri"},
     {"薄切鱼肉上有浅色纹理。", "Fine pale seams in a deep red cut."},
     17,
     18,
     .fresh_seconds = 24},
    {"sushi-shrimp",
     {"熟虾握", "Cooked Shrimp Nigiri"},
     {"弯弯虾身，尾尖留红。", "A curled prawn with a coral tail."},
     15,
     17,
     .fresh_seconds = 32},
    {"sushi-cucumber-roll",
     {"黄瓜细卷", "Cucumber Maki"},
     {"海苔卷着脆嫩瓜心。", "Cool cucumber inside a crisp nori wrap."},
     8,
     10,
     .fresh_seconds = 38},
    {"sushi-inari",
     {"豆皮寿司", "Inari Sushi"},
     {"豆皮袋里点上几粒芝麻。", "A sweet tofu pocket finished with sesame."},
     11,
     25,
     .fresh_seconds = 45},
    {"sushi-shoyu-ramen",
     {"酱油拉面", "Shoyu Ramen"},
     {"琥珀色汤底，细面配一片叉烧。",
      "Thin noodles and chashu in amber soy broth."},
     24,
     32,
     .fresh_seconds = 26},
    {"sushi-chawanmushi",
     {"茶碗蒸", "Chawanmushi"},
     {"揭开小盅，蛋羹里透出高汤香。",
      "A small cup of silky, dashi-steamed egg."},
     9,
     9,
     .fresh_seconds = 34},
};
const bf_rect_t bf_language_button = {710, 12, 74, 34},
                bf_goal_button = {502, 224, 237, 42},
                bf_ticket_button = {502, 356, 237, 54};
const bf_rect_t bf_album_button = {601, 9, 96, 38},
                bf_round_cards = {24, 322, 419, 107},
                bf_album_back = {663, 26, 105, 48},
                bf_album_tab = {264, 27, 255, 41},
                bf_inspect_back = {640, 12, 132, 74};
const bf_rect_t bf_menu_button = {550, 0, 80, 74},
                bf_pause_left = {222, 320, 170, 74},
                bf_pause_right = {408, 320, 170, 74};
const bf_rect_t bf_stick_bounds = {
    BF_STICK_X - BF_STICK_HIT_X, BF_CONTROL_Y - BF_STICK_HIT_UP,
    2 * BF_STICK_HIT_X, BF_STICK_HIT_UP + BF_STICK_HIT_DOWN};
const bf_rect_t bf_eat_bounds = {BF_EAT_X - BF_EAT_HIT_RADIUS,
                                 BF_CONTROL_Y - BF_EAT_HIT_RADIUS,
                                 2 * BF_EAT_HIT_RADIUS, 2 * BF_EAT_HIT_RADIUS};

static int min_int(int a, int b) { return a < b ? a : b; }
static int countdown(int value, int elapsed) {
  return value > elapsed ? value - elapsed : 0;
}
bool bf_in(bf_rect_t r, float x, float y) {
  return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}
bool bf_has_goal(const bf_game_t* g) {
  if (!g->tasting) return g->value >= bf_rules.ticket;
  for (int d = 0; d < BF_SUSHI; d++)
    if (!g->bites[d]) return false;
  return true;
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
int bf_card_at(unsigned mask, int slot) {
  for (int i = 0; i < BF_CARD_COUNT; i++)
    if ((mask & (1u << i)) && slot-- == 0) return i;
  return -1;
}
bf_rect_t bf_rack_card(int slot) {
  return (bf_rect_t){30 + slot * 74, 348, 62, 74};
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
  if ((!album->open || album->this_meal) && !album->inspecting)
    album->selected = bf_card_at(fresh, 0);
  album->reveal_ms = (album->reduced_motion || album->open)
                         ? bf_card_count(fresh) * BF_REVEAL_MS
                         : 0;
}
static void open_album(bf_game_t* g, bool this_meal) {
  g->album.open = true;
  g->album.this_meal = this_meal;
  g->album.selected =
      bf_card_at(this_meal ? g->album.fresh : (3u | g->album.owned), 0);
  g->album.reveal_ms = bf_reveal_duration(g);
}
static bool inspect_card(bf_game_t* g, int id, int rack_slot) {
  bf_album_t* a = &g->album;
  unsigned mask = a->this_meal || rack_slot >= 0 ? a->fresh : a->owned;
  if (id < 0 || id >= BF_CARD_COUNT || !(mask & (1u << id))) return false;
  a->selected = id;
  a->inspecting = true;
  a->inspect_receipt = rack_slot >= 0;
  a->inspect_slot = rack_slot;
  a->inspect_ms = a->reduced_motion ? BF_INSPECT_MS : 0;
  return true;
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
  if (!visible) return;
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
  g->player = (bf_actor_t){.pos = bf_project((bf_world_t){10, 10.9f}, 0),
                           .face = BF_DOWN};
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
  g->player.walking = false;
  g->bite_ms = 680;
  g->feedback_ms = 850;
}
int bf_eat_target(const bf_game_t* g) {
  return g->phase == BF_DINING && g->meal_menu == BF_LIVE && !g->bite_ms
             ? bf_nearest(g)
             : -1;
}

static void set_meal_menu(bf_game_t* g, bf_meal_menu_t menu) {
  bf_reset_controls(g);
  g->meal_menu = menu;
  g->leave_selected = false;
  g->release_axes = true;
}

static void choose_meal_action(bf_game_t* g, bool leave) {
  if (g->meal_menu == BF_MEAL_STATUS) {
    set_meal_menu(g, leave ? BF_CONFIRM_LEAVE : BF_LIVE);
  } else if (!leave) {
    set_meal_menu(g, BF_MEAL_STATUS);
  } else {
    /* Each admission has its own purse. Keep this one's actual remainder on
     * the ticket counter until the player explicitly buys another meal. */
    set_meal_menu(g, BF_LIVE);
    g->phase = BF_TICKET;
    g->left_early = true;
  }
}

void bf_action(bf_game_t* g, unsigned actions) {
  if (g->phase == BF_DINING && (actions & H2_BUFFET_MENU)) {
    switch (g->meal_menu) {
      case BF_LIVE:
        set_meal_menu(g, BF_MEAL_STATUS);
        break;
      case BF_MEAL_STATUS:
        set_meal_menu(g, BF_LIVE);
        break;
      case BF_CONFIRM_LEAVE:
        set_meal_menu(g, BF_MEAL_STATUS);
        break;
    }
    return;
  }
  if (g->meal_menu != BF_LIVE) {
    /* Leave moves to the opposite side on confirmation. Repeating the
     * original tap returns to the status sheet. The initial key choice is safe.
     */
    if (actions & H2_BUFFET_PREVIOUS)
      g->leave_selected = g->meal_menu == BF_MEAL_STATUS;
    if (actions & H2_BUFFET_NEXT)
      g->leave_selected = g->meal_menu == BF_CONFIRM_LEAVE;
    if (actions & H2_BUFFET_CONFIRM) choose_meal_action(g, g->leave_selected);
    return;
  }
  if (actions & H2_BUFFET_SOUND) g->muted = !g->muted;
  if (g->album.inspecting) {
    if (actions & (H2_BUFFET_CONFIRM | H2_BUFFET_ALBUM))
      g->album.inspecting = false;
    return;
  }
  if (g->album.open) {
    if (actions & H2_BUFFET_ALBUM) {
      g->album.open = false;
      return;
    }
    if (actions & H2_BUFFET_CONFIRM) {
      if (!inspect_card(g, g->album.selected, -1)) g->album.open = false;
      return;
    }
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
      eat(g, bf_eat_target(g));
    else if (g->album.reveal_ms < bf_reveal_duration(g))
      g->album.reveal_ms = bf_reveal_duration(g);
    else if (g->receipt_ms >= 650) {
      g->phase = BF_TICKET;
      bf_reset_controls(g);
      g->album.awaiting = 0;
      g->purse = bf_rules.purse;
    }
  }
}
bf_rect_t bf_sound_bounds(const bf_game_t* g) {
  return g->phase == BF_DINING ? (bf_rect_t){735, 2, 61, 55}
                               : (bf_rect_t){523, 9, 74, 38};
}
void bf_tap(bf_game_t* g, float x, float y) {
  if (g->meal_menu != BF_LIVE) {
    if (bf_in(bf_pause_left, x, y))
      choose_meal_action(g, g->meal_menu == BF_MEAL_STATUS);
    else if (bf_in(bf_pause_right, x, y))
      choose_meal_action(g, g->meal_menu == BF_CONFIRM_LEAVE);
    return;
  }
  if (g->phase == BF_DINING && bf_in(bf_menu_button, x, y)) {
    bf_action(g, H2_BUFFET_MENU);
    return;
  }
  if (g->album.inspecting) {
    if (bf_in(bf_inspect_back, x, y)) g->album.inspecting = false;
    return;
  }
  if (bf_in(bf_sound_bounds(g), x, y)) {
    bf_action(g, H2_BUFFET_SOUND);
    return;
  }
  if (g->album.open) {
    if (bf_in(bf_album_back, x, y))
      g->album.open = false;
    else if (bf_in(bf_album_tab, x, y) && g->phase != BF_TICKET)
      open_album(g, !g->album.this_meal);
    else if (y >= 102 && y <= 316) {
      for (int i = 0; i < BF_CARD_COUNT; i++)
        if ((bf_visible_cards(g) & (1u << i)) &&
            fabsf(x - bf_card_position(g, i)) < 98) {
          g->album.selected = i;
          inspect_card(g, i, -1);
          break;
        }
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
    else if (bf_in((bf_rect_t){BF_MENU_X, 389, BF_DISHES * BF_MENU_PITCH, 65},
                   x, y))
      g->cover = (int)(x - BF_MENU_X) / BF_MENU_PITCH;
  } else if (g->phase != BF_DINING) {
    if (bf_in(bf_round_cards, x, y)) {
      if (g->album.reveal_ms >= bf_reveal_duration(g)) {
        for (int i = 0; i < bf_card_count(g->album.fresh); i++)
          if (bf_in(bf_rack_card(i), x, y) &&
              inspect_card(g, bf_card_at(g->album.fresh, i), i))
            return;
      }
      open_album(g, true);
    } else if (bf_in(bf_album_button, x, y))
      open_album(g, false);
    else if (bf_in(bf_ticket_button, x, y))
      bf_action(g, H2_BUFFET_CONFIRM);
  }
}
void bf_reset_controls(bf_game_t* g) {
  g->controls = (bf_controls_t){0};
  g->player.walking = false;
}
void bf_pointer(bf_game_t* g, h2_pal_touch_event_kind_t kind, uint32_t id,
                float x, float y) {
  bf_controls_t* c = &g->controls;
  if (kind == H2_PAL_TOUCH_EVENT_CANCEL && id == H2_PAL_TOUCH_ALL_POINTERS) {
    bf_reset_controls(g);
    return;
  }
  bool stick_owner = c->stick_held && c->stick_id == id;
  bool press_owner = c->press_held && c->press_id == id;
  if (kind == H2_PAL_TOUCH_EVENT_DOWN) {
    if (stick_owner || press_owner) return;
    bool dining = g->phase == BF_DINING && g->meal_menu == BF_LIVE;
    if (g->meal_menu != BF_LIVE && !bf_in(bf_pause_left, x, y) &&
        !bf_in(bf_pause_right, x, y))
      return;
    if (dining && bf_in(bf_stick_bounds, x, y)) {
      if (c->stick_held) return;
      c->stick_held = true;
      c->stick_id = id;
      c->stick_origin = (bf_point_t){x, y};
      return;
    }
    bool is_eat = dining && bf_in(bf_eat_bounds, x, y);
    if (c->press_held || (dining && !is_eat && !bf_in(bf_menu_button, x, y) &&
                          !bf_in(bf_sound_bounds(g), x, y)))
      return;
    c->press_held = true;
    c->press_id = id;
    c->press_origin = (bf_point_t){x, y};
    c->press_tap = !is_eat;
    c->eat_held = is_eat && bf_eat_target(g) >= 0;
    if (c->eat_held) bf_action(g, H2_BUFFET_CONFIRM);
  } else if (stick_owner) {
    if (kind == H2_PAL_TOUCH_EVENT_MOVE) {
      float dx = x - c->stick_origin.x, dy = y - c->stick_origin.y;
      float travel = hypotf(dx, dy), deadzone = 7.f;
      float gain =
          travel > deadzone
              ? fminf(1.f, (travel - deadzone) / (BF_STICK_TRAVEL - deadzone)) /
                    travel
              : 0;
      c->stick = (bf_point_t){dx * gain, dy * gain};
    } else if (kind == H2_PAL_TOUCH_EVENT_UP ||
               kind == H2_PAL_TOUCH_EVENT_CANCEL) {
      c->stick_held = false;
      c->stick = (bf_point_t){0};
      g->player.walking = false;
    }
  } else if (press_owner) {
    float dx = x - c->press_origin.x, dy = y - c->press_origin.y;
    if (dx * dx + dy * dy > 14.f * 14.f) c->press_tap = false;
    if (kind == H2_PAL_TOUCH_EVENT_UP || kind == H2_PAL_TOUCH_EVENT_CANCEL) {
      bool tap = c->press_tap && kind == H2_PAL_TOUCH_EVENT_UP;
      c->press_held = c->press_tap = c->eat_held = false;
      if (tap) bf_tap(g, x, y);
    }
  }
}
void bf_step(bf_game_t* g, int dt_ms, int dx, int dy) {
  if (dt_ms <= 0 || g->meal_menu != BF_LIVE) return;
  if (g->release_axes) {
    g->release_axes = dx != 0 || dy != 0;
    dx = dy = 0;
  }
  g->ambient_ms = (g->ambient_ms + dt_ms % 360000) % 360000;
  if (g->phase != BF_DINING) {
    if (g->album.inspecting)
      g->album.inspect_ms +=
          min_int(dt_ms, BF_INSPECT_MS - g->album.inspect_ms);
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
  if (g->phase != BF_DINING) {
    bf_reset_controls(g);
    bf_award(g);
  } else {
    bf_service_update(g);
    if (g->controls.stick_held)
      bf_walk(g, min_int(dt, 100), g->controls.stick.x, g->controls.stick.y);
    else
      bf_walk(g, min_int(dt, 100), (float)dx, (float)dy);
  }
}
