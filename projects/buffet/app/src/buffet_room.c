#include "buffet_render.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define COLOR(rgb) BF_COLOR(rgb)
static const uint16_t ink = COLOR(0x263e41), linen = COLOR(0xf7ecd4),
                      mint = COLOR(0xbad3bb);
static const bf_paint_t* const servings[] = {&bf_salmon_table, &bf_tamago_table,
                                             &bf_tuna_table,   &bf_shrimp_table,
                                             &bf_maki_table,   &bf_inari_table};
static const bf_paint_t* const bites[] = {&bf_salmon_bite, &bf_tamago_bite,
                                          &bf_tuna_bite,   &bf_shrimp_bite,
                                          &bf_maki_bite,   &bf_inari_bite};
static const bf_paint_t* const stands[] = {&bf_wave, &bf_leaf, &bf_sun};
static const bf_paint_t* const emblems[] = {&bf_hud_wave, &bf_hud_leaf,
                                            &bf_hud_sun};
static const char* tr(bf_canvas_t c, const char* zh, const char* en) {
  return c.en ? en : zh;
}
static void panel(bf_canvas_t c, bf_rect_t r, uint16_t color) {
  bf_box(c, r.x + 4, r.y, r.w - 8, r.h, color);
  bf_box(c, r.x, r.y + 3, r.w, r.h - 6, color);
}
static void next_orders(bf_canvas_t c, const bf_game_t* g) {
  int order[BF_TABLES];
  int count = bf_next_tables(g, order);
  bf_layer(c, &bf_hud_next, (bf_rect_t){10, 7, 236, 51}, false);
  if (!count) {
    bool full = true;
    for (int table = 0; table < BF_TABLES; table++)
      if (bf_next_setting(g, table) >= 0) full = false;
    bf_text(c, BF_SMALL, 25, 24, mint, tr(c, "餐台", "Serving tables"));
    bf_text(c, BF_BODY, 25, 43, linen,
            full ? tr(c, "餐台已摆满", "Tables are stocked")
                 : tr(c, "最后一轮已上桌", "Last servings are out"));
    return;
  }
  int table = order[0];
  int dish = bf_next_dish(g, table);
  int seconds = (g->service[table].next_at_ms - g->elapsed_ms + 999) / 1000;
  const char* name = c.en ? bf_menu[dish].name.en : bf_menu[dish].name.zh;
  bf_layer(c, servings[dish], (bf_rect_t){14, 12, 44, 35}, false);
  bf_text(c, BF_SMALL, 63, 24, mint, tr(c, "下次上菜", "Up next"));
  bf_text(c, BF_BODY, 63, 43, linen, name);
  const char* table_name =
      c.en ? bf_tables[table].name.en : bf_tables[table].name.zh;
  bf_layer(c, emblems[table], (bf_rect_t){126, 11, 15, 15}, false);
  bf_text(c, BF_SMALL, 145, 24, mint, table_name);
  char timer[16];
  snprintf(timer, sizeof(timer), tr(c, "%d秒", "%ds"), seconds);
  bf_center(c, BF_BODY, 218, 36, COLOR(0xffda93), timer);
}
static void lower_hud(bf_canvas_t c, const bf_game_t* g) {
  char text[68];
  bf_layer(c, &bf_hud_fullness, (bf_rect_t){9, 430, 211, 49}, false);
  bf_layer(c, &bf_hud_plates, (bf_rect_t){14, 433, 26, 26}, false);
  bf_text(c, BF_SMALL, 44, 446, mint, tr(c, "已吃", "Eaten"));
  snprintf(text, sizeof(text), tr(c, "%d 盘", "%d plates"), g->eaten);
  bf_text(c, BF_BODY, 44, 465, linen, text);
  bf_text(c, BF_SMALL, 94, 446, mint, tr(c, "饱腹", "Fullness"));
  int gx = 90, gy = 448, gw = 124, gh = 24;
  bf_image(c, &bf_gauge, gx, gy, gw, gh);
  int fill = (int)(112 * fminf(1.f, g->gauge_milli / 160000.f));
  if (fill > 0)
    bf_blit(c, &bf_gauge_full, (bf_rect_t){72, 16, 2 * fill, 28},
            (bf_rect_t){gx + 29, gy + 7, (int)lroundf(fill * gw / 154.f), 12});
  if (g->fullness_milli > bf_rules.red_line * 1000) {
    snprintf(text, sizeof(text),
             tr(c, "快撑了   %d 秒后送医", "Too full   Hospital in %ds"),
             (bf_rules.tolerance_ms - g->red_ms + 999) / 1000);
    panel(c, (bf_rect_t){13, 407, bf_width(c.en, BF_SMALL, text) + 20, 23},
          COLOR(0x943e32));
    bf_text(c, BF_SMALL, 22, 423, linen, text);
  } else if (g->recovered_ms) {
    const char* message = tr(c, "已经脱险", "All clear");
    panel(c, (bf_rect_t){13, 407, bf_width(c.en, BF_SMALL, message) + 20, 23},
          ink);
    bf_text(c, BF_SMALL, 22, 423, mint, message);
  }
  bf_layer(c, &bf_hud_clock_bg, (bf_rect_t){672, 430, 118, 49}, false);
  bf_layer(c, &bf_hud_clock, (bf_rect_t){676, 434, 33, 33}, false);
  bf_text(c, BF_SMALL, 711, 445, mint, tr(c, "用餐剩余", "Time left"));
  int sec = (bf_rules.duration_ms - g->elapsed_ms + 999) / 1000;
  snprintf(text, sizeof(text), "%d:%02d", sec / 60, sec % 60);
  bf_text(c, BF_HEAD, 711, 467, sec < 15 ? COLOR(0xffab83) : linen, text);
}
void bf_paint_ground(bf_canvas_t c, bool dining) {
  if (!dining) {
    bf_image(c, &bf_cloth, 0, 0, 800, 480);
    return;
  }
  memcpy(c.pixels, bf_floor_pixels, sizeof(bf_floor_pixels));
  /* The shadow belongs to the floor so that the player walks through it. */
  for (int t = 0; t < BF_TABLES; t++) {
    bf_rect_t r = bf_table_footprint(t);
    bf_layer(c, &bf_table_shadow,
             (bf_rect_t){r.x - 8, r.y - BF_TABLE_RISE - 8, 160, 110}, false);
  }
}
static void table_surface(bf_canvas_t c, int table) {
  bf_rect_t foot = bf_table_footprint(table);
  bf_layer(c, stands[table],
           (bf_rect_t){foot.x - 8, foot.y - BF_TABLE_RISE - 8, 160, 110},
           false);
}
static void ring(bf_canvas_t c, int x, int y, int rx, int ry, uint16_t color) {
  int px = x + rx, py = y;
  for (int i = 1; i <= 36; i++) {
    float a = i * .174532925f;
    int nx = x + (int)lroundf(rx * cosf(a)),
        ny = y + (int)lroundf(ry * sinf(a));
    bf_line(c, px, py, nx, ny, color);
    px = nx;
    py = ny;
  }
}
static void plates(bf_canvas_t c, const bf_game_t* g, int t, int near) {
  for (int i = 0; i < BF_SETTINGS; i++) {
    int slot = t * BF_SETTINGS + i;
    const bf_setting_t* setting = &g->settings[slot];
    bf_point_t p = bf_place(g, slot).pos;
    int x = (int)lroundf(p.x), y = (int)lroundf(p.y);
    if (setting->dish < 0) {
      bf_layer(c, &bf_hud_berth, (bf_rect_t){x - 16, y - 12, 32, 24}, false);
      int till = g->service[t].next_at_ms - g->elapsed_ms;
      if (slot == bf_next_setting(g, t) && g->service[t].next_at_ms >= 0 &&
          g->service[t].next_at_ms < bf_rules.duration_ms && till >= 0 &&
          till < 500)
        bf_layer(c, &bf_hud_arrival, (bf_rect_t){x - 18, y - 12, 36, 24},
                 false);
      continue;
    }
    int age = g->elapsed_ms - setting->arrived_ms;
    float rise = 0;
    int w = 42, h = 34;
    if (!g->album.reduced_motion && age >= 0 && age < 240) {
      float f = age / 240.f;
      rise = (1 - f) * (1 - f) * 7.f - 1.5f * sinf(f * 3.14159f);
      w -= (int)(2 * (1 - f));
      h -= (int)(1.5f * (1 - f));
    }
    bf_layer(c, &bf_plate_shade, (bf_rect_t){x - w / 2, y - 21, w, h}, false);
    if (slot == near)
      bf_layer(c, &bf_hud_selected, (bf_rect_t){x - 21, y - 14, 42, 28}, false);
    else if (age >= 0 && age < 250 && !g->album.reduced_motion)
      bf_layer(c, &bf_hud_arrival, (bf_rect_t){x - 21, y - 14, 42, 28}, false);
    bf_layer(c, servings[setting->dish],
             (bf_rect_t){x - w / 2, y - 21 - (int)lroundf(rise), w, h}, false);
  }
}

static void person(bf_canvas_t c, const bf_game_t* g) {
  const bf_actor_t* player = &g->player;
  int x = (int)lroundf(player->pos.x), y = (int)lroundf(player->pos.y);
  int pose = 0;
  if (player->walking) pose = 1 + (int)player->stride % 4;
  if (!player->walking && g->ambient_ms % 4200 > 4020) pose = 8;
  if (g->bite_ms) pose = g->bite_ms > 494 ? 5 : g->bite_ms > 220 ? 6 : 7;
  bf_face_t face = player->face;
  if (g->bite_ms && face == BF_UP) face = BF_RIGHT;
  int w = BF_PERSON_W, h = BF_PERSON_H;
  if (g->turn_ms && !g->album.reduced_motion)
    w -= (int)lroundf(3 * sinf(g->turn_ms * 3.14159f / 115));
  int breath =
      !player->walking && !g->bite_ms && sinf(g->ambient_ms * .0034f) > 0;
  bf_ellipse(c, x + 2, y + 1, 12, 5, COLOR(0x624f43), 54);
  bf_ellipse(c, x + 1, y + 1, 8, 3, COLOR(0x594d41), 55);
  bf_blit(c, &bf_cast,
          (bf_rect_t){pose * BF_PERSON_W * 2, (int)face * BF_PERSON_H * 2,
                      BF_PERSON_W * 2, BF_PERSON_H * 2},
          (bf_rect_t){x - w / 2, y - BF_PERSON_FOOT + breath, w, h - breath});
}
static void bite(bf_canvas_t c, const bf_game_t* g) {
  if (!g->bite_ms || g->last_dish < 0) return;
  int age = 680 - g->bite_ms;
  if (age > 352) return;
  float f = fminf(1, age / 185.f);
  float t = f * f * (3.f - 2.f * f);
  bool left = g->player.face == BF_LEFT;
  float hand_x = g->player.pos.x + (left ? -5 : 5),
        hand_y = g->player.pos.y - BF_PERSON_MOUTH;
  float x = g->bite_source.x + (hand_x - g->bite_source.x) * t;
  float y = (g->bite_source.y - 7) * (1 - t) + hand_y * t;
  /* Limit the lift beside the face. The tiny final piece arrives level with
   * the palm and stays below the near eye through contact. */
  if (f < .69f)
    y -= 3.f * sinf(f * 3.14159f);
  else
    y = fmaxf(hand_y, y);
  int w = (int)lroundf(18 - 11 * t), h = (int)lroundf(10 - 6 * t);
  bf_layer(c, bites[g->last_dish],
           (bf_rect_t){(int)lroundf(x) - w / 2, (int)lroundf(y) - h / 2, w, h},
           left);
}
static void draw_table(bf_canvas_t c, const bf_game_t* g, int t, int near) {
  table_surface(c, t);
  plates(c, g, t, near);
  /* A short light at the actual destination ties the serving slip to its table.
   */
  int due = g->service[t].next_at_ms - g->elapsed_ms;
  if (g->service[t].next_at_ms >= 0 &&
      g->service[t].next_at_ms < bf_rules.duration_ms && due >= 0 &&
      due < 900) {
    bf_rect_t r = bf_table_footprint(t);
    int y = r.y + r.h - 12;
    bf_ellipse(c, r.x + r.w / 2, y, 4, 2, COLOR(0xf4d38d), 230);
  }
}
static void action(bf_canvas_t c, const bf_game_t* g, int slot) {
  if (slot < 0 || g->bite_ms) return;
  bf_rect_t r = bf_eat_bounds(g, slot);
  r.x += 2;
  r.y += 6;
  r.w -= 4;
  r.h -= 12;
  panel(c, r, COLOR(0x96513a));
  bf_center(c, BF_BODY, r.x + r.w / 2, r.y + 21, linen, tr(c, "吃", "Eat"));
}
void bf_paint_dining(bf_canvas_t c, const bf_game_t* g) {
  int near = bf_nearest(g);
  if (g->route_at < g->route_len) {
    bf_point_t d = g->route[g->route_len - 1];
    ring(c, (int)d.x, (int)d.y, 6, 3, COLOR(0x47776e));
  }
  const int order[BF_TABLES] = {0, 2, 1};
  bool person_drawn = false;
  for (int j = 0; j < BF_TABLES; j++) {
    int t = order[j];
    bf_rect_t r = bf_table_footprint(t);
    if (!person_drawn && g->player.pos.y < r.y + r.h) {
      person(c, g);
      person_drawn = true;
    }
    draw_table(c, g, t, near);
  }
  if (!person_drawn) person(c, g);
  bite(c, g);
  if (g->feedback_ms && g->last_dish >= 0) {
    char s[12];
    snprintf(s, sizeof(s), "+%d", bf_menu[g->last_dish].coins);
    int x = (int)g->player.pos.x + 14,
        y = (int)g->player.pos.y - 34 - (850 - g->feedback_ms) / 125;
    bf_text(c, BF_SMALL, x + 1, y + 1, ink, s);
    bf_text(c, BF_SMALL, x, y, COLOR(0xffde8d), s);
  }
  action(c, g, near);
  next_orders(c, g);
  lower_hud(c, g);
}
