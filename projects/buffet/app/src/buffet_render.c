#include "buffet_render.h"

#include <math.h>
#include <stdio.h>

#define C(x) BF_COLOR(x)
#define T(zh, en_text) (c.en ? (en_text) : (zh))
static const uint16_t ink = C(0x243c3d), pale = C(0xf8ecd1),
                      shell = C(0xf4e1b8), jade = C(0x8ac3b1),
                      deep = C(0x203e42), sub = C(0x586252), clay = C(0xa94531),
                      dark_red = C(0x812f26);
static const bf_sprite_t* const detail[] = {&bf_salmon, &bf_tamago, &bf_tuna,
                                            &bf_shrimp, &bf_maki,   &bf_inari,
                                            &bf_empty};
static const bf_sprite_t* const small[] = {
    &bf_salmon_belt, &bf_tamago_belt, &bf_tuna_belt, &bf_shrimp_belt,
    &bf_maki_belt,   &bf_inari_belt,  &bf_empty_belt};
static const bf_sprite_t* const cleared[] = {
    &bf_salmon_cleared, &bf_tamago_cleared, &bf_tuna_cleared,
    &bf_shrimp_cleared, &bf_maki_cleared,   &bf_inari_cleared};
static const char* name_of(bool en, int i) {
  return en ? bf_menu[i].name.en : bf_menu[i].name.zh;
}
static void right(bf_canvas_t c, bf_size_t size, int x, int y, uint16_t color,
                  const char* text) {
  bf_text(c, size, x - bf_width(c.en, size, text), y, color, text);
}
static void button(bf_canvas_t c, bf_rect_t b, uint16_t fill, uint16_t fg,
                   const char* s) {
  bf_box(c, b.x, b.y + 3, b.w, b.h - 6, fill);
  bf_box(c, b.x + 3, b.y, b.w - 6, b.h, fill);
  bf_center(c, BF_BODY, b.x + b.w / 2, b.y + b.h / 2 + 7, fg, s);
}
static void dotted(bf_canvas_t c, int x, int end, int y, uint16_t color) {
  for (; x < end; x += 7) bf_box(c, x, y, 3, 1, color);
}
static void clock_text(char* s, size_t n, int ms) {
  int secs = (ms + 999) / 1000;
  (void)snprintf(s, n, "%d:%02d", secs / 60, secs % 60);
}
void bf_paint_ground(bf_canvas_t c, bool dining) {
  if (dining) {
    bf_box(c, 0, 0, 800, 480, deep);
    bf_image(c, &bf_room, 0, 45, 800, 330);
  } else
    bf_image(c, &bf_cloth, 0, 0, 800, 480);
}
static void ticket_paper(bf_canvas_t c) {
  bf_box(c, 483, 62, 282, 378, C(0x193e40));
  bf_box(c, 479, 57, 282, 378, shell);
  bf_box(c, 480, 57, 280, 4, C(0xfbebc9));
  bf_box(c, 486, 65, 1, 359, C(0xd4b88e));
  for (int x = 486; x < 757; x += 12) {
    bf_line(c, x, 434, x + 3, 430, C(0x2b5957));
    bf_line(c, x + 3, 430, x + 6, 434, C(0x2b5957));
  }
  for (int i = 0; i < 2; i++)
    bf_ellipse(c, i ? 761 : 478, 326, 7, 7, C(0x2a5858), 255);
  dotted(c, 494, 748, 326, C(0xab9878));
}
static void foyer(bf_canvas_t c, const bf_game_t* g) {
  char s[112];
  int dish = g->cover;
  bf_image(c, &bf_mark, 29, 17, 54, 33);
  bf_text(c, BF_TITLE, 92, 53, pale, T("潮轮寿司", "Tidewheel Sushi"));
  bf_box(c, 28, 69, 410, 1, C(0x537774));
  button(c, bf_language_button, C(0x335f60), pale, T("EN", "中文"));
  for (int i = 6; i > 0; i--)
    bf_ellipse(c, 250, 228, 126 + 4 * i, 47 + 2 * i, C(0x102e32), 13);
  bf_image(c, detail[dish], 65, 74, 360, 234);
  bf_text(c, BF_HEAD, 34, 325, pale, name_of(c.en, dish));
  bf_text(c, BF_SMALL, 35, 349, pale,
          c.en ? bf_menu[dish].detail.en : bf_menu[dish].detail.zh);
  snprintf(s, sizeof(s),
           T("价值 %d 币    饱腹 +%d%%", "Value %d coins    Fullness +%d%%"),
           bf_menu[dish].coins, bf_menu[dish].fullness);
  bf_text(c, BF_BODY, 35, 377, C(0xbce1c8), s);
  for (int i = 0; i < BF_DISHES; i++) {
    int x = 24 + i * 70;
    bf_image(c, small[i], x + 2, 397, 66, 43);
    if (i == dish) {
      bf_box(c, x + 8, 445, 52, 2, C(0xf7c984));
      bf_box(c, x + 29, 443, 10, 3, C(0xf7c984));
    }
  }
  ticket_paper(c);
  bf_text(c, BF_HEAD, 502, 108, ink, T("用餐券", "Meal ticket"));
  bf_line(c, 502, 121, 739, 121, C(0xb79d78));
  bf_text(c, BF_BODY, 502, 154, sub, T("用餐时间", "Dining time"));
  clock_text(s, sizeof(s), bf_rules.duration_ms);
  right(c, BF_HEAD, 737, 155, ink, s);
  bf_text(c, BF_BODY, 502, 188, sub, T("每局餐费袋", "Meal purse"));
  snprintf(s, sizeof(s), T("%d 币", "%d coins"), bf_rules.purse);
  right(c, BF_BODY, 737, 188, ink, s);
  bf_text(c, BF_SMALL, 502, 216, sub, T("选个目标", "Choose a goal"));
  button(c, bf_goal_button, C(0xdacbac), ink,
         g->tasting ? T("尝遍六种", "Taste all six")
                    : T("吃回票价", "Cover the ticket"));
  bf_line(c, 720, 243, 724, 247, sub);
  bf_line(c, 724, 247, 728, 243, sub);
  bf_text(c, BF_SMALL, 503, 292, sub,
          T("点地面行走，靠近餐盘再点吃。",
            "Tap the floor to walk. Eat within reach."));
  snprintf(s, sizeof(s),
           T("超过 %d%%，先停口消化。", "Above %d%%? Let your meal digest."),
           bf_rules.red_line);
  bf_text(c, BF_SMALL, 503, 314, sub, s);
  snprintf(s, sizeof(s), T("买票入座   %d 币", "Buy ticket   %d coins"),
           bf_rules.ticket);
  button(c, bf_ticket_button, clay, pale, s);
}
static void hud(bf_canvas_t c, const bf_game_t* g) {
  char s[64];
  bool red = g->fullness_milli > bf_rules.red_line * 1000;
  bf_box(c, 0, 0, 800, 46, deep);
  bf_box(c, 0, 45, 800, 2, C(0xb89568));
  clock_text(s, sizeof(s), bf_rules.duration_ms - g->elapsed_ms);
  bf_text(c, BF_HEAD, 17, 32, pale, s);
  bf_text(c, BF_SMALL, 81, 30, jade, T("后打烊", "to close"));
  bf_box(c, 162, 9, 1, 28, C(0x557571));
  bf_text(c, BF_SMALL, 183, 29, jade,
          g->tasting ? T("已尝", "Tasted") : T("已吃价值", "Meal value"));
  if (g->tasting)
    snprintf(s, sizeof(s), "%d / %d", g->varieties, BF_DISHES);
  else
    snprintf(s, sizeof(s), "%d / %d", g->value, bf_rules.ticket);
  bf_text(c, BF_BODY, 279, 31, pale, s);
  int display = (g->fullness_milli + 999) / 1000;
  snprintf(s, sizeof(s), T("饱腹 %d%%", "Fullness %d%%"), display);
  right(c, BF_BODY, 525, 31, red ? C(0xffb09a) : pale, s);
  int x = 543, w = 235, max = 160, cap = x + w * bf_rules.capacity / max,
      edge = x + w * bf_rules.red_line / max;
  bf_box(c, x, 10, w, 7, C(0x62716b));
  bf_box(c, edge, 10, x + w - edge, 7, C(0x865a50));
  int fill = (int)(w * fminf(1.f, (float)g->fullness_milli / (max * 1000.f)));
  bf_box(c, x, 10, fill, 7, red ? C(0xee805c) : jade);
  bf_box(c, cap, 8, 2, 12, pale);
  bf_box(c, edge, 8, 2, 12, C(0xf4ab8f));
  snprintf(s, sizeof(s), T("容量 %d%%", "%d%% full"), bf_rules.capacity);
  right(c, BF_SMALL, cap + 3, 37, pale, s);
  snprintf(s, sizeof(s), T("红线 %d%%", "%d%% red"), bf_rules.red_line);
  bf_text(c, BF_SMALL, edge - 7, 37, C(0xf3ba9e), s);
}
static void actor_in_room(bf_canvas_t c, const bf_game_t* g,
                          const bf_actor_t* a, int kind) {
  int x = (int)lroundf(a->pos.x), y = (int)lroundf(a->pos.y);
  float scale = a->pos.y < 210 ? .88f : 1.03f;
  int w = (int)(72 * scale), h = (int)(80 * scale);
  int pose = a->walking ? 1 + ((int)a->stride % 4) : 0;
  if (kind == 0) {
    if (g->bite_ms > 0)
      pose = g->bite_ms > 494 ? 5 : g->bite_ms > 220 ? 6 : 7;
    else if (!a->walking && g->ambient_ms % 3800 > 3610)
      pose = 8;
    if (g->turn_ms > 0) w -= (int)(3 * sinf(g->turn_ms * 3.14159f / 115.f));
  }
  bf_ellipse(c, x + 3, y + 2, (int)(17 * scale), (int)(5 * scale), C(0x674d3c),
             66);
  bf_ellipse(c, x + 1, y + 1, (int)(12 * scale), 3, C(0x614c3c), 64);
  int breath = (!a->walking && sinf(g->ambient_ms * .0033f + kind) > 0) ? 1 : 0;
  bf_face_t visible_face =
      (!kind && g->bite_ms > 0 && a->face == BF_UP) ? BF_RIGHT : a->face;
  bf_blit(
      c, &bf_cast,
      (bf_rect_t){pose * 72, (kind * 4 + (int)visible_face) * 80, 72, 80},
      (bf_rect_t){x - w / 2, y - (int)(74 * scale) + breath, w, h - breath});
  if (!kind && g->elapsed_ms < 2400) {
    int top = y - (int)(57 * scale);
    bf_box(c, x - 16, top - 21, 32, 16, deep);
    bf_center(c, BF_SMALL, x, top - 8, pale, T("你", "You"));
    bf_box(c, x - 1, top - 5, 3, 3, deep);
  }
}
static void plates(bf_canvas_t c, const bf_game_t* g, int chosen) {
  const float lap = (float)g->elapsed_ms / bf_rules.lap_ms;
  for (int i = 0; i < 74; i++) {
    bf_place_t b = bf_belt(lap + (float)i / 74.f);
    float nx = b.normal.x, ny = b.normal.y;
    int x = (int)b.pos.x, y = (int)b.pos.y;
    bf_line(c, (int)(x - nx * 4), (int)(y - ny * 3), (int)(x + nx * 3),
            (int)(y + ny * 2), C(0x5d766e));
  }
  /* The rear arc is painted first so adjacent overhanging plates occlude in
   * surface depth order through the bends. */
  for (int half = 0; half < 2; half++)
    for (int i = 0; i < BF_SLOTS; i++) {
      bf_place_t p = bf_place(g, i);
      if ((p.pos.y >= 225) != (half == 1)) continue;
      int x = (int)lroundf(p.pos.x), y = (int)lroundf(p.pos.y),
          d = bf_serving(g, i);
      bf_ellipse(c, x + 3, y + 2, 25, 8, C(0x3c4841), 88);
      if (i == chosen) {
        bf_ellipse(c, x, y - 1, 33, 13, C(0xffdb88), 255);
        bf_ellipse(c, x, y - 2, 29, 11, C(0x34594d), 255);
      }
      bf_image(c, g->empty[i] ? cleared[d] : small[d], x - 30, y - 25, 60, 40);
      if (i == chosen) {
        int v = y + 17;
        bf_line(c, x - 4, v + 4, x, v, C(0x315948));
        bf_line(c, x, v, x + 4, v + 4, C(0x315948));
        bf_box(c, x - 1, v + 3, 3, 4, C(0xffe5a3));
      }
    }
}
static void dining(bf_canvas_t c, const bf_game_t* g) {
  const bf_actor_t* actors[3] = {&g->player, &g->diners[0], &g->diners[1]};
  int order[3] = {0, 1, 2};
  for (int i = 1; i < 3; i++)
    for (int j = i;
         j > 0 && actors[order[j]]->pos.y < actors[order[j - 1]]->pos.y; j--) {
      int old = order[j];
      order[j] = order[j - 1];
      order[j - 1] = old;
    }
  int near = bf_nearest(g);
  if (g->route_at < g->route_len) {
    bf_point_t dest = g->route[g->route_len - 1];
    int x = (int)dest.x, y = (int)dest.y;
    bf_ellipse(c, x, y, 9, 4, C(0x47786b), 172);
    bf_ellipse(c, x, y, 5, 2, C(0xf8e8be), 255);
  }
  for (int i = 0; i < 3; i++)
    if (actors[order[i]]->pos.y < 238)
      actor_in_room(c, g, actors[order[i]], order[i]);
  bf_image(c, &bf_counter, 136, 172, 528, 125);
  plates(c, g, near);
  for (int i = 0; i < 3; i++)
    if (actors[order[i]]->pos.y >= 238)
      actor_in_room(c, g, actors[order[i]], order[i]);
  if (g->feedback_ms > 0 && g->last_dish >= 0) {
    char feedback[20];
    snprintf(feedback, sizeof(feedback), "+%d", bf_menu[g->last_dish].coins);
    int y = (int)g->player.pos.y - 58 - (1020 - g->feedback_ms) / 95;
    int x = (int)g->player.pos.x + 20;
    bf_box(c, x - 2, y - 17, 33, 23, deep);
    bf_text(c, BF_SMALL, x + 1, y, pale, feedback);
  }
  hud(c, g);
  bool red = g->fullness_milli > bf_rules.red_line * 1000;
  bf_box(c, 0, 374, 800, 106, red ? dark_red : deep);
  bf_box(c, 0, 374, 800, 2, red ? C(0xf5ac87) : C(0xdbaf78));
  char s[118];
  int preview = g->bite_ms ? g->last_dish : near < 0 ? -1 : bf_serving(g, near);
  if (preview >= 0) {
    bf_image(c, detail[preview], 10, 382, 128, 84);
    bf_text(c, BF_HEAD, 149, 411, pale, name_of(c.en, preview));
    snprintf(s, sizeof(s),
             T("价值 %d 币    饱腹 +%d%%", "Value %d coins    Fullness +%d%%"),
             bf_menu[preview].coins, bf_menu[preview].fullness);
    bf_text(c, BF_BODY, 150, 438, C(0xd7e7d0), s);
  } else {
    bf_image(c, &bf_otter, 34, 384, 85, 84);
    bool waiting = bf_at_belt(g);
    bf_text(c, BF_HEAD, 148, 413, pale,
            waiting ? T("等一盘过来", "Watch the next plate")
                    : T("走到带边", "Walk to the belt"));
    bf_text(c, BF_BODY, 149, 441, pale,
            g->obstructed ? T("等食客走过，或绕到另一侧。",
                              "Wait for the diner or take the other side.")
            : waiting ? T("盘沿亮起，就能吃这盘。",
                          "Eat when the plate rim lights up.")
                      : T("点地面或远处的盘子走过去。",
                          "Tap the floor or a passing plate to walk there."));
  }
  if (red) {
    snprintf(
        s, sizeof(s),
        T("红区！停口消化，%d 秒后送医", "Red zone! Rest now. Hospital in %ds"),
        (bf_rules.tolerance_ms - g->red_ms + 999) / 1000);
  } else if (g->recovered_ms)
    snprintf(
        s, sizeof(s), "%s",
        T("已脱离红区，警报解除。", "Back below the red line. All clear."));
  else if (g->fullness_milli > bf_rules.capacity * 1000)
    snprintf(s, sizeof(s),
             T("已经饱了，超过 %d%% 会进红区。",
               "Past capacity. Red zone above %d%%."),
             bf_rules.red_line);
  else if (g->keyboard)
    snprintf(s, sizeof(s), "%s",
             T("点地面或方向键行走，点吃或按空格。",
               "Walk by tap or arrow keys. Tap Eat or press Space."));
  else
    snprintf(s, sizeof(s), "%s",
             T("点地面行走，在带边点吃。",
               "Tap the floor to walk. Tap Eat at the belt."));
  bf_text(c, BF_SMALL, 150, 464, red ? pale : jade, s);
  button(c, bf_eat_button, near < 0 || g->bite_ms ? C(0x496264) : C(0xc05d3c),
         pale, g->bite_ms ? T("品尝中", "Tasting") : T("吃这盘", "Eat plate"));
}
static void receipt(bf_canvas_t c, const bf_game_t* g) {
  bool hospital = g->phase == BF_HOSPITAL;
  char s[128];
  bf_image(c, &bf_mark, 29, 17, 54, 33);
  bf_text(c, BF_TITLE, 91, 53, pale, T("潮轮寿司", "Tidewheel Sushi"));
  bf_line(c, 30, 69, 437, 69, C(0x587d78));
  bf_ellipse(c, 234, 286, 154, 24, C(0x173c3e), 110);
  for (int i = 0; i < (g->eaten > 4 ? 4 : g->eaten); i++)
    bf_image(c, &bf_empty, 204, 210 - 6 * i, 182, 117);
  bf_image(c, hospital ? &bf_rest : &bf_otter, 84, 101, 190, 187);
  bf_text(c, BF_HEAD, 34, 347, pale,
          hospital ? T("这顿吃撑了", "A little too full")
                   : T("打烊收盘", "Last plates home"));
  snprintf(
      s, sizeof(s),
      T("吃了 %d 盘，尝了 %d 种。", "%d plates eaten, %d of 6 dishes tasted."),
      g->eaten, g->varieties);
  bf_text(c, BF_BODY, 35, 380, pale, s);
  snprintf(s, sizeof(s),
           T("传送带已回收 %d 只空盘。", "The belt cleared %d empty plates."),
           g->returned);
  bf_text(c, BF_SMALL, 35, 412, jade, s);
  ticket_paper(c);
  bf_text(c, BF_HEAD, 502, 107, ink,
          hospital ? T("住院账单", "Hospital bill")
                   : T("用餐小票", "Meal receipt"));
  bf_line(c, 502, 120, 737, 120, C(0xb79d78));
  bf_text(c, BF_BODY, 502, 150, sub, T("吃菜价值", "Food value"));
  snprintf(s, sizeof(s), "%d", g->value);
  right(c, BF_BODY, 737, 150, ink, s);
  bf_text(c, BF_BODY, 502, 180, sub, T("实付票价", "Ticket paid"));
  snprintf(s, sizeof(s), "%d", bf_rules.ticket);
  right(c, BF_BODY, 737, 180, ink, s);
  bf_text(c, BF_BODY, 502, 210, sub, T("价值差额", "Value less ticket"));
  snprintf(s, sizeof(s), "%+d", g->value - bf_rules.ticket);
  right(c, BF_HEAD, 737, 211, g->value >= bf_rules.ticket ? C(0x34634c) : clay,
        s);
  dotted(c, 503, 738, 221, C(0xb19b77));
  bf_text(c, BF_SMALL, 502, 249, sub, T("住院费", "Hospital fee"));
  snprintf(s, sizeof(s), "%d", hospital ? bf_rules.fee : 0);
  right(c, BF_BODY, 737, 249, ink, s);
  bf_text(c, BF_SMALL, 502, 275, sub, T("餐费袋余额", "Purse left"));
  snprintf(s, sizeof(s), "%d", g->purse);
  right(c, BF_BODY, 737, 275, ink, s);
  bf_text(c, BF_SMALL, 502, 307, ink,
          g->tasting
              ? (g->varieties == 6 ? T("已尝遍六种", "All six tasted")
                                   : T("还有新口味可尝", "More dishes to try"))
              : (bf_has_goal(g) ? T("已吃回票价", "Ticket covered")
                                : T("未吃回票价", "Below ticket value")));
  button(c, bf_ticket_button, clay, pale, T("再来一局", "Play again"));
}
void bf_paint_scene(bf_canvas_t c, const bf_game_t* g) {
  c.en = g->en;
  if (g->phase == BF_TICKET)
    foyer(c, g);
  else if (g->phase == BF_DINING)
    dining(c, g);
  else
    receipt(c, g);
}
