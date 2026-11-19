#include "buffet_render.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define C(x) BF_COLOR(x)
#define T(zh, en_text) (c.en ? (en_text) : (zh))
static const uint16_t ink = C(0x273d3c), paper = C(0xf3e3bf),
                      light = C(0xf6ead0), sea = C(0x294c4b),
                      patina = C(0xaacaba), clay = C(0xa94f38),
                      mute = C(0x536457);
static const bf_paint_t* const detail[] = {&bf_salmon, &bf_tamago, &bf_tuna,
                                           &bf_shrimp, &bf_maki,   &bf_inari};
static const bf_paint_t* const small[] = {&bf_salmon_belt, &bf_tamago_belt,
                                          &bf_tuna_belt,   &bf_shrimp_belt,
                                          &bf_maki_belt,   &bf_inari_belt};
static const bf_paint_t* const clear[] = {
    &bf_salmon_cleared, &bf_tamago_cleared, &bf_tuna_cleared,
    &bf_shrimp_cleared, &bf_maki_cleared,   &bf_inari_cleared};
static const bf_paint_t* const bites[] = {&bf_salmon_bite, &bf_tamago_bite,
                                          &bf_tuna_bite,   &bf_shrimp_bite,
                                          &bf_maki_bite,   &bf_inari_bite};
static const bf_sprite_t* const prints[] = {&bf_card_bite, &bf_card_close,
                                            &bf_card_rest};
static const bf_text_t card_names[] = {
    {"第一口", "First Bite"},
    {"顺利吃到打烊", "Made It to Closing"},
    {"第一次进病房", "First Trip to the Hospital"}};
static const bf_text_t card_notes[] = {
    {"在带边吃一份菜，顺利吃到打烊。",
     "Eat a serving from the belt, then stay until closing."},
    {"凭票入座，等到餐厅收盘。",
     "Stay for a paid meal until the restaurant closes."},
    {"首次因持续超过危险线而住院。",
     "Be admitted after staying over the red line."}};
static const char* local(bf_canvas_t c, bf_text_t t) {
  return c.en ? t.en : t.zh;
}
static void right(bf_canvas_t c, bf_size_t size, int x, int y, uint16_t color,
                  const char* s) {
  bf_text(c, size, x - bf_width(c.en, size, s), y, color, s);
}
static void slab(bf_canvas_t c, bf_rect_t b, uint16_t color) {
  bf_box(c, b.x + 3, b.y, b.w - 6, b.h, color);
  bf_box(c, b.x, b.y + 3, b.w, b.h - 6, color);
}
static void btn(bf_canvas_t c, bf_rect_t b, uint16_t bg, const char* s) {
  slab(c, b, bg);
  bf_center(c, BF_BODY, b.x + b.w / 2, b.y + b.h / 2 + 6, light, s);
}
static void dotted(bf_canvas_t c, int x, int end, int y, uint16_t color) {
  for (; x < end; x += 7) bf_box(c, x, y, 3, 1, color);
}
static void clock_string(char* s, size_t n, int ms) {
  int t = (ms + 999) / 1000;
  snprintf(s, n, "%d:%02d", t / 60, t % 60);
}
static const bf_rect_t island = {136, 168, 528, 140};
void bf_paint_ground(bf_canvas_t c, bool dining) {
  if (dining) {
    memcpy(c.pixels, bf_floor_pixels, sizeof(bf_floor_pixels));
    bf_layer(c, &bf_mat, (bf_rect_t){299, 412, 202, 62}, false);
    bf_layer(c, &bf_counter_shade, island, false);
  } else {
    bf_image(c, &bf_cloth, 0, 0, 800, 480);
  }
}
static void ticket(bf_canvas_t c) {
  bf_box(c, 482, 59, 280, 376, C(0x244c4b));
  bf_box(c, 479, 56, 280, 376, paper);
  bf_box(c, 485, 64, 1, 357, C(0xd5b992));
  for (int x = 484; x < 754; x += 12) {
    bf_line(c, x, 431, x + 3, 428, C(0x42625a));
    bf_line(c, x + 3, 428, x + 6, 431, C(0x42625a));
  }
  for (int i = 0; i < 2; i++) bf_ellipse(c, i ? 759 : 479, 326, 6, 6, sea, 255);
  dotted(c, 494, 747, 326, C(0xac9474));
}
static void stack_mark(bf_canvas_t c, int x, int y) {
  bf_box(c, x + 2, y, 13, 15, C(0xbaa783));
  bf_box(c, x, y + 2, 13, 16, C(0xf0dbad));
  bf_box(c, x + 2, y + 4, 9, 12, C(0x547875));
  bf_line(c, x + 4, y + 11, x + 7, y + 8, light);
  bf_line(c, x + 7, y + 8, x + 9, y + 10, light);
}
static void house(bf_canvas_t c) {
  bf_image(c, &bf_mark, 26, 16, 42, 25);
  bf_text(c, BF_TITLE, 78, 42, light, T("潮轮寿司", "Tidewheel Sushi"));
}
static void album_entry(bf_canvas_t c) {
  stack_mark(c, 610, 21);
  bf_text(c, BF_SMALL, 632, 34, light, T("成就卡册", "Card album"));
}
static void foyer(bf_canvas_t c, const bf_game_t* g) {
  char s[96];
  int d = g->cover;
  house(c);
  album_entry(c);
  btn(c, bf_language_button, sea, T("EN", "中文"));
  for (int i = 6; i > 0; i--)
    bf_ellipse(c, 250, 228, 126 + 4 * i, 47 + 2 * i, C(0x102e32), 13);
  bf_layer(c, detail[d], (bf_rect_t){57, 68, 375, 244}, false);
  bf_text(c, BF_HEAD, 34, 335, light, local(c, bf_menu[d].name));
  bf_text(c, BF_SMALL, 35, 359, light, local(c, bf_menu[d].detail));
  snprintf(s, sizeof(s),
           T("价值 %d 币    饱腹 +%d%%", "%d coins    +%d%% fullness"),
           bf_menu[d].coins, bf_menu[d].fullness);
  bf_text(c, BF_SMALL, 35, 383, patina, s);
  for (int i = 0; i < 6; i++) {
    int x = 24 + i * 70;
    bf_layer(c, small[i], (bf_rect_t){x + 2, 397, 66, 43}, false);
    if (i == d) bf_box(c, x + 10, 446, 47, 2, C(0xefc589));
  }
  ticket(c);
  bf_text(c, BF_HEAD, 502, 102, ink, T("用餐券", "Meal ticket"));
  bf_line(c, 501, 114, 737, 114, C(0xbba882));
  bf_text(c, BF_SMALL, 502, 148, mute, T("用餐时间", "Dining time"));
  clock_string(s, sizeof(s), bf_rules.duration_ms);
  right(c, BF_BODY, 737, 149, ink, s);
  bf_text(c, BF_SMALL, 502, 179, mute, T("随身餐费", "Meal purse"));
  snprintf(s, sizeof(s), T("%d 币", "%d coins"), bf_rules.purse);
  right(c, BF_BODY, 737, 179, ink, s);
  bf_text(c, BF_SMALL, 502, 215, mute, T("这顿想试试", "Meal goal"));
  slab(c, bf_goal_button, C(0xdac9a7));
  bf_center(c, BF_BODY, 620, 250, ink,
            g->tasting ? T("尝遍六种", "Taste all six")
                       : T("吃回票价", "Cover the ticket"));
  bf_line(c, 718, 243, 722, 247, mute);
  bf_line(c, 722, 247, 726, 243, mute);
  bf_text(c, BF_SMALL, 502, 288, mute,
          g->keyboard ? T("点地面或方向键行走，点吃或按空格。",
                          "Walk by tap or arrow keys. Eat with Space.")
                      : T("点地面行走，在带边点吃。",
                          "Tap the floor to walk. Eat at the belt."));
  bf_text(c, BF_SMALL, 502, 311, mute,
          T("米碗白线是吃饱，尖格亮起就歇口。",
            "The white notch is full. Rest at the points."));
  snprintf(s, sizeof(s), T("买票入座   %d 币", "Buy ticket   %d coins"),
           bf_rules.ticket);
  btn(c, bf_ticket_button, clay, s);
}
static void hud(bf_canvas_t c, const bf_game_t* g) {
  char s[64];
  bf_rect_t tag = {10, 5, 129, 25};
  slab(c, tag, C(0x4e5143));
  bf_ellipse(c, 23, 17, 7, 7, C(0xd9bd87), 255);
  bf_ellipse(c, 23, 17, 5, 5, C(0x4e5143), 255);
  bf_line(c, 23, 13, 23, 17, light);
  bf_line(c, 23, 17, 26, 18, light);
  clock_string(s, sizeof(s), bf_rules.duration_ms - g->elapsed_ms);
  bf_text(c, BF_BODY, 36, 23, light, s);
  if (g->tasting) {
    bf_ellipse(c, 83, 17, 7, 4, C(0xcfddbd), 255);
    bf_ellipse(c, 83, 16, 5, 2, C(0x839e87), 255);
    snprintf(s, sizeof(s), "%d/6", g->varieties);
  } else {
    bf_ellipse(c, 83, 18, 6, 6, C(0xb18851), 255);
    bf_ellipse(c, 82, 16, 5, 5, C(0xf1ce85), 255);
    bf_box(c, 81, 15, 3, 3, C(0x93754d));
    snprintf(s, sizeof(s), "%d/%d", g->value, bf_rules.ticket);
  }
  bf_text(c, BF_SMALL, 94, 22, light, s);
  /* The bowl lip marks capacity, and the pointed reserve is danger. Fill
   * uses the same continuous value as digestion, with a short bite rise. */
  int gx = 637, gy = 2;
  bf_image(c, &bf_gauge, gx, gy, 154, 29);
  int fill = (int)(112 * fminf(1, g->gauge_milli / 160000.f));
  if (fill > 0) {
    bf_rect_t r = {72, 16, fill * 2, 28};
    bf_blit(c, &bf_gauge_full, r, (bf_rect_t){gx + 36, gy + 8, fill, 14});
  }
  if (g->bite_ms > 470) {
    int tip = gx + 36 + fill;
    bf_box(c, tip - 1, gy + 10, 1, 9, C(0xf7e4ae));
  }
  if (g->fullness_milli > bf_rules.red_line * 1000) {
    snprintf(s, sizeof(s),
             T("停口休息   %d 秒后送医", "Rest now   Hospital in %ds"),
             (bf_rules.tolerance_ms - g->red_ms + 999) / 1000);
    int w = bf_width(c.en, BF_SMALL, s) + 22;
    slab(c, (bf_rect_t){788 - w, 35, w, 23}, C(0x903e30));
    right(c, BF_SMALL, 777, 51, light, s);
  } else if (g->recovered_ms) {
    const char* msg = T("已经脱险", "All clear");
    int w = bf_width(c.en, BF_SMALL, msg) + 20;
    slab(c, (bf_rect_t){788 - w, 35, w, 22}, sea);
    right(c, BF_SMALL, 778, 50, light, msg);
  }
}
static void person(bf_canvas_t c, const bf_game_t* g, const bf_actor_t* a,
                   int kind) {
  int x = (int)lroundf(a->pos.x), y = (int)lroundf(a->pos.y);
  float size = .85f + fminf(.22f, fmaxf(0, (a->pos.y - 145.f) / 680.f));
  int w = (int)(68 * size), h = (int)(96 * size);
  int pose = a->walking ? 1 + (int)a->stride % 4
                        : (a->face == BF_DOWN &&
                                   (g->ambient_ms + kind * 680) % 4200 > 4050
                               ? 8
                               : 0);
  if (!kind && a->walking && g->turn_ms &&
      (a->face == BF_RIGHT || a->face == BF_LEFT))
    pose = 8;
  if (!kind && g->bite_ms)
    pose = g->bite_ms > 494 ? 5 : g->bite_ms > 220 ? 6 : 7;
  if (!kind && g->turn_ms) w -= (int)(4 * sinf(g->turn_ms * 3.14159f / 115.f));
  bf_face_t face = !kind && g->bite_ms && a->face == BF_UP ? BF_RIGHT : a->face;
  bf_ellipse(c, x + 3, y + 2, (int)(13 * size), (int)(4 * size), C(0x705640),
             63);
  bf_ellipse(c, x + 1, y + 1, (int)(8 * size), 2, C(0x51443a), 63);
  int breath = !a->walking && sinf(g->ambient_ms * .0034f + kind) > 0;
  bf_blit(c, &bf_cast,
          (bf_rect_t){pose * 136, (kind * 4 + (int)face) * 192, 136, 192},
          (bf_rect_t){x - w / 2, y - (int)(89 * size) + breath, w, h - breath});
  if (!kind && g->elapsed_ms < 3100 && !g->eaten) {
    int top = y - (int)(86 * size) - 7;
    bf_line(c, x - 3, top - 2, x, top + 1, C(0x733f31));
    bf_line(c, x, top + 1, x + 3, top - 2, C(0x733f31));
    bf_box(c, x - 2, top - 3, 5, 2, C(0xffdc8c));
  }
}
static void lit_rim(bf_canvas_t c, int x, int y, int width, int height) {
  const float rx = width * .47f, ry = height * .28f;
  int px = x + (int)rx, py = y;
  for (int i = 1; i <= 48; i++) {
    const float angle = i * .130899694f;
    int nx = x + (int)lroundf(rx * cosf(angle)),
        ny = y + (int)lroundf(ry * sinf(angle));
    bf_line(c, px, py + 1, nx, ny + 1, C(0x665540));
    bf_line(c, px, py, nx, ny, C(0xffe1a2));
    px = nx;
    py = ny;
  }
}
static void plates(bf_canvas_t c, const bf_game_t* g, int near, bool front) {
  for (int i = 0; i < BF_SLOTS; i++) {
    bf_place_t b = bf_place(g, i);
    if ((b.pos.y >= 225) != front) continue;
    float depth = (b.pos.y - 194) / 62.f;
    int x = (int)lroundf(b.pos.x), y = (int)lroundf(b.pos.y);
    int w = 43 + (int)lroundf(depth * 5), h = 28 + (int)lroundf(depth * 3);
    bf_rect_t bounds = {x - w / 2, y - (h * 300 + 208) / 416, w, h};
    bf_layer(c, &bf_plate_shade, bounds, false);
    if (i == near) lit_rim(c, x, y - h * 50 / 416, w, h);
    int d = bf_serving(g, i);
    bf_layer(c, g->empty[i] ? clear[d] : small[d], bounds, false);
    if (i == near) {
      int tip = y + 7 + (int)lroundf(sinf(g->ambient_ms * .006f));
      bf_line(c, x - 3, tip + 3, x, tip, C(0x5e4d38));
      bf_line(c, x, tip, x + 3, tip + 3, C(0x5e4d38));
      bf_line(c, x - 3, tip + 2, x, tip - 1, C(0xffdf94));
      bf_line(c, x, tip - 1, x + 3, tip + 2, C(0xffdf94));
    }
  }
}
static void service_tag(bf_canvas_t c, const bf_game_t* g, int near) {
  if (near < 0 && !g->bite_ms) return;
  int d = g->bite_ms ? g->last_dish : bf_serving(g, near);
  char s[64];
  const char* dish = local(c, bf_menu[d].name);
  int w = bf_width(c.en, BF_BODY, dish);
  if (w < 114) w = 114;
  int x = 685 - w - 53;
  slab(c, (bf_rect_t){x, 415, w + 51, 46}, C(0xf2e1b9));
  bf_layer(c, small[d], (bf_rect_t){x + 2, 423, 45, 30}, false);
  bf_text(c, BF_BODY, x + 48, 434, ink, dish);
  snprintf(s, sizeof(s), T("%d 币   饱腹 +%d%%", "%d coins   +%d%% full"),
           bf_menu[d].coins, bf_menu[d].fullness);
  bf_text(c, BF_SMALL, x + 49, 453, mute, s);
  btn(c, bf_eat_button, g->bite_ms ? C(0x62746a) : clay,
      g->bite_ms ? T("品尝中", "Tasting") : T("吃这盘", "Eat plate"));
}
static void pick_up(bf_canvas_t c, const bf_game_t* g) {
  if (!g->bite_ms || g->last_dish < 0) return;
  float age = (float)(680 - g->bite_ms);
  if (age > 420) return;
  /* The plate starts its empty return immediately. Its food follows the
   * player's palm, contacts the mouth on the audible bite frame, and settles.
   */
  float t = fminf(1.f, age / 185.f), travel = t * t * (3.f - 2.f * t);
  float size = .85f + fminf(.22f, fmaxf(0, (g->player.pos.y - 145.f) / 680.f));
  bool left = g->player.face == BF_LEFT;
  float hand_x = g->player.pos.x + (left ? -9.5f : 9.5f) * size;
  float hand_y = g->player.pos.y - 55.8f * size;
  float x = g->bite_source.x + (hand_x - g->bite_source.x) * travel;
  float y = g->bite_source.y - 8 + (hand_y - g->bite_source.y + 8) * travel -
            11 * sinf(t * 3.14159f);
  float settle = fminf(1.f, fmaxf(0, (age - 340) / 80));
  int w = (int)lroundf((26 - 14 * travel) * (1 - .65f * settle));
  int h = (int)lroundf((14 - 6.5f * travel) * (1 - .65f * settle));
  bf_layer(c, bites[g->last_dish],
           (bf_rect_t){(int)lroundf(x) - w / 2, (int)lroundf(y) - h / 2, w, h},
           left);
}

typedef struct {
  int foot;
  const bf_paint_t* paint;
  bf_rect_t bounds;
  bool mirror;
} furniture_t;
/* The service island occupies its own depth position between both aisles.
 * Its top is composed in the same order a diner sees across the belt. */
static void draw_island(bf_canvas_t c, const bf_game_t* g, int near) {
  bf_layer(c, &bf_counter, island, false);
  bf_steel_track(c, g->elapsed_ms);
  plates(c, g, near, false);
  bf_layer(c, &bf_condiments, (bf_rect_t){287, 197, 42, 31}, false);
  bf_layer(c, &bf_tea, (bf_rect_t){477, 209, 43, 24}, false);
  plates(c, g, near, true);
}
static void dining(bf_canvas_t c, const bf_game_t* g) {
  const bf_actor_t* actors[] = {&g->player, &g->diners[0], &g->diners[1]};
  int order[] = {0, 1, 2};
  for (int i = 1; i < 3; i++)
    for (int j = i;
         j > 0 && actors[order[j]]->pos.y < actors[order[j - 1]]->pos.y; j--) {
      int old = order[j];
      order[j] = order[j - 1];
      order[j - 1] = old;
    }
  int near = bf_nearest(g);
  if (g->route_at < g->route_len) {
    bf_point_t d = g->route[g->route_len - 1];
    bf_ellipse(c, (int)d.x, (int)d.y, 6, 3, C(0x47786b), 172);
    bf_ellipse(c, (int)d.x, (int)d.y, 3, 1, light, 255);
  }
  static const furniture_t furniture[] = {
      {227, &bf_booth, {696, 139, 106, 92}, true},
      {238, NULL, {0, 0, 0, 0}, false},
      {311, &bf_booth, {-32, 219, 103, 88}, false},
      {424, &bf_yuzu, {6, 344, 67, 81}, false},
      {431, &bf_booth, {693, 323, 130, 113}, true}};
  int next = 0;
  for (size_t i = 0; i < sizeof(furniture) / sizeof(furniture[0]); i++) {
    const furniture_t* prop = &furniture[i];
    while (next < 3 && actors[order[next]]->pos.y < prop->foot) {
      int k = order[next++];
      person(c, g, actors[k], k);
    }
    if (prop->paint)
      bf_layer(c, prop->paint, prop->bounds, prop->mirror);
    else
      draw_island(c, g, near);
  }
  while (next < 3) {
    int k = order[next++];
    person(c, g, actors[k], k);
  }
  pick_up(c, g);
  if (g->feedback_ms && g->last_dish >= 0) {
    char s[16];
    snprintf(s, sizeof(s), "+%d", bf_menu[g->last_dish].coins);
    int x = (int)g->player.pos.x + 17,
        y = (int)g->player.pos.y - 56 - (1020 - g->feedback_ms) / 100;
    bf_text(c, BF_SMALL, x + 1, y + 1, C(0x6c4a36), s);
    bf_text(c, BF_SMALL, x, y, C(0xffe2a0), s);
  }
  hud(c, g);
  service_tag(c, g, near);
}
static void row(bf_canvas_t c, int y, const char* name, int value,
                bool signed_value) {
  char s[16];
  snprintf(s, sizeof(s), signed_value ? "%+d" : "%d", value);
  bf_text(c, BF_SMALL, 502, y, mute, name);
  right(c, BF_BODY, 737, y, ink, s);
}
static float ease(float x) {
  float y = 1 - x;
  return 1 - y * y * y;
}
static int index_at(unsigned mask, int slot) {
  for (int i = 0; i < 3; i++)
    if (mask & (1u << i)) {
      if (slot-- == 0) return i;
    }
  return 0;
}
static void receipt(bf_canvas_t c, const bf_game_t* g) {
  const bf_album_t* a = &g->album;
  bool hospital = g->phase == BF_HOSPITAL;
  char s[100];
  house(c);
  album_entry(c);
  ticket(c);
  bf_text(c, BF_HEAD, 502, 101, ink,
          hospital ? T("住院账单", "Hospital bill")
                   : T("用餐小票", "Meal receipt"));
  bf_line(c, 502, 114, 737, 114, C(0xbba882));
  row(c, 148, T("吃菜价值", "Food value"), g->value, false);
  row(c, 177, T("实付票价", "Ticket paid"), bf_rules.ticket, false);
  row(c, 207, T("价值差额", "Value less ticket"), g->value - bf_rules.ticket,
      true);
  dotted(c, 503, 738, 219, C(0xb19b77));
  row(c, 247, T("住院费", "Hospital fee"), hospital ? bf_rules.fee : 0, false);
  row(c, 272, T("餐费余额", "Purse left"), g->purse, false);
  const char* outcome =
      g->tasting
          ? (g->varieties == 6 ? T("已尝遍六种", "All six tasted")
                               : T("还有新口味可尝", "More dishes to try"))
          : (bf_has_goal(g) ? T("已吃回票价", "Ticket covered")
                            : T("未吃回票价", "Below ticket value"));
  bf_text(c, BF_SMALL, 502, 305, ink, outcome);
  bool revealing = a->reveal_ms < bf_reveal_duration(g);
  btn(c, bf_ticket_button, clay,
      revealing ? T("收下新卡", "Collect cards") : T("再来一局", "Play again"));
  bf_ellipse(c, 157, 287, 66, 9, C(0x163d3c), 82);
  bf_image(c, hospital ? &bf_rest : &bf_guest, 72, 73, 164, 232);
  bf_text(c, BF_HEAD, 239, 130, light,
          hospital ? T("这顿吃撑了", "A little too full")
                   : T("打烊收盘", "Last plates home"));
  snprintf(s, sizeof(s),
           T("吃了 %d 盘，尝了 %d 种。", "%d plates, %d dishes tasted."),
           g->eaten, g->varieties);
  bf_text(c, BF_BODY, 240, 157, patina, s);
  snprintf(s, sizeof(s), T("回收空盘 %d 只", "%d empty plates cleared"),
           g->returned);
  bf_text(c, BF_SMALL, 240, 181, patina, s);
  for (int i = 0; i < (g->eaten > 3 ? 3 : g->eaten); i++)
    bf_layer(c, &bf_empty, (bf_rect_t){248, 215 - i * 3, 100, 65}, false);
  dotted(c, 31, 438, 314, C(0x78978a));
  bf_text(c, BF_BODY, 34, 345, light, T("本局成就", "This Round's Cards"));
  int count = bf_card_count(a->fresh);
  if (!count)
    bf_text(c, BF_SMALL, 35, 375, patina,
            T("本局没有新卡", "No new cards this meal"));
  for (int i = 0; i < count; i++) {
    int card = index_at(a->fresh, i), x = 38 + i * 74;
    if (a->reveal_ms >= (i + 1) * BF_REVEAL_MS)
      bf_image(c, prints[card], x, 354, 46, 59);
    else
      bf_image(c, &bf_card_back, x, 354, 46, 59);
  }
  if (revealing) {
    int cur = a->reveal_ms / BF_REVEAL_MS, id = index_at(a->fresh, cur);
    float t = (a->reveal_ms % BF_REVEAL_MS) / (float)BF_REVEAL_MS,
          fly = ease(fmaxf(0, (t - .64f) / .36f));
    float flip = fminf(1, t / .40f);
    int w = (int)((104 - 58 * fly) * fabsf(cosf(flip * 3.14159f)));
    if (w < 2) w = 2;
    int h = (int)(134 - 75 * fly), x = (int)(312 + (61 + cur * 74 - 312) * fly),
        y = (int)(232 + (383 - 232) * fly);
    bf_image(c, flip < .5f ? &bf_card_back : prints[id], x - w / 2, y - h / 2,
             w, h);
  }
  bf_text(c, BF_SMALL, 244, 391, light, T("打开成就卡册", "Open card album"));
  if (a->unsaved)
    bf_text(c, BF_SMALL, 34, 448, light,
            T("卡册暂存于本次游戏", "Cards kept for this session"));
}
static void album(bf_canvas_t c, const bf_game_t* g) {
  const bf_album_t* a = &g->album;
  unsigned mask = a->this_meal ? a->fresh : a->owned;
  bf_text(c, BF_TITLE, 34, 59, light, T("成就卡册", "Card album"));
  btn(c, bf_album_back, sea, T("返回", "Back"));
  if (g->phase != BF_TICKET)
    btn(c, bf_album_tab, sea,
        a->this_meal
            ? T("本局新卡   /   查看全部", "This round    /    View all")
            : T("全部收藏   /   查看本局", "Collection    /    This round"));
  char s[80];
  snprintf(s, sizeof(s),
           a->this_meal ? T("本局新卡 %d 张", "%d new this meal")
                        : T("已收藏 %d 张", "%d collected"),
           bf_card_count(mask));
  bf_text(c, BF_BODY, 36, 87, patina, s);
  if (!mask && a->this_meal) {
    bf_center(c, BF_HEAD, 400, 234, light,
              T("本局没有新卡", "No new cards this meal"));
    return;
  }
  for (int i = 0; i < 3; i++) {
    int center = bf_card_position(g, i);
    bool owned = mask & (1u << i);
    if (!(bf_visible_cards(g) & (1u << i))) continue;
    bf_image(c, owned ? prints[i] : &bf_card_back, center - 56, 113, 112, 144);
    const char* title = !owned && i == 2 ? T("尚未发现", "Undiscovered")
                                         : local(c, card_names[i]);
    bf_center(c, BF_BODY, center, 287, light, title);
    if (i == a->selected) bf_box(c, center - 21, 303, 42, 2, C(0xedc48a));
  }
  int id = a->selected;
  if (!a->this_meal || mask & (1u << id)) {
    bf_center(c, BF_SMALL, 400, 360, patina,
              (mask & (1u << id)) ? T("已收藏", "Collected")
                                  : T("尚未收藏", "Not collected"));
    bf_center(c, BF_BODY, 400, 391, light,
              !(mask & (1u << id)) && id == 2
                  ? T("一次特别的用餐经历。", "A surprise during a meal.")
                  : local(c, card_notes[id]));
  }
  if (a->unsaved)
    bf_center(c, BF_SMALL, 400, 444, light,
              T("卡册暂存于本次游戏", "Cards kept for this session"));
}
static void sound_toggle(bf_canvas_t c, const bf_game_t* g) {
  bf_rect_t b = bf_sound_bounds(g);
  bool play = g->phase == BF_DINING;
  int x = b.x + (play ? 16 : 4), y = b.y + (play ? 17 : 18);
  if (play) bf_ellipse(c, x + 4, y, 11, 10, C(0x3d4b42), 225);
  bf_box(c, x - 4, y - 2, 4, 5, light);
  for (int i = 0; i < 5; i++) bf_box(c, x + i, y - 2 - i / 2, 1, 5 + i, light);
  if (g->muted) {
    bf_line(c, x - 5, y + 6, x + 10, y - 6, C(0xf2ad8b));
  } else {
    bf_line(c, x + 7, y - 3, x + 9, y - 1, light);
    bf_line(c, x + 9, y - 1, x + 9, y + 1, light);
    bf_line(c, x + 9, y + 1, x + 7, y + 3, light);
  }
  if (!play)
    bf_text(c, BF_SMALL, x + 15, y + 5, light,
            g->muted ? T("静音", "Muted") : T("声音", "Sound"));
}
void bf_paint_scene(bf_canvas_t c, const bf_game_t* g) {
  c.en = g->en;
  if (g->album.open)
    album(c, g);
  else if (g->phase == BF_DINING)
    dining(c, g);
  else if (g->phase == BF_TICKET)
    foyer(c, g);
  else
    receipt(c, g);
  sound_toggle(c, g);
}
