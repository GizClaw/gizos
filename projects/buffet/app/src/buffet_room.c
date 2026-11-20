#include "buffet_render.h"
#include "buffet_mesh.h"
#include "buffet_rotary.h"
#include <math.h>
#include <stdio.h>

#define COLOR(rgb) BF_COLOR(rgb)
static const uint16_t linen = COLOR(0xf7ecd4);
static const bf_paint_t* const servings[BF_DISHES] = {
    &bf_salmon_table, &bf_tamago_table, &bf_tuna_table,  &bf_shrimp_table,
    &bf_maki_table,   &bf_inari_table,  &bf_ramen_table, &bf_custard_table};
static const bf_paint_t* const bites[BF_DISHES] = {
    &bf_salmon_bite, &bf_tamago_bite, &bf_tuna_bite,  &bf_shrimp_bite,
    &bf_maki_bite,   &bf_inari_bite,  &bf_ramen_bite, &bf_custard_bite};
static const uint16_t enamel[BF_TABLES] = {COLOR(0x337672), COLOR(0xb45e42),
                                           COLOR(0x557d51), COLOR(0x934830),
                                           COLOR(0x616083)};

static void ring(bf_canvas_t c, int x, int y, int rx, int ry, uint16_t color) {
  int px = x + rx, py = y;
  for (int i = 1; i <= 40; i++) {
    float a = i * .157079633f;
    int nx = x + (int)lroundf(rx * cosf(a)),
        ny = y + (int)lroundf(ry * sinf(a));
    bf_line(c, px, py, nx, ny, color);
    px = nx;
    py = ny;
  }
}
typedef struct {
  int x, y, rx, ry;
} badge_t;
static void stroke(bf_canvas_t c, badge_t b, float x1, float y1, float x2,
                   float y2, uint16_t color) {
  int ax = b.x + (int)lroundf(b.rx * x1), ay = b.y + (int)lroundf(b.ry * y1),
      bx = b.x + (int)lroundf(b.rx * x2), by = b.y + (int)lroundf(b.ry * y2);
  bf_line(c, ax, ay, bx, by, color);
  bf_line(c, ax, ay + 1, bx, by + 1, color);
}
/* One enamel die is used on its workstation and on the next-serving slip. */
static void seal(bf_canvas_t c, int station, badge_t b) {
  bf_ellipse(c, b.x, b.y + 1, b.rx + 1, b.ry + 1, COLOR(0x68563e), 255);
  bf_ellipse(c, b.x, b.y, b.rx + 1, b.ry + 1, COLOR(0xf0d3a1), 255);
  bf_ellipse(c, b.x, b.y, b.rx - 1, b.ry - 1, enamel[station], 255);
  if (station == 0) {
    for (int row = 0; row < 2; row++) {
      float last = row * .36f - .18f;
      for (int i = 1; i <= 12; i++) {
        float t = i / 12.f,
              next = row * .36f - .18f + .13f * sinf(t * 6.28318f);
        stroke(c, b, -.67f + (i - 1) * 1.34f / 12, last, -.67f + i * 1.34f / 12,
               next, linen);
        last = next;
      }
    }
  } else if (station == 1) {
    bf_ellipse(c, b.x, b.y, b.rx / 4, b.ry / 4, linen, 255);
    for (int i = 0; i < 8; i++) {
      float a = i * .78539816f;
      stroke(c, b, .40f * cosf(a), .40f * sinf(a), .67f * cosf(a),
             .67f * sinf(a), linen);
    }
  } else if (station == BF_LEAF) {
    const float leaf[][2] = {{-.5f, .4f},   {-.5f, -.16f}, {-.14f, -.49f},
                             {.53f, -.52f}, {.49f, .09f},  {.13f, .4f},
                             {-.5f, .4f}};
    for (int i = 1; i < 7; i++)
      stroke(c, b, leaf[i - 1][0], leaf[i - 1][1], leaf[i][0], leaf[i][1],
             linen);
    stroke(c, b, -.64f, .59f, .28f, -.22f, linen);
  } else if (station == BF_HOT) {
    stroke(c, b, -.68f, .10f, .68f, .10f, linen);
    stroke(c, b, -.61f, .14f, -.38f, .48f, linen);
    stroke(c, b, -.38f, .48f, .36f, .48f, linen);
    stroke(c, b, .36f, .48f, .61f, .14f, linen);
    for (int i = 0; i < 2; i++) {
      float x = i * .43f - .28f;
      stroke(c, b, x, -.07f, x + .11f, -.31f, linen);
      stroke(c, b, x + .11f, -.31f, x, -.53f, linen);
    }
  } else {
    stroke(c, b, -.54f, -.10f, .54f, -.10f, linen);
    stroke(c, b, -.46f, -.05f, -.36f, .48f, linen);
    stroke(c, b, -.36f, .48f, .36f, .48f, linen);
    stroke(c, b, .36f, .48f, .46f, -.05f, linen);
    stroke(c, b, -.48f, -.23f, -.06f, -.39f, linen);
    stroke(c, b, -.06f, -.39f, .45f, -.23f, linen);
    bf_ellipse(c, b.x, b.y - (int)(b.ry * .51f), 2, 2, linen, 255);
  }
}
/* World badges follow countertop heights. */
static void stations(bf_canvas_t c) {
  const bf_world_t at[BF_TABLES] = {
      {6, 7.24f}, {14, 7.24f}, {10, .21f}, {.85f, 9.63f}, {19.15f, 9.63f}};
  for (int t = 0; t < BF_TABLES; t++) {
    float z = t < BF_LOOPS ? 23.6f : t == BF_LEAF ? 40 : 18.7f;
    bf_point_t p = bf_project(at[t], z);
    int rx = t < BF_LOOPS ? 20 : 13;
    seal(c, t, (badge_t){(int)p.x, (int)p.y, rx, rx * 3 / 4});
  }
}
/* Last-call tickets share the station's enamel crest. An hourglass and the
 * shrinking rule distinguish a withdrawal from the next-service clock. */
static void hourglass(bf_canvas_t c, int x, int y, uint16_t color) {
  bf_box(c, x - 6, y - 8, 13, 2, color);
  bf_box(c, x - 6, y + 6, 13, 2, color);
  for (int i = 0; i < 2; ++i) {
    bf_line(c, x - 5 + i, y - 6, x + 4 + i, y + 6, color);
    bf_line(c, x + 4 + i, y - 6, x - 5 + i, y + 6, color);
  }
  bf_box(c, x - 3, y + 4, 7, 2, color);
}

static unsigned last_call_pulse(int left, bool reduced_motion) {
  if (reduced_motion) return 130;
  int phase = (BF_FRESH_LAST_MS - left) % 1400;
  return (unsigned)(phase < 700 ? phase : 1400 - phase) * 255 / 700;
}

static void expiry_ticket(bf_canvas_t c, const bf_game_t* g, int slot,
                          int row) {
  const bf_setting_t* portion = &g->settings[slot];
  int left = bf_fresh_left(g, slot), y = 6 + row * 33;
  bool last = left <= BF_FRESH_LAST_MS;
  bf_layer(c, &bf_hud_next, (bf_rect_t){7, y, 119, 32}, false);
  if (last) {
    unsigned pulse = last_call_pulse(left, g->album.reduced_motion);
    bf_ellipse(c, 100, y + 15, 21, 13, COLOR(0xa84e3c), 180 + pulse / 4);
  }
  bf_layer(c, servings[portion->dish], (bf_rect_t){11, y - 2, 43, 35}, false);
  seal(c, bf_station_of(slot), (badge_t){66, y + 15, 11, 10});
  hourglass(c, 91, y + 15, last ? linen : COLOR(0xe3bb87));
  char number[12];
  snprintf(number, sizeof(number), "%d", (left + 999) / 1000);
  bf_center(c, BF_BODY, 111, y + 21, linen, number);
  int length = (left * 36 + BF_FRESH_SOON_MS - 1) / BF_FRESH_SOON_MS;
  bf_box(c, 82, y + 27, length, 1, last ? COLOR(0xffad85) : COLOR(0xcdb38c));
}

static void next_orders(bf_canvas_t c, const bf_game_t* g) {
  int expiring[BF_FRESH_QUEUE];
  int shown = bf_expiring(g, expiring);
  if (shown) {
    for (int i = 0; i < shown; ++i) expiry_ticket(c, g, expiring[i], i);
    return;
  }
  int order[BF_TABLES];
  int count = bf_next_tables(g, order);
  bf_layer(c, &bf_hud_next, (bf_rect_t){10, 7, 130, 42}, false);
  if (!count) {
    bf_layer(c, &bf_hud_plates, (bf_rect_t){27, 12, 30, 30}, false);
    bf_line(c, 71, 28, 77, 34, linen);
    bf_line(c, 77, 34, 90, 20, linen);
    return;
  }
  int station = order[0];
  int secs = (g->service[station].next_at_ms - g->elapsed_ms + 999) / 1000;
  bf_layer(c, servings[bf_next_dish(g, station)], (bf_rect_t){13, 7, 49, 39},
           false);
  seal(c, station, (badge_t){73, 28, 13, 12});
  bf_layer(c, &bf_hud_clock, (bf_rect_t){91, 17, 19, 19}, false);
  char value[12];
  snprintf(value, sizeof(value), "%d", secs);
  bf_center(c, BF_BODY, 121, 35, COLOR(0xffda93), value);
}
static void lower_hud(bf_canvas_t c, const bf_game_t* g) {
  bf_layer(c, &bf_hud_fullness, (bf_rect_t){312, 438, 176, 36}, false);
  bf_layer(c, &bf_hud_plates, (bf_rect_t){317, 443, 26, 24}, false);
  char text[20];
  snprintf(text, sizeof(text), "%d", g->eaten);
  bf_center(c, BF_BODY, 350, 463, linen, text);
  int gx = 365, gy = 444, gw = 117, gh = 22;
  bf_image(c, &bf_gauge, gx, gy, gw, gh);
  int fill = (int)(112 * fminf(1.f, g->gauge_milli / 160000.f));
  if (fill > 0)
    bf_blit(c, &bf_gauge_full, (bf_rect_t){72, 16, 2 * fill, 28},
            (bf_rect_t){gx + 27, gy + 6, (int)lroundf(fill * gw / 154.f), 11});
  if (g->fullness_milli > bf_rules.red_line * 1000) {
    int x = 449, y = 421;
    bf_ellipse(c, x + 10, y, 24, 13, COLOR(0x873d34), 255);
    bf_line(c, x - 10, y + 7, x - 2, y - 8, linen);
    bf_line(c, x - 2, y - 8, x + 6, y + 7, linen);
    bf_line(c, x - 10, y + 7, x + 6, y + 7, linen);
    bf_line(c, x - 2, y - 3, x - 2, y + 1, linen);
    bf_box(c, x - 3, y + 3, 2, 2, linen);
    snprintf(text, sizeof(text), "%d",
             (bf_rules.tolerance_ms - g->red_ms + 999) / 1000);
    bf_center(c, BF_BODY, x + 18, y + 6, linen, text);
  } else if (g->recovered_ms) {
    bf_ellipse(c, 460, 421, 11, 10, COLOR(0x426e62), 255);
    bf_line(c, 455, 421, 459, 425, linen);
    bf_line(c, 459, 425, 466, 416, linen);
  }
  bf_layer(c, &bf_hud_clock_bg, (bf_rect_t){636, 9, 91, 37}, false);
  bf_layer(c, &bf_hud_clock, (bf_rect_t){642, 14, 25, 25}, false);
  int sec = (bf_rules.duration_ms - g->elapsed_ms + 999) / 1000;
  snprintf(text, sizeof(text), "%d:%02d", sec / 60, sec % 60);
  bf_center(c, BF_HEAD, 693, 36, sec < 15 ? COLOR(0xffab83) : linen, text);
}
static void touch_controls(bf_canvas_t c, const bf_controls_t* controls,
                           bool can_eat) {
  const int x = BF_STICK_X, y = BF_CONTROL_Y;
  const uint16_t sea = COLOR(0x254c4d), trim = COLOR(0xc6ab7c);
  bf_ellipse(c, x, y + 3, 53, 53, COLOR(0x142c2c), 84);
  bf_ellipse(c, x, y, 52, 52, trim, 205);
  bf_ellipse(c, x, y, 49, 49, sea, 255);
  for (int i = 0; i < 4; i++) {
    int dx = i % 2 == 0 ? (i == 0 ? 1 : -1) : 0;
    int dy = i % 2 == 1 ? (i == 1 ? 1 : -1) : 0;
    bf_line(c, x + dx * 40, y + dy * 40, x + dx * 33 - dy * 4,
            y + dy * 33 + dx * 4, trim);
    bf_line(c, x + dx * 40, y + dy * 40, x + dx * 33 + dy * 4,
            y + dy * 33 - dx * 4, trim);
  }
  int knob_x = x + (int)lroundf(controls->stick.x * 29),
      knob_y = y + (int)lroundf(controls->stick.y * 29);
  bf_ellipse(c, knob_x, knob_y + 2, 18, 18, COLOR(0x182f2d), 180);
  bf_ellipse(c, knob_x, knob_y, 18, 18,
             controls->stick_held ? linen : COLOR(0xd8bd92), 255);
  bf_ellipse(c, knob_x, knob_y, 10, 10, COLOR(0x7c775f), 210);
  bf_ellipse(c, knob_x, knob_y, 8, 8, COLOR(0xe6caa0), 255);

  const int eat_x = BF_EAT_X;
  bool held = controls->eat_held, ready = can_eat || held;
  uint16_t rim = ready ? COLOR(0xf4d89e) : COLOR(0xa49e81);
  uint16_t fill = held ? COLOR(0x833f2d) : can_eat ? COLOR(0xb05636) : sea;
  uint16_t icon = ready ? linen : COLOR(0x9db0a0);
  int r = held ? 40 : 44;
  bf_ellipse(c, eat_x, y + 3, 45, 45, COLOR(0x142c2c), 84);
  bf_ellipse(c, eat_x, y, r, r, rim, 255);
  bf_ellipse(c, eat_x, y, r - 3, r - 3, fill, 255);
  /* Chopsticks above a rice bowl, with the same silhouette in all states. */
  bf_ellipse(c, eat_x, y + 10, 18, 11, icon, 255);
  bf_box(c, eat_x - 19, y - 2, 38, 7, fill);
  bf_box(c, eat_x - 18, y + 5, 36, 2, icon);
  bf_box(c, eat_x - 7, y + 23, 14, 2, icon);
  for (int i = 0; i < 2; i++) {
    int shift = 6 * i;
    bf_line(c, eat_x - 12 + shift, y - 3, eat_x + 13 + shift, y - 21, icon);
    bf_line(c, eat_x - 12 + shift, y - 2, eat_x + 13 + shift, y - 20, icon);
  }
}
void bf_paint_ground(bf_canvas_t c, bool dining) {
  if (dining) {
    bf_build_room(c);
    stations(c);
  } else
    bf_image(c, &bf_cloth, 0, 0, 800, 480);
}
static void plates(bf_canvas_t c, const bf_game_t* g, int near,
                   bool foreground) {
  for (int slot = 0; slot < BF_SLOTS; slot++) {
    int t = bf_station_of(slot);
    const bf_setting_t* setting = &g->settings[slot];
    bf_point_t p = bf_place(g, slot).pos;
    if ((p.y + (t == BF_LEAF ? 26 : 18) > g->player.pos.y) != foreground)
      continue;
    int x = (int)lroundf(p.x), y = (int)lroundf(p.y);
    int till = g->service[t].next_at_ms - g->elapsed_ms;
    if (setting->dish < 0) {
      if (slot == bf_next_setting(g, t) && g->service[t].next_at_ms >= 0 &&
          g->service[t].next_at_ms < bf_rules.duration_ms && till >= 0 &&
          till < 420)
        ring(c, x, y, 14, 8, COLOR(0xf7d28c));
      continue;
    }
    int age = g->elapsed_ms - setting->arrived_ms;
    int w = 42, h = 34;
    float drop = 0;
    if (!g->album.reduced_motion && setting->arrived_ms > 0 && age >= 0 &&
        age < 240) {
      float f = age / 240.f;
      drop = (1 - f) * (1 - f) * 7 - 1.5f * sinf(f * 3.14159f);
      w -= (int)(2 * (1 - f));
    }
    bf_layer(c, &bf_plate_shade, (bf_rect_t){x - w / 2, y - 21, w, h}, false);
    if (slot == near) {
      int extra = g->album.reduced_motion
                      ? 0
                      : (int)lroundf(1.3f * (1 + sinf(g->ambient_ms * .008f)));
      ring(c, x, y - 1, 18 + extra, 11 + extra / 2, COLOR(0x48766a));
      ring(c, x, y - 2, 18 + extra, 11 + extra / 2, COLOR(0xffe7a8));
    } else if (setting->arrived_ms > 0 && age >= 0 && age < 340 &&
               !g->album.reduced_motion)
      bf_layer(c, &bf_hud_arrival, (bf_rect_t){x - 21, y - 14, 42, 28}, false);
    bf_rect_t dst = {x - w / 2, y - 21 - (int)lroundf(drop), w, h};
    int left = bf_fresh_left(g, slot);
    if (left > 0 && left <= BF_FRESH_LAST_MS) {
      unsigned pulse = last_call_pulse(left, g->album.reduced_motion);
      bf_ellipse(c, x, y - 2, 18, 10, COLOR(0xc34533), 100 + pulse / 3);
      bf_layer_opacity(c, servings[setting->dish], dst, 248 - pulse / 4);
      /* Corners remain visible through the entire slow pulse. */
      for (int side = -1; side <= 1; side += 2) {
        int edge = x + 18 * side;
        bf_box(c, edge, y - 9, 2, 6, COLOR(0xffcc9d));
        bf_line(c, edge, y - 9, edge - 4 * side, y - 11, COLOR(0xffcc9d));
      }
    } else {
      bf_layer(c, servings[setting->dish], dst, false);
    }
  }
}
static void person(bf_canvas_t c, const bf_game_t* g) {
  const bf_actor_t* player = &g->player;
  int x = (int)lroundf(player->pos.x),
      y = (int)lroundf(player->pos.y -
                       bf_floor_height(bf_unproject(player->pos)));
  c.foot_depth = player->pos.y + 3;
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
  int tile_w = bf_cast.w / 9, tile_h = bf_cast.h / 4;
  bf_blit(c, &bf_cast,
          (bf_rect_t){pose * tile_w, (int)face * tile_h, tile_w, tile_h},
          (bf_rect_t){x - w / 2, y - BF_PERSON_FOOT + breath, w, h - breath});
}
static void bite(bf_canvas_t c, const bf_game_t* g) {
  if (!g->bite_ms || g->last_dish < 0) return;
  int age = 680 - g->bite_ms;
  if (age > 352) return;
  float f = fminf(1, age / 185.f), ease = f * f * (3 - 2 * f);
  bool left = g->player.face == BF_LEFT;
  float handx = g->player.pos.x + (left ? -5 : 5),
        handy = g->player.pos.y - bf_floor_height(bf_unproject(g->player.pos)) -
                BF_PERSON_MOUTH;
  float x = g->bite_source.x + (handx - g->bite_source.x) * ease,
        y = (g->bite_source.y - 7) * (1 - ease) + handy * ease;
  if (f < .69f)
    y -= 3 * sinf(f * 3.14159f);
  else
    y = fmaxf(handy, y);
  int w = (int)lroundf(18 - 11 * ease), h = (int)lroundf(10 - 6 * ease);
  bf_layer(c, bites[g->last_dish],
           (bf_rect_t){(int)lroundf(x) - w / 2, (int)lroundf(y) - h / 2, w, h},
           left);
}
void bf_paint_dining(bf_canvas_t c, const bf_game_t* g) {
  int near = bf_eat_target(g);
  bf_move_tracks(c, g);
  plates(c, g, near, false);
  person(c, g);
  plates(c, g, near, true);
  bite(c, g);
  next_orders(c, g);
  lower_hud(c, g);
  touch_controls(c, &g->controls, near >= 0);
}
