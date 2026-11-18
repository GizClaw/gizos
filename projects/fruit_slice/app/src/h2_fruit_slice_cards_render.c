#include "h2_fruit_slice_cards_render.h"

#define TR(value) h2_fruit_slice_translate(game->language, (value))

#include <math.h>
#include <stdio.h>
#include <string.h>

#define RGB H2_FRUIT_SLICE_RGB
#define PI H2_FRUIT_SLICE_PI

typedef enum card_side {
  SIDE_FACE = 0,
  SIDE_LOCKED,
  SIDE_BACK,
} card_side_t;

static const uint16_t k_silhouette = RGB(14, 12, 22);

static float clamp01(float value) {
  if (!(value > 0.0f)) return 0.0f;
  return value < 1.0f ? value : 1.0f;
}

static float ease_out_cubic(float t) {
  const float a = 1.0f - clamp01(t);
  return 1.0f - a * a * a;
}

static float ease_out_back(float t) {
  const float c1 = 1.70158f, c3 = c1 + 1.0f;
  const float a = clamp01(t) - 1.0f;
  return 1.0f + c3 * a * a * a + c1 * a * a;
}

static float ease_in_out(float t) {
  t = clamp01(t);
  return t < 0.5f ? 4.0f * t * t * t
                  : 1.0f - (-2.0f * t + 2.0f) * (-2.0f * t + 2.0f) *
                               (-2.0f * t + 2.0f) * 0.5f;
}

static uint32_t alpha_of(float fraction) {
  return (uint32_t)(clamp01(fraction) * 256.0f);
}

static unsigned popcount32(uint32_t value) {
  unsigned count = 0;
  for (; value != 0u; value &= value - 1u) ++count;
  return count;
}

static float min3(float a, float b, float c) { return fminf(a, fminf(b, c)); }
static float max3(float a, float b, float c) { return fmaxf(a, fmaxf(b, c)); }

/* Filled triangle with horizontal edge coverage. */
static void triangle(h2_fruit_slice_canvas_t* canvas, float x0, float y0,
                     float x1, float y1, float x2, float y2, uint16_t color,
                     uint32_t alpha) {
  h2_fruit_slice_region_t g;
  if (!h2_fruit_slice_canvas_region(
          canvas, min3(x0, x1, x2) - 1.0f, min3(y0, y1, y2) - 1.0f,
          max3(x0, x1, x2) + 1.0f, max3(y0, y1, y2) + 1.0f, &g))
    return;
  const float xs[3] = {x0, x1, x2}, ys[3] = {y0, y1, y2};
  for (int32_t py = g.y0; py < g.y1; ++py) {
    const float yc = (float)py + 0.5f;
    float lo = 1e30f, hi = -1e30f;
    for (unsigned e = 0; e < 3u; ++e) {
      const float ax = xs[e], ay = ys[e];
      const float bx = xs[(e + 1u) % 3u], by = ys[(e + 1u) % 3u];
      if ((yc < ay) == (yc < by)) continue;
      const float x = ax + (yc - ay) * (bx - ax) / (by - ay);
      lo = fminf(lo, x);
      hi = fmaxf(hi, x);
    }
    if (!(hi > lo)) continue;
    int32_t a = (int32_t)floorf(lo), b = (int32_t)ceilf(hi);
    if (a < g.x0) a = g.x0;
    if (b > g.x1) b = g.x1;
    uint16_t* row = h2_fruit_slice_canvas_row(canvas, py);
    for (int32_t px = a; px < b; ++px) {
      const float cover =
          clamp01(fminf((float)px + 1.0f, hi) - fmaxf((float)px, lo));
      row[px] = h2_fruit_slice_blend(row[px], color,
                                     (uint32_t)(cover * (float)alpha));
    }
  }
}

/* Anti-aliased annulus; only ring pixels are visited. */
static void ring(h2_fruit_slice_canvas_t* canvas, float cx, float cy, float r,
                 float thickness, uint16_t color, uint32_t alpha) {
  if (!(r > 0.0f) || !(thickness > 0.0f) || alpha == 0u) return;
  const float outer = r + thickness * 0.5f;
  const float inner = fmaxf(0.0f, r - thickness * 0.5f);
  h2_fruit_slice_region_t g;
  if (!h2_fruit_slice_canvas_region(canvas, cx - outer - 1.0f,
                                    cy - outer - 1.0f, cx + outer + 1.0f,
                                    cy + outer + 1.0f, &g))
    return;
  const float o2 = (outer + 0.5f) * (outer + 0.5f);
  const float i2 = inner > 0.5f ? (inner - 0.5f) * (inner - 0.5f) : 0.0f;
  for (int32_t py = g.y0; py < g.y1; ++py) {
    const float dy = (float)py + 0.5f - cy, dy2 = dy * dy;
    if (dy2 >= o2) continue;
    const float so = sqrtf(o2 - dy2);
    const float si = dy2 < i2 ? sqrtf(i2 - dy2) : 0.0f;
    uint16_t* row = h2_fruit_slice_canvas_row(canvas, py);
    int32_t spans[2][2] = {{(int32_t)floorf(cx - so), (int32_t)ceilf(cx - si)},
                           {(int32_t)floorf(cx + si), (int32_t)ceilf(cx + so)}};
    if (spans[1][0] < spans[0][1]) spans[1][0] = spans[0][1];
    for (unsigned s = 0; s < 2u; ++s) {
      int32_t a = spans[s][0], b = spans[s][1];
      if (a < g.x0) a = g.x0;
      if (b > g.x1) b = g.x1;
      for (int32_t px = a; px < b; ++px) {
        const float dx = (float)px + 0.5f - cx;
        const float d = sqrtf(dx * dx + dy2);
        const float cover =
            clamp01(fminf(outer + 0.5f - d, d - (inner - 0.5f)));
        if (cover > 0.0f)
          row[px] = h2_fruit_slice_blend(row[px], color,
                                         (uint32_t)(cover * (float)alpha));
      }
    }
  }
}

/* A soft diagonal light band crossing a rectangle; progress is 0..1. */
static void shimmer(h2_fruit_slice_canvas_t* canvas, float x0, float y0,
                    float x1, float y1, float progress, float band,
                    uint32_t alpha) {
  h2_fruit_slice_region_t g;
  if (!(band > 0.5f) ||
      !h2_fruit_slice_canvas_region(canvas, x0, y0, x1, y1, &g))
    return;
  const float slope = 0.45f;
  const float travel = (x1 - x0) + (y1 - y0) * slope + 2.0f * band;
  const float top = x0 - band + travel * clamp01(progress);
  const uint16_t white = RGB(255, 255, 255);
  for (int32_t py = g.y0; py < g.y1; ++py) {
    const float c = top - ((float)py + 0.5f - y0) * slope;
    int32_t a = (int32_t)floorf(c - band), b = (int32_t)ceilf(c + band);
    if (a < g.x0) a = g.x0;
    if (b > g.x1) b = g.x1;
    uint16_t* row = h2_fruit_slice_canvas_row(canvas, py);
    for (int32_t px = a; px < b; ++px) {
      const float k = 1.0f - fabsf((float)px + 0.5f - c) / band;
      if (k > 0.0f)
        row[px] =
            h2_fruit_slice_blend(row[px], white, (uint32_t)(k * k * alpha));
    }
  }
}

static void star(h2_fruit_slice_canvas_t* canvas, float cx, float cy,
                 float outer, float rotation, uint16_t color, uint32_t alpha) {
  const float inner = outer * 0.45f;
  float ox[5], oy[5], ix[5], iy[5];
  for (unsigned i = 0; i < 5u; ++i) {
    const float a = rotation - PI * 0.5f + (float)i * (2.0f * PI / 5.0f);
    ox[i] = cx + cosf(a) * outer;
    oy[i] = cy + sinf(a) * outer;
    ix[i] = cx + cosf(a + PI / 5.0f) * inner;
    iy[i] = cy + sinf(a + PI / 5.0f) * inner;
  }
  for (unsigned i = 0; i < 5u; ++i) {
    const unsigned prev = (i + 4u) % 5u;
    triangle(canvas, ox[i], oy[i], ix[i], iy[i], cx, cy, color, alpha);
    triangle(canvas, ox[i], oy[i], ix[prev], iy[prev], cx, cy, color, alpha);
  }
  h2_fruit_slice_draw_disc(canvas, cx, cy, inner * 0.75f, color, alpha);
}

static uint16_t category_tint(unsigned category) {
  switch (category) {
    case H2_FRUIT_SLICE_CARD_TAB_WEAPON:
      return RGB(36, 64, 104);
    case H2_FRUIT_SLICE_CARD_TAB_BOMB:
      return RGB(98, 34, 34);
    case H2_FRUIT_SLICE_CARD_TAB_SKILL:
      return RGB(74, 42, 112);
    default:
      return RGB(40, 98, 54);
  }
}

static uint16_t lighten(uint16_t color, uint32_t amount) {
  return h2_fruit_slice_blend(color, RGB(255, 255, 255), amount);
}

/* Flat color used for small icons and flights. */
static uint16_t icon_color(unsigned id) {
  if (id < H2_FRUIT_SLICE_FRUIT_KINDS)
    return h2_fruit_slice_game_juice((h2_fruit_slice_kind_t)id);
  switch (id) {
    case H2_FRUIT_SLICE_CARD_FRUIT_SALAD:
      return RGB(255, 140, 60);
    case H2_FRUIT_SLICE_CARD_CLASSIC_KNIFE:
    case H2_FRUIT_SLICE_CARD_JADE_KATANA:
    case H2_FRUIT_SLICE_CARD_FLAME_AXE:
      return h2_fruit_slice_weapon(id - H2_FRUIT_SLICE_CARD_CLASSIC_KNIFE)
          ->color;
    case H2_FRUIT_SLICE_CARD_BOMB:
    case H2_FRUIT_SLICE_CARD_CLOSE_CALL:
    case H2_FRUIT_SLICE_CARD_BOMB_SQUAD:
      return RGB(70, 74, 88);
    default:
      return RGB(255, 206, 64);
  }
}

static void weapon_art(h2_fruit_slice_canvas_t* canvas, unsigned weapon,
                       float cx, float cy, float r, bool silhouette,
                       uint64_t time_ms) {
  const float dx = 0.7071f, dy = -0.7071f; /* Toward the top-right. */
  const float nx = 0.7071f, ny = 0.7071f;
#define AT(along, across)                  \
  cx + (along) * r *dx + (across) * r *nx, \
      cy + (along) * r *dy + (across) * r *ny
  const uint16_t glow = h2_fruit_slice_weapon(weapon)->color;
  if (weapon == 0u) {
    if (!silhouette)
      h2_fruit_slice_draw_capsule(canvas, AT(-0.2f, 0.0f), AT(0.95f, 0.0f),
                                  0.30f * r, glow, 60u);
    h2_fruit_slice_draw_capsule(
        canvas, AT(-0.2f, 0.0f), AT(0.95f, 0.0f), 0.17f * r,
        silhouette ? k_silhouette : RGB(214, 222, 232), 256u);
    if (!silhouette)
      h2_fruit_slice_draw_capsule(canvas, AT(-0.1f, -0.08f), AT(0.85f, -0.08f),
                                  0.04f * r, RGB(255, 255, 255), 210u);
    h2_fruit_slice_draw_capsule(
        canvas, AT(-0.25f, -0.3f), AT(-0.25f, 0.3f), 0.07f * r,
        silhouette ? k_silhouette : RGB(150, 150, 164), 256u);
    h2_fruit_slice_draw_capsule(
        canvas, AT(-0.3f, 0.0f), AT(-0.95f, 0.0f), 0.12f * r,
        silhouette ? k_silhouette : RGB(122, 74, 40), 256u);
  } else if (weapon == 1u) {
    if (!silhouette)
      h2_fruit_slice_draw_capsule(canvas, AT(-0.35f, 0.0f), AT(1.08f, 0.0f),
                                  0.26f * r, glow, 80u);
    h2_fruit_slice_draw_capsule(
        canvas, AT(-0.35f, 0.0f), AT(1.08f, 0.0f), 0.11f * r,
        silhouette ? k_silhouette : RGB(170, 248, 204), 256u);
    if (!silhouette)
      h2_fruit_slice_draw_capsule(canvas, AT(-0.25f, -0.05f), AT(1.0f, -0.05f),
                                  0.03f * r, RGB(255, 255, 255), 220u);
    h2_fruit_slice_draw_disc(canvas, AT(-0.38f, 0.0f), 0.17f * r,
                             silhouette ? k_silhouette : RGB(232, 190, 70),
                             256u);
    h2_fruit_slice_draw_capsule(
        canvas, AT(-0.45f, 0.0f), AT(-1.05f, 0.0f), 0.1f * r,
        silhouette ? k_silhouette : RGB(44, 38, 56), 256u);
    if (!silhouette)
      for (unsigned i = 0; i < 3u; ++i)
        h2_fruit_slice_draw_disc(canvas, AT(-0.58f - 0.17f * (float)i, 0.0f),
                                 0.05f * r, RGB(220, 210, 236), 200u);
  } else {
    h2_fruit_slice_draw_capsule(
        canvas, AT(-0.95f, 0.0f), AT(0.85f, 0.0f), 0.09f * r,
        silhouette ? k_silhouette : RGB(132, 84, 46), 256u);
    if (!silhouette) {
      /* Flickering flames lick off the axe head. */
      const float flicker = (float)(time_ms % 360u) / 360.0f * 2.0f * PI;
      for (unsigned i = 0; i < 3u; ++i) {
        const float k = sinf(flicker + (float)i * 2.1f);
        h2_fruit_slice_draw_disc(
            canvas, AT(0.62f + 0.18f * (float)i, 0.55f + 0.12f * k),
            (0.16f + 0.04f * k) * r,
            i == 1u ? RGB(255, 226, 90) : RGB(255, 132, 36), 200u);
      }
      h2_fruit_slice_draw_disc(canvas, AT(0.5f, 0.32f), 0.52f * r, glow, 70u);
    }
    h2_fruit_slice_draw_disc(canvas, AT(0.5f, 0.3f), 0.42f * r,
                             silhouette ? k_silhouette : RGB(214, 84, 34),
                             256u);
    if (!silhouette) {
      h2_fruit_slice_draw_disc(canvas, AT(0.5f, 0.42f), 0.28f * r,
                               RGB(255, 168, 70), 256u);
      h2_fruit_slice_draw_disc(canvas, AT(0.5f, 0.52f), 0.14f * r,
                               RGB(255, 236, 170), 230u);
    }
  }
#undef AT
}

static void bomb_art(h2_fruit_slice_canvas_t* canvas, float cx, float cy,
                     float r, bool silhouette, float angle, uint64_t time_ms) {
  if (silhouette) {
    h2_fruit_slice_draw_disc(canvas, cx, cy, r, k_silhouette, 256u);
    h2_fruit_slice_draw_capsule(canvas, cx + 0.2f * r, cy - 0.85f * r,
                                cx + 0.42f * r, cy - 1.22f * r, 0.08f * r,
                                k_silhouette, 256u);
    return;
  }
  h2_fruit_slice_draw_fruit(canvas, H2_FRUIT_SLICE_BOMB, cx, cy, r, angle,
                            time_ms);
}

static void mini_bomb(h2_fruit_slice_canvas_t* canvas, float cx, float cy,
                      float r, bool silhouette) {
  h2_fruit_slice_draw_disc(canvas, cx, cy, r,
                           silhouette ? k_silhouette : RGB(52, 56, 68), 256u);
  if (silhouette) return;
  h2_fruit_slice_draw_disc(canvas, cx - 0.35f * r, cy - 0.35f * r, 0.3f * r,
                           RGB(120, 128, 150), 160u);
  h2_fruit_slice_draw_disc(canvas, cx + 0.3f * r, cy - 1.0f * r, 0.22f * r,
                           RGB(255, 190, 60), 230u);
}

/* Card illustration centered in the art window; r is a prepared sprite
 * radius for fruit and bombs. */
static void card_art(h2_fruit_slice_canvas_t* canvas,
                     const h2_fruit_slice_game_t* game, unsigned id, float cx,
                     float cy, float r, bool silhouette, float angle) {
  const uint64_t now = game->cards.time_ms;
  const float u = game->unit;
  if (id < H2_FRUIT_SLICE_FRUIT_KINDS) {
    if (silhouette)
      h2_fruit_slice_draw_disc(canvas, cx, cy, r, k_silhouette, 256u);
    else
      h2_fruit_slice_draw_fruit(canvas, (h2_fruit_slice_kind_t)id, cx, cy, r,
                                angle, now);
    return;
  }
  char label[8];
  switch (id) {
    case H2_FRUIT_SLICE_CARD_FRUIT_SALAD: {
      static const float fruit[3][3] = {{-0.48f, -0.14f, 0.42f},
                                        {0.06f, -0.34f, 0.40f},
                                        {0.52f, -0.12f, 0.34f}};
      static const h2_fruit_slice_kind_t kinds[3] = {H2_FRUIT_SLICE_WATERMELON,
                                                     H2_FRUIT_SLICE_ORANGE,
                                                     H2_FRUIT_SLICE_KIWI};
      for (unsigned i = 0; i < 3u; ++i) {
        const float fx = cx + fruit[i][0] * r, fy = cy + fruit[i][1] * r;
        h2_fruit_slice_draw_disc(
            canvas, fx, fy, fruit[i][2] * r,
            silhouette ? k_silhouette : h2_fruit_slice_game_juice(kinds[i]),
            256u);
        if (!silhouette)
          h2_fruit_slice_draw_disc(canvas, fx - 0.12f * r, fy - 0.12f * r,
                                   fruit[i][2] * 0.35f * r, RGB(255, 255, 255),
                                   120u);
      }
      h2_fruit_slice_draw_rounded_rect(
          canvas, cx - 0.9f * r, cy, cx + 0.9f * r, cy + 0.72f * r, 0.36f * r,
          silhouette ? k_silhouette : RGB(152, 98, 54), 256u);
      h2_fruit_slice_draw_capsule(canvas, cx - 0.95f * r, cy + 0.02f * r,
                                  cx + 0.95f * r, cy + 0.02f * r, 0.09f * r,
                                  silhouette ? k_silhouette : RGB(206, 150, 92),
                                  256u);
      return;
    }
    case H2_FRUIT_SLICE_CARD_CLASSIC_KNIFE:
    case H2_FRUIT_SLICE_CARD_JADE_KATANA:
    case H2_FRUIT_SLICE_CARD_FLAME_AXE:
      weapon_art(canvas, id - H2_FRUIT_SLICE_CARD_CLASSIC_KNIFE, cx, cy, r,
                 silhouette, now);
      return;
    case H2_FRUIT_SLICE_CARD_BOMB:
      bomb_art(canvas, cx, cy + 0.12f * r, r, silhouette, angle * 0.3f, now);
      return;
    case H2_FRUIT_SLICE_CARD_CLOSE_CALL:
      for (unsigned i = 0; i < 3u; ++i) {
        const float y = cy + (-0.45f + 0.45f * (float)i) * r;
        h2_fruit_slice_draw_capsule(
            canvas, cx - 1.45f * r, y, cx - (0.8f - 0.1f * (float)i) * r, y,
            0.06f * r, silhouette ? k_silhouette : RGB(240, 244, 255),
            silhouette ? 256u : 190u);
      }
      bomb_art(canvas, cx + 0.32f * r, cy + 0.12f * r, r, silhouette, angle,
               now);
      return;
    case H2_FRUIT_SLICE_CARD_BOMB_SQUAD:
      mini_bomb(canvas, cx - 1.0f * r, cy + 0.3f * r, 0.52f * r, silhouette);
      mini_bomb(canvas, cx + 1.0f * r, cy + 0.3f * r, 0.52f * r, silhouette);
      bomb_art(canvas, cx, cy + 0.12f * r, r, silhouette, angle, now);
      return;
    case H2_FRUIT_SLICE_CARD_COMBO:
    case H2_FRUIT_SLICE_CARD_MEGA_COMBO: {
      const bool mega = id == H2_FRUIT_SLICE_CARD_MEGA_COMBO;
      const unsigned slashes = mega ? 5u : 3u;
      for (unsigned i = 0; i < slashes; ++i) {
        const float off = ((float)i - (float)(slashes - 1u) * 0.5f) *
                          (mega ? 0.34f : 0.44f) * r;
        if (!silhouette)
          h2_fruit_slice_draw_capsule(
              canvas, cx - 0.9f * r + off, cy + 0.9f * r, cx + 0.5f * r + off,
              cy - 0.9f * r, 0.16f * r,
              mega ? RGB(214, 120, 255) : RGB(110, 214, 255), 90u);
        h2_fruit_slice_draw_capsule(
            canvas, cx - 0.9f * r + off, cy + 0.9f * r, cx + 0.5f * r + off,
            cy - 0.9f * r, 0.05f * r,
            silhouette ? k_silhouette : RGB(255, 255, 255), 240u);
      }
      if (silhouette) return;
      (void)snprintf(label, sizeof(label), "%ux", mega ? 5u : 3u);
      const int32_t scale = h2_fruit_slice_fit_scale(
          label, 1.6f * r, (int32_t)fmaxf(1.0f, r * 0.11f));
      h2_fruit_slice_draw_text_centered(
          canvas, label, cx + 0.35f * r, cy + 0.2f * r, scale,
          mega ? RGB(255, 214, 64) : RGB(255, 236, 160), 256u);
      return;
    }
    case H2_FRUIT_SLICE_CARD_HIGH_SCORE:
    case H2_FRUIT_SLICE_CARD_LEGEND: {
      const bool legend = id == H2_FRUIT_SLICE_CARD_LEGEND;
      if (legend) {
        const uint16_t gold = silhouette ? k_silhouette : RGB(255, 200, 40);
        triangle(canvas, cx - 0.95f * r, cy + 0.2f * r, cx - 0.7f * r,
                 cy - 0.7f * r, cx - 0.3f * r, cy + 0.2f * r, gold, 256u);
        triangle(canvas, cx - 0.4f * r, cy + 0.2f * r, cx, cy - 0.95f * r,
                 cx + 0.4f * r, cy + 0.2f * r, gold, 256u);
        triangle(canvas, cx + 0.3f * r, cy + 0.2f * r, cx + 0.7f * r,
                 cy - 0.7f * r, cx + 0.95f * r, cy + 0.2f * r, gold, 256u);
        h2_fruit_slice_draw_rounded_rect(canvas, cx - 0.95f * r, cy + 0.1f * r,
                                         cx + 0.95f * r, cy + 0.62f * r,
                                         0.1f * r, gold, 256u);
        if (!silhouette) {
          h2_fruit_slice_draw_rounded_rect(
              canvas, cx - 0.95f * r, cy + 0.1f * r, cx + 0.95f * r,
              cy + 0.24f * r, 0.06f * r, RGB(255, 236, 150), 200u);
          static const float gems[3][2] = {
              {-0.7f, -0.7f}, {0.0f, -0.95f}, {0.7f, -0.7f}};
          static const uint16_t colors[3] = {
              RGB(80, 160, 255), RGB(255, 60, 80), RGB(90, 230, 120)};
          for (unsigned i = 0; i < 3u; ++i)
            h2_fruit_slice_draw_disc(canvas, cx + gems[i][0] * r,
                                     cy + gems[i][1] * r, 0.12f * r, colors[i],
                                     256u);
          h2_fruit_slice_draw_disc(canvas, cx, cy + 0.4f * r, 0.13f * r,
                                   RGB(255, 60, 80), 256u);
          shimmer(canvas, cx - r, cy - r, cx + r, cy + 0.7f * r,
                  (float)(now % 1600u) / 1100.0f, 0.25f * r, 150u);
        }
        return;
      }
      if (!silhouette)
        star(canvas, cx, cy, 1.15f * r, angle * 0.4f, RGB(130, 200, 255), 70u);
      star(canvas, cx, cy, 0.98f * r, angle * 0.4f,
           silhouette ? k_silhouette : RGB(226, 234, 246), 256u);
      if (silhouette) return;
      const int32_t scale = h2_fruit_slice_fit_scale(
          "30", 0.8f * r, (int32_t)fmaxf(1.0f, r * 0.09f));
      h2_fruit_slice_draw_text_centered(canvas, "30", cx,
                                        cy - (float)scale * 3.0f, scale,
                                        RGB(36, 60, 120), 256u);
      return;
    }
    default:
      (void)u;
      return;
  }
}

/* One card. sx squeezes it horizontally for flips; content is drawn only
 * while the card is nearly face-on. */
static void draw_card(h2_fruit_slice_canvas_t* canvas,
                      const h2_fruit_slice_game_t* game, unsigned id,
                      card_side_t side, float cx, float cy, float w, float h,
                      float sx, float art_r, float angle, bool badges) {
  const float hw = w * 0.5f * sx, hh = h * 0.5f;
  if (!(hw >= 1.0f) || !(hh >= 1.0f)) return;
  const h2_fruit_slice_card_t* card = h2_fruit_slice_card(id);
  const uint16_t rc = h2_fruit_slice_card_rarity_color(card->rarity);
  const float x0 = cx - hw, x1 = cx + hw, y0 = cy - hh, y1 = cy + hh;
  const float corner = fminf(w * 0.08f, hw);
  const float b = fmaxf(1.5f, w * 0.045f), bx = b * sx;
  const uint16_t frame = side == SIDE_FACE     ? rc
                         : side == SIDE_LOCKED ? RGB(74, 76, 92)
                                               : RGB(216, 170, 66);
  /* Drop shadow grounds the card on the wood. */
  h2_fruit_slice_draw_rounded_rect(canvas, x0 + b, y0 + b * 1.5f, x1 + b,
                                   y1 + b * 1.5f, corner, RGB(0, 0, 0), 90u);
  h2_fruit_slice_draw_rounded_rect(canvas, x0, y0, x1, y1, corner, frame, 256u);
  if (side == SIDE_FACE && card->rarity >= H2_FRUIT_SLICE_RARITY_RARE)
    h2_fruit_slice_draw_rounded_rect(canvas, x0 + bx * 0.45f, y0 + b * 0.45f,
                                     x1 - bx * 0.45f, y1 - b * 0.45f, corner,
                                     lighten(rc, 150u), 220u);
  const uint16_t inner = side == SIDE_FACE     ? RGB(30, 22, 38)
                         : side == SIDE_LOCKED ? RGB(30, 30, 40)
                                               : RGB(26, 34, 82);
  h2_fruit_slice_draw_rounded_rect(canvas, x0 + bx, y0 + b, x1 - bx, y1 - b,
                                   fmaxf(1.0f, corner - bx), inner, 256u);
  if (side == SIDE_BACK) {
    const float er = fminf(hw, hh) * 0.62f;
    if (sx > 0.25f) {
      const uint16_t gold = RGB(220, 176, 72);
      h2_fruit_slice_draw_capsule(canvas, x0 + 2.0f * bx, y0 + 2.0f * b,
                                  x1 - 2.0f * bx, y1 - 2.0f * b,
                                  fmaxf(0.8f, w * 0.012f), gold, 70u);
      h2_fruit_slice_draw_capsule(canvas, x1 - 2.0f * bx, y0 + 2.0f * b,
                                  x0 + 2.0f * bx, y1 - 2.0f * b,
                                  fmaxf(0.8f, w * 0.012f), gold, 70u);
      h2_fruit_slice_draw_disc(canvas, cx, cy, er, RGB(44, 56, 128), 256u);
      ring(canvas, cx, cy, er, fmaxf(1.2f, w * 0.03f), gold, 256u);
      ring(canvas, cx, cy, er * 0.72f, fmaxf(0.8f, w * 0.012f), gold, 150u);
    }
    if (sx > 0.8f) {
      const int32_t scale = h2_fruit_slice_fit_scale(
          "?", er * 1.1f, (int32_t)fmaxf(1.0f, er * 0.16f));
      h2_fruit_slice_draw_text_centered(canvas, "?", cx,
                                        cy - (float)scale * 3.5f, scale,
                                        RGB(255, 222, 110), 256u);
    }
    return;
  }
  const float wx0 = x0 + 2.0f * bx, wx1 = x1 - 2.0f * bx;
  const float wy0 = y0 + 2.0f * b, wy1 = y0 + h * 0.62f;
  const uint16_t tint =
      side == SIDE_FACE ? category_tint(card->category) : RGB(42, 42, 54);
  h2_fruit_slice_draw_rounded_rect(canvas, wx0, wy0, wx1, wy1,
                                   fmaxf(1.0f, w * 0.05f * sx), tint, 256u);
  const float acx = cx, acy = (wy0 + wy1) * 0.5f;
  if (side == SIDE_FACE)
    h2_fruit_slice_draw_disc(canvas, acx, acy,
                             fminf(art_r * 1.35f, (wx1 - wx0) * 0.5f),
                             lighten(tint, 70u), 200u);
  if (sx < 0.86f) return;
  card_art(canvas, game, id, acx, acy, art_r, side != SIDE_FACE, angle);
  if (w >= 34.0f) {
    const char* name = side == SIDE_FACE ? TR(card->name) : "???";
    /* Bitmap text cannot shrink below one pixel per stroke. Keep tiny card
     * labels within their border; the detail panel retains the full name. */
    static const char* const compact[H2_FRUIT_SLICE_CARD_COUNT] = {
        "MELON", "ORNG", "APPLE", "KIWI",  "LEMON", "SALAD", "KNIFE", "JADE",
        "AXE",   "BOMB", "DODGE", "SQUAD", "COMBO", "MEGA",  "SCORE", "CROWN"};
    if (side == SIDE_FACE && h2_fruit_slice_text_width(name, 1) > w * 0.88f)
      name = TR(compact[id]);
    const int32_t scale = h2_fruit_slice_fit_scale(
        name, w * 0.88f, (int32_t)fmaxf(1.0f, floorf(w / 56.0f)));
    h2_fruit_slice_draw_text_centered(
        canvas, name, cx, y0 + h * 0.64f, scale,
        side == SIDE_FACE ? RGB(248, 242, 228) : RGB(130, 130, 146), 256u);
  }
  const unsigned gems = card->rarity + 1u;
  const float gap = w * 0.11f, gem_r = fmaxf(1.2f, w * 0.032f);
  for (unsigned i = 0; i < gems; ++i) {
    const float gx = cx + ((float)i - (float)(gems - 1u) * 0.5f) * gap;
    h2_fruit_slice_draw_disc(canvas, gx, y0 + h * 0.93f, gem_r,
                             side == SIDE_FACE ? rc : RGB(84, 84, 98), 256u);
    if (side == SIDE_FACE)
      h2_fruit_slice_draw_disc(canvas, gx - gem_r * 0.3f,
                               y0 + h * 0.93f - gem_r * 0.3f, gem_r * 0.4f,
                               RGB(255, 255, 255), 170u);
  }
  if (!badges || side != SIDE_FACE) return;
  if (w < 34.0f) {
    /* Tiny cards retain a dot; the selected detail keeps full counts/text. */
    if ((game->cards.unseen & (1u << id)) != 0u)
      h2_fruit_slice_draw_disc(canvas, x1 - 2.0f, y0 + 2.0f, 2.0f,
                               RGB(226, 40, 56), 256u);
    return;
  }
  const int32_t small = (int32_t)fmaxf(1.0f, floorf(w / 70.0f));
  const float pill_h =
      (float)h2_fruit_slice_text_height(TR("NEW"), small) + 4.0f;
  const unsigned count = game->cards.counts[id];
  if (count > 1u) {
    char text[8];
    (void)snprintf(text, sizeof(text), "x%u", count);
    const float tw = (float)h2_fruit_slice_text_width(text, small);
    h2_fruit_slice_draw_rounded_rect(canvas, x1 - tw - 8.0f, y0 - pill_h * 0.4f,
                                     x1 + 2.0f, y0 + pill_h * 0.6f,
                                     pill_h * 0.5f, RGB(20, 14, 10), 235u);
    h2_fruit_slice_draw_text(canvas, text, (int32_t)(x1 - tw - 3.0f),
                             (int32_t)(y0 - pill_h * 0.4f + 2.0f), small,
                             RGB(255, 222, 80), 256u);
  }
  if ((game->cards.unseen & (1u << id)) != 0u) {
    const float tw = (float)h2_fruit_slice_text_width(TR("NEW"), small);
    h2_fruit_slice_draw_rounded_rect(canvas, x0 - 2.0f, y0 - pill_h * 0.4f,
                                     x0 + tw + 8.0f, y0 + pill_h * 0.6f,
                                     pill_h * 0.5f, RGB(226, 40, 56), 256u);
    h2_fruit_slice_draw_text(canvas, TR("NEW"), (int32_t)(x0 + 3.0f),
                             (int32_t)(y0 - pill_h * 0.4f + 2.0f), small,
                             RGB(255, 255, 255), 256u);
  }
}

/* Small card used in flights and the HUD badge: no textures. */
static void mini_card(h2_fruit_slice_canvas_t* canvas, unsigned id,
                      card_side_t side, float cx, float cy, float w, float h,
                      float sx) {
  const float hw = w * 0.5f * sx, hh = h * 0.5f;
  if (!(hw >= 0.75f) || !(hh >= 1.0f)) return;
  const uint16_t rc =
      h2_fruit_slice_card_rarity_color(h2_fruit_slice_card(id)->rarity);
  const float corner = fminf(w * 0.12f, hw);
  const float b = fmaxf(1.0f, w * 0.07f), bx = b * sx;
  h2_fruit_slice_draw_rounded_rect(
      canvas, cx - hw, cy - hh, cx + hw, cy + hh, corner,
      side == SIDE_BACK ? RGB(216, 170, 66) : rc, 256u);
  h2_fruit_slice_draw_rounded_rect(
      canvas, cx - hw + bx, cy - hh + b, cx + hw - bx, cy + hh - b,
      fmaxf(1.0f, corner - bx),
      side == SIDE_BACK ? RGB(26, 34, 82)
                        : category_tint(h2_fruit_slice_card(id)->category),
      256u);
  const float r = fminf(hw, hh) * 0.5f;
  if (side == SIDE_BACK) {
    ring(canvas, cx, cy, r, fmaxf(1.0f, w * 0.06f), RGB(220, 176, 72), 256u);
    return;
  }
  h2_fruit_slice_draw_disc(canvas, cx, cy, r, icon_color(id), 256u);
  h2_fruit_slice_draw_disc(canvas, cx - r * 0.3f, cy - r * 0.3f, r * 0.38f,
                           RGB(255, 255, 255), 150u);
}

static void draw_sparks(h2_fruit_slice_canvas_t* canvas,
                        const h2_fruit_slice_game_t* game) {
  const float u = game->unit;
  for (unsigned i = 0; i < H2_FRUIT_SLICE_CARD_SPARKS; ++i) {
    const h2_fruit_slice_card_spark_t* s = &game->cards.sparks[i];
    if (!s->active || s->life_ms == 0u) continue;
    const float left = 1.0f - (float)s->age_ms / (float)s->life_ms;
    const float size = s->size * (0.45f + 0.55f * left);
    const uint32_t alpha = alpha_of(left * 1.4f);
    h2_fruit_slice_draw_disc(canvas, s->x, s->y, size, s->color, alpha);
    if (size >= 2.0f * u) {
      const float arm = size * 2.6f, thin = fmaxf(0.6f, size * 0.22f);
      h2_fruit_slice_draw_capsule(canvas, s->x - arm, s->y, s->x + arm, s->y,
                                  thin, RGB(255, 255, 255), alpha * 3u / 4u);
      h2_fruit_slice_draw_capsule(canvas, s->x, s->y - arm, s->x, s->y + arm,
                                  thin, RGB(255, 255, 255), alpha * 3u / 4u);
    }
  }
}

static void halo(h2_fruit_slice_canvas_t* canvas, float cx, float cy, float w,
                 float h, uint16_t color, float strength, float u) {
  for (unsigned k = 0; k < 3u; ++k) {
    const float pad = (4.0f + 5.0f * (float)k) * u * (0.8f + 0.4f * strength);
    const uint32_t alpha =
        (uint32_t)((float)(78u - 22u * k) * clamp01(strength));
    h2_fruit_slice_draw_rounded_rect(
        canvas, cx - w * 0.5f - pad, cy - h * 0.5f - pad, cx + w * 0.5f + pad,
        cy + h * 0.5f + pad, w * 0.08f + pad, color, alpha);
  }
}

bool h2_fruit_slice_cards_prepare_sprites(h2_fruit_slice_sprites_t* sprites,
                                          const h2_fruit_slice_game_t* game) {
  h2_fruit_slice_album_layout_t album;
  h2_fruit_slice_reveal_layout_t reveal;
  h2_fruit_slice_cards_album_layout(game->width, game->height, game->unit,
                                    &album);
  h2_fruit_slice_cards_reveal_layout(game->width, game->height, &reveal);
  bool ok = sprites != NULL;
  for (unsigned k = 0; ok && k < H2_FRUIT_SLICE_KIND_COUNT; ++k) {
    const h2_fruit_slice_kind_t kind = (h2_fruit_slice_kind_t)k;
    ok =
        h2_fruit_slice_sprites_prepare_fruit(sprites, kind, album.art_radius) &&
        h2_fruit_slice_sprites_prepare_fruit(sprites, kind, reveal.art_radius);
  }
  return ok;
}

#define BADGE_Y(u) (30.0f * (u))

void h2_fruit_slice_cards_render_badge(h2_fruit_slice_canvas_t* canvas,
                                       const h2_fruit_slice_game_t* game) {
  const unsigned landed = h2_fruit_slice_cards_run_landed(&game->cards);
  if (game->phase != H2_FRUIT_SLICE_PHASE_PLAYING || landed == 0u) return;
  const float u = game->unit;
  const float cx = (float)canvas->width * 0.5f - 12.0f * u;
  const float cy = BADGE_Y(u);
  const float w = 18.0f * u, h = 25.0f * u;
  h2_fruit_slice_draw_rounded_rect(
      canvas, cx - w * 0.5f + 4.0f * u, cy - h * 0.5f - 3.0f * u,
      cx + w * 0.5f + 4.0f * u, cy + h * 0.5f - 3.0f * u, 3.0f * u,
      RGB(150, 112, 40), 256u);
  mini_card(canvas, H2_FRUIT_SLICE_CARD_LEGEND, SIDE_BACK, cx, cy, w, h, 1.0f);
  char text[12]; /* 'x', any uint32-sized unsigned value, and the terminator. */
  (void)snprintf(text, sizeof(text), "x%u", landed);
  const int32_t scale = h2_fruit_slice_text_scale(u, 3.0f);
  h2_fruit_slice_draw_text(canvas, text, (int32_t)(cx + w * 0.5f + 8.0f * u),
                           (int32_t)(cy - (float)scale * 3.5f), scale,
                           RGB(255, 222, 80), 256u);
}

/* Format and size the same translated line used for drawing and layout. */
static int32_t result_text(const h2_fruit_slice_game_t* game, char text[96]) {
  const unsigned earned = popcount32(game->cards.run_earned);
  if (earned == 0u) return 0;
  const unsigned fresh = popcount32(game->cards.run_new);
  if (fresh != 0u)
    (void)snprintf(text, 96, TR("CARDS +%u  NEW %u"), earned, fresh);
  else
    (void)snprintf(text, 96, TR("CARDS +%u"), earned);
  return h2_fruit_slice_fit_scale(text, (float)game->width * 0.9f,
                                  h2_fruit_slice_text_scale(game->unit, 2.5f));
}

int32_t h2_fruit_slice_cards_result_height(const h2_fruit_slice_game_t* game) {
  char text[96];
  const int32_t scale = result_text(game, text);
  if (scale == 0) return 0;
  const int32_t shadow = scale / 2 > 0 ? scale / 2 : 1;
  return h2_fruit_slice_text_height(text, scale) + shadow;
}

void h2_fruit_slice_cards_render_result(h2_fruit_slice_canvas_t* canvas,
                                        const h2_fruit_slice_game_t* game,
                                        float y) {
  char text[96];
  const int32_t scale = result_text(game, text);
  if (scale == 0) return;
  const unsigned fresh = popcount32(game->cards.run_new);
  const float w = (float)canvas->width;
  h2_fruit_slice_draw_text_centered(
      canvas, text, w * 0.5f, y, scale,
      fresh != 0u ? RGB(255, 210, 90) : RGB(150, 214, 255), 256u);
}

void h2_fruit_slice_cards_render_play_scene(h2_fruit_slice_canvas_t* canvas,
                                            const h2_fruit_slice_game_t* game) {
  const h2_fruit_slice_cards_t* cards = &game->cards;
  const float u = game->unit;
  const float tx = (float)canvas->width * 0.5f - 12.0f * u;
  const float ty = BADGE_Y(u);
  for (unsigned i = 0; i < H2_FRUIT_SLICE_CARD_FLIGHTS; ++i) {
    const h2_fruit_slice_card_flight_t* f = &cards->flights[i];
    if (!f->active) continue;
    const float a = (float)f->age_ms;
    float x = f->x, y = f->y, s, sx = 1.0f;
    card_side_t side = SIDE_FACE;
    if (a < 200.0f) {
      s = ease_out_back(a / 200.0f);
    } else {
      const float e = ease_in_out(
          (a - 200.0f) / (float)(H2_FRUIT_SLICE_CARD_FLIGHT_MS - 200u));
      const float cx = (f->x + tx) * 0.5f;
      const float cy = fminf(f->y, ty) - 60.0f * u;
      const float k = 1.0f - e;
      x = k * k * f->x + 2.0f * k * e * cx + e * e * tx;
      y = k * k * f->y + 2.0f * k * e * cy + e * e * ty;
      s = 1.0f - 0.45f * e;
      const float c = cosf(e * 4.0f * PI);
      sx = fabsf(c);
      if (c < 0.0f) side = SIDE_BACK;
    }
    const float w = 34.0f * u * s, h = 48.0f * u * s;
    const uint16_t rc =
        h2_fruit_slice_card_rarity_color(h2_fruit_slice_card(f->card)->rarity);
    h2_fruit_slice_draw_disc(canvas, x, y, h * 0.8f, rc,
                             f->duplicate ? 50u : 90u);
    mini_card(canvas, f->card, side, x, y, w, h, sx);
    if (a < 760.0f) {
      const char* label = f->duplicate ? "+1" : TR("NEW!");
      const int32_t scale = h2_fruit_slice_text_scale(u, 2.5f);
      const uint32_t alpha =
          a < 460.0f ? 256u : alpha_of((760.0f - a) / 300.0f);
      h2_fruit_slice_draw_text_centered(
          canvas, label, f->x,
          f->y - 24.0f * u - (float)scale * 7.0f - a * 0.03f * u, scale,
          f->duplicate ? RGB(235, 240, 255) : RGB(255, 222, 80), alpha);
    }
  }
  if (cards->badge_pulse_ms > 0u &&
      game->phase == H2_FRUIT_SLICE_PHASE_PLAYING) {
    const float p = 1.0f - (float)cards->badge_pulse_ms / 420.0f;
    ring(canvas, tx, ty, (12.0f + 18.0f * p) * u, (4.0f - 2.5f * p) * u,
         RGB(255, 222, 80), alpha_of(1.0f - p));
  }
  draw_sparks(canvas, game);
}

void h2_fruit_slice_cards_render_reveal_static(
    h2_fruit_slice_canvas_t* canvas, const h2_fruit_slice_game_t* game) {
  if (!game->cards.reveal_active) return;
  h2_fruit_slice_draw_rounded_rect(canvas, 0.0f, 0.0f, (float)canvas->width,
                                   (float)canvas->height, 0.0f, RGB(8, 6, 16),
                                   214u);
}

void h2_fruit_slice_cards_render_reveal_scene(
    h2_fruit_slice_canvas_t* canvas, const h2_fruit_slice_game_t* game) {
  const h2_fruit_slice_cards_t* cards = &game->cards;
  if (!cards->reveal_active) return;
  const float u = game->unit;
  const float w = (float)canvas->width, h = (float)canvas->height;
  h2_fruit_slice_reveal_layout_t l;
  h2_fruit_slice_cards_reveal_layout(canvas->width, canvas->height, &l);
  const unsigned id = cards->reveal_card;
  const unsigned rarity = h2_fruit_slice_card(id)->rarity;
  const uint16_t rc = h2_fruit_slice_card_rarity_color(rarity);
  const float t = (float)cards->reveal_ms;
  const float zoom = (float)H2_FRUIT_SLICE_REVEAL_ZOOM_MS;
  const float flip0 = (float)H2_FRUIT_SLICE_REVEAL_FLIP_START_MS;
  const float face = (float)H2_FRUIT_SLICE_REVEAL_FACE_MS;
  const float turn = (flip0 + face) * 0.5f;
  const float settled = t >= face ? t - face : 0.0f;
  const float bob = t >= face ? sinf(settled * 0.0032f) * 3.0f * u : 0.0f;
  const float cy = l.cy + bob;

  /* God rays for rare and better cards, sweeping in during the flip. */
  if (rarity > 0u && t >= flip0) {
    const unsigned count = 6u + 2u * rarity;
    const float grow =
        t < face ? ease_out_cubic((t - flip0) / (face - flip0)) : 1.0f;
    const float reach = l.card_h * (0.66f + 0.1f * (float)rarity) * grow;
    const float spin = t * 0.00032f * (1.0f + 0.3f * (float)rarity);
    for (unsigned i = 0; i < count; ++i) {
      const float a = spin + (float)i * (2.0f * PI / (float)count);
      const float half = 0.11f;
      triangle(canvas, l.cx, cy, l.cx + cosf(a - half) * reach,
               cy + sinf(a - half) * reach, l.cx + cosf(a + half) * reach,
               cy + sinf(a + half) * reach, rc, 40u + 14u * rarity);
      triangle(canvas, l.cx, cy, l.cx + cosf(a - half * 0.5f) * reach * 0.62f,
               cy + sinf(a - half * 0.5f) * reach * 0.62f,
               l.cx + cosf(a + half * 0.5f) * reach * 0.62f,
               cy + sinf(a + half * 0.5f) * reach * 0.62f, lighten(rc, 140u),
               46u);
    }
  }

  float card_w = l.card_w, card_h = l.card_h, sx = 1.0f, x = l.cx;
  card_side_t side = SIDE_BACK;
  if (t < zoom) {
    const float s = ease_out_back(t / zoom);
    card_w *= s;
    card_h *= s;
  } else if (t < flip0) {
    const float charge = (t - zoom) / (flip0 - zoom);
    x += sinf(t * 0.09f) * (1.0f + (float)rarity) * 1.6f * u * charge;
  } else if (t < face) {
    const float theta = PI * (t - flip0) / (face - flip0);
    sx = fabsf(cosf(theta));
    card_h *= 1.0f + 0.08f * sinf(theta);
    if (theta > PI * 0.5f) side = SIDE_FACE;
  } else {
    side = SIDE_FACE;
  }
  if (t >= zoom) {
    const float strength =
        t < face ? clamp01((t - zoom) / (face - zoom)) : 1.0f;
    halo(canvas, x, cy, card_w * sx, card_h,
         side == SIDE_FACE ? rc : RGB(220, 176, 72),
         strength * (0.78f + 0.22f * sinf(t * 0.006f)), u);
  }
  draw_card(canvas, game, id, side, x, cy, card_w, card_h, sx, l.art_radius,
            side == SIDE_FACE ? sinf(t * 0.0015f) * 0.3f : 0.0f, false);

  if (t >= turn) {
    const float since = t - turn;
    if (since < 260.0f) {
      const float f = 1.0f - since / 260.0f;
      h2_fruit_slice_draw_disc(canvas, l.cx, cy,
                               l.card_h * (0.32f + 0.38f * (1.0f - f)),
                               RGB(255, 252, 240), alpha_of(f * 0.85f));
    }
    for (unsigned wave = 0; wave < (rarity >= 2u ? 2u : 1u); ++wave) {
      const float p = (since - 130.0f * (float)wave) / 520.0f;
      if (p < 0.0f || p >= 1.0f) continue;
      ring(canvas, l.cx, cy, l.card_h * (0.36f + 0.74f * ease_out_cubic(p)),
           (6.0f - 4.0f * p) * u, wave == 0u ? rc : RGB(255, 255, 255),
           alpha_of((1.0f - p) * 0.9f));
    }
  }
  if (side == SIDE_FACE && t >= face && rarity >= H2_FRUIT_SLICE_RARITY_RARE) {
    const float period =
        rarity == H2_FRUIT_SLICE_RARITY_RARE ? 2600.0f : 1700.0f;
    const float p = fmodf(settled, period) / 700.0f;
    if (p < 1.0f)
      shimmer(canvas, x - card_w * 0.5f + 2.0f, cy - card_h * 0.5f + 2.0f,
              x + card_w * 0.5f - 2.0f, cy + card_h * 0.5f - 2.0f, p,
              card_w * 0.16f, 90u + 30u * rarity);
  }

  if (t >= face) {
    const char* title = TR("NEW CARD!");
    int32_t scale = h2_fruit_slice_fit_scale(
        title, w * 0.9f, h2_fruit_slice_text_scale(u, 5.0f));
    if (settled < 140.0f) ++scale; /* Pops in one size larger. */
    h2_fruit_slice_draw_text_centered(
        canvas, title, w * 0.5f, h * 0.045f, scale,
        rarity == 0u ? RGB(255, 222, 80) : rc, alpha_of(settled / 120.0f));
    const char* name = TR(h2_fruit_slice_card_rarity_name(rarity));
    const int32_t small = h2_fruit_slice_fit_scale(
        name, w * 0.8f, h2_fruit_slice_text_scale(u, 3.0f));
    h2_fruit_slice_draw_text_centered(canvas, name, w * 0.5f,
                                      l.cy + l.card_h * 0.5f + 10.0f * u, small,
                                      rc, alpha_of(settled / 200.0f));
  }
  if (t >= (float)H2_FRUIT_SLICE_REVEAL_INPUT_MS) {
    char hint[96];
    if (cards->reveal_total > 1u)
      (void)snprintf(hint, sizeof(hint), TR("TAP TO CONTINUE %u/%u"),
                     (unsigned)cards->reveal_index + 1u,
                     (unsigned)cards->reveal_total);
    else
      (void)snprintf(hint, sizeof(hint), "%s", TR("TAP TO CONTINUE"));
    const int32_t scale = h2_fruit_slice_fit_scale(
        hint, w * 0.92f, h2_fruit_slice_text_scale(u, 2.0f));
    h2_fruit_slice_draw_text_centered(
        canvas, hint, w * 0.5f,
        h * 0.93f - (float)h2_fruit_slice_text_height(hint, scale), scale,
        RGB(240, 232, 214),
        150u + (uint32_t)(100.0f * fabsf(sinf(t * 0.003f))));
  }
  draw_sparks(canvas, game);
}

void h2_fruit_slice_cards_render_album_static(
    h2_fruit_slice_canvas_t* canvas, const h2_fruit_slice_game_t* game) {
  const h2_fruit_slice_cards_t* cards = &game->cards;
  const float w = (float)canvas->width, h = (float)canvas->height;
  const float u = game->unit;
  h2_fruit_slice_album_layout_t l;
  h2_fruit_slice_cards_album_layout(canvas->width, canvas->height, u, &l);
  h2_fruit_slice_draw_rounded_rect(canvas, 0.0f, 0.0f, w, h, 0.0f,
                                   RGB(16, 10, 22), 190u);

  const char* title = TR("CARD ALBUM");
  h2_fruit_slice_draw_text_centered(
      canvas, title, w * 0.5f, l.title.y0,
      h2_fruit_slice_fit_scale(title, w * 0.9f,
                               h2_fruit_slice_text_scale(u, 4.0f)),
      RGB(255, 204, 48), 256u);

  const unsigned owned = h2_fruit_slice_cards_owned_total(cards);
  const h2_fruit_slice_card_rect_t* bar = &l.progress;
  const float bar_h = bar->y1 - bar->y0;
  h2_fruit_slice_draw_rounded_rect(canvas, bar->x0, bar->y0, bar->x1, bar->y1,
                                   bar_h * 0.5f, RGB(44, 34, 30), 256u);
  if (owned != 0u) {
    const float fill = bar->x0 + (bar->x1 - bar->x0) * (float)owned /
                                     (float)H2_FRUIT_SLICE_CARD_COUNT;
    h2_fruit_slice_draw_rounded_rect(canvas, bar->x0, bar->y0,
                                     fmaxf(fill, bar->x0 + bar_h), bar->y1,
                                     bar_h * 0.5f, RGB(255, 184, 36), 256u);
    h2_fruit_slice_draw_rounded_rect(
        canvas, bar->x0 + bar_h * 0.3f, bar->y0 + bar_h * 0.15f,
        fmaxf(fill, bar->x0 + bar_h) - bar_h * 0.3f, bar->y0 + bar_h * 0.45f,
        bar_h * 0.15f, RGB(255, 236, 150), 170u);
  }
  char text[96];
  (void)snprintf(text, sizeof(text), "%u/%u", owned,
                 (unsigned)H2_FRUIT_SLICE_CARD_COUNT);
  const int32_t count_scale = h2_fruit_slice_fit_scale(
      text, w * 0.16f, h2_fruit_slice_text_scale(u, 2.0f));
  h2_fruit_slice_draw_text(
      canvas, text, (int32_t)(bar->x1 + 6.0f * u),
      (int32_t)((bar->y0 + bar->y1) * 0.5f - (float)count_scale * 3.5f),
      count_scale, RGB(255, 236, 190), 256u);

  for (unsigned tab = 0; tab < H2_FRUIT_SLICE_CARD_TABS; ++tab) {
    const h2_fruit_slice_card_rect_t* r = &l.tabs[tab];
    const bool active = tab == cards->tab;
    h2_fruit_slice_draw_rounded_rect(
        canvas, r->x0, r->y0, r->x1, r->y1,
        fminf(10.0f * u, (r->y1 - r->y0) * 0.5f),
        active ? RGB(255, 196, 60) : RGB(50, 38, 30), active ? 256u : 230u);
    const char* name = TR(h2_fruit_slice_card_tab_name(tab));
    const int32_t scale = h2_fruit_slice_fit_scale(
        name, (r->x1 - r->x0) * 0.86f,
        (int32_t)fmaxf(1.0f, fminf((float)h2_fruit_slice_text_scale(u, 2.5f),
                                   floorf((r->y1 - r->y0) * 0.6f / 7.0f))));
    h2_fruit_slice_draw_text_centered(
        canvas, name, (r->x0 + r->x1) * 0.5f,
        (r->y0 + r->y1 - (float)h2_fruit_slice_text_height(name, scale)) * 0.5f,
        scale, active ? RGB(48, 26, 8) : RGB(232, 222, 204), 256u);
    uint8_t ids[H2_FRUIT_SLICE_CARD_TAB_SLOTS];
    const unsigned n = h2_fruit_slice_card_tab_cards(tab, ids);
    bool fresh = false;
    for (unsigned i = 0; i < n; ++i)
      fresh |= (cards->unseen & (1u << ids[i])) != 0u;
    if (fresh)
      h2_fruit_slice_draw_disc(canvas, r->x1 - 4.0f * u, r->y0 + 4.0f * u,
                               fmaxf(2.0f, 4.5f * u), RGB(236, 44, 60), 256u);
  }

  if (!h2_fruit_slice_cards_tab_sliding(cards)) {
    for (unsigned slot = 0; slot < H2_FRUIT_SLICE_CARD_TAB_SLOTS; ++slot) {
      const int id = h2_fruit_slice_cards_slot_card(cards, slot);
      if (id < 0 || slot == cards->selected) continue;
      const h2_fruit_slice_card_rect_t* r = &l.slots[slot];
      draw_card(canvas, game, (unsigned)id,
                h2_fruit_slice_cards_owned(cards, (unsigned)id) ? SIDE_FACE
                                                                : SIDE_LOCKED,
                (r->x0 + r->x1) * 0.5f, (r->y0 + r->y1) * 0.5f, l.card_w,
                l.card_h, 1.0f, l.art_radius, 0.0f, true);
    }
  }

  const h2_fruit_slice_card_rect_t* d = &l.detail;
  h2_fruit_slice_draw_rounded_rect(canvas, d->x0, d->y0, d->x1, d->y1,
                                   10.0f * u, RGB(22, 14, 10), 225u);
  const int id = h2_fruit_slice_cards_slot_card(cards, cards->selected);
  if (id >= 0) {
    const h2_fruit_slice_card_t* card = h2_fruit_slice_card((unsigned)id);
    const bool have = h2_fruit_slice_cards_owned(cards, (unsigned)id);
    const uint16_t rc = h2_fruit_slice_card_rarity_color(card->rarity);
    const float dw = (d->x1 - d->x0) * 0.92f, dh = d->y1 - d->y0;
    const float limit = fmaxf(1.0f, floorf(dh * 0.24f / 7.0f));
    const float cx = (d->x0 + d->x1) * 0.5f;
    const char* name = have ? TR(card->name) : "???";
    const int32_t big = h2_fruit_slice_fit_scale(
        name, dw,
        (int32_t)fminf(limit, (float)h2_fruit_slice_text_scale(u, 3.0f)));
    const bool compact = dh < 42.0f;
    if (!compact)
      h2_fruit_slice_draw_text_centered(canvas, name, cx, d->y0 + dh * 0.10f,
                                        big, have ? rc : RGB(150, 150, 166),
                                        256u);
    char line[96];
    if (have)
      (void)snprintf(line, sizeof(line), "%s  x%u",
                     TR(h2_fruit_slice_card_rarity_name(card->rarity)),
                     (unsigned)cards->counts[id]);
    else
      (void)snprintf(line, sizeof(line), TR("%s - LOCKED"),
                     TR(h2_fruit_slice_card_rarity_name(card->rarity)));
    const int32_t small_pref =
        (int32_t)fminf(limit, (float)h2_fruit_slice_text_scale(u, 2.0f));
    if (compact) {
      char combined[128];
      (void)snprintf(combined, sizeof(combined), "%s %s", name, line);
      if ((float)h2_fruit_slice_text_width(combined, 1) > dw) {
        if (have)
          (void)snprintf(combined, sizeof(combined), "%s x%u", name,
                         (unsigned)cards->counts[id]);
        else
          (void)snprintf(combined, sizeof(combined), "%s %s", name,
                         TR("LOCKED"));
      }
      h2_fruit_slice_draw_text_centered(
          canvas, combined, cx, d->y0 + 1.0f,
          h2_fruit_slice_fit_scale(combined, dw, 1),
          have ? lighten(rc, 60u) : RGB(170, 170, 184), 256u);
    } else {
      h2_fruit_slice_draw_text_centered(
          canvas, line, cx, d->y0 + dh * 0.44f,
          h2_fruit_slice_fit_scale(line, dw, small_pref),
          have ? lighten(rc, 60u) : RGB(170, 170, 184), 256u);
    }
    const char* status = cards->save_status == 1u   ? TR("SESSION ONLY")
                         : cards->save_status == 2u ? TR("UNSAVED - RETRYING")
                         : cards->save_status == 3u ? TR("UNSAVED - PROTECTED")
                                                    : NULL;
    const char* hint = status != NULL ? status : TR(card->hint);
    h2_fruit_slice_draw_text_centered(
        canvas, hint, cx, compact ? d->y0 + 16.0f : d->y0 + dh * 0.72f,
        h2_fruit_slice_fit_scale(hint, dw, small_pref),
        status != NULL ? RGB(255, 150, 90)
                       : (have ? RGB(206, 196, 176) : RGB(255, 222, 120)),
        256u);
  }
}

void h2_fruit_slice_cards_render_album_scene(
    h2_fruit_slice_canvas_t* canvas, const h2_fruit_slice_game_t* game) {
  const h2_fruit_slice_cards_t* cards = &game->cards;
  const float u = game->unit;
  h2_fruit_slice_album_layout_t l;
  h2_fruit_slice_cards_album_layout(canvas->width, canvas->height, u, &l);
  if (h2_fruit_slice_cards_tab_sliding(cards)) {
    const float span = (float)H2_FRUIT_SLICE_CARD_TAB_SLIDE_MS - 45.0f * 5.0f;
    for (unsigned slot = 0; slot < H2_FRUIT_SLICE_CARD_TAB_SLOTS; ++slot) {
      const int id = h2_fruit_slice_cards_slot_card(cards, slot);
      if (id < 0) continue;
      const float p = ((float)cards->tab_ms - 45.0f * (float)slot) / span;
      if (p <= 0.0f) continue;
      const float e = ease_out_back(p);
      const float offset = (float)cards->tab_direction * (float)canvas->width *
                           0.7f * (1.0f - e);
      const h2_fruit_slice_card_rect_t* r = &l.slots[slot];
      /* Cards also turn in from their edge as they arrive. */
      const float sx = 0.35f + 0.65f * clamp01(p * 1.4f);
      draw_card(canvas, game, (unsigned)id,
                h2_fruit_slice_cards_owned(cards, (unsigned)id) ? SIDE_FACE
                                                                : SIDE_LOCKED,
                (r->x0 + r->x1) * 0.5f + offset, (r->y0 + r->y1) * 0.5f,
                l.card_w, l.card_h, sx, l.art_radius, 0.0f, true);
    }
    draw_sparks(canvas, game);
    return;
  }
  const int id = h2_fruit_slice_cards_slot_card(cards, cards->selected);
  if (id >= 0) {
    const bool have = h2_fruit_slice_cards_owned(cards, (unsigned)id);
    const h2_fruit_slice_card_t* card = h2_fruit_slice_card((unsigned)id);
    const uint16_t rc = h2_fruit_slice_card_rarity_color(card->rarity);
    const h2_fruit_slice_card_rect_t* r = &l.slots[cards->selected];
    const float t = (float)cards->album_ms;
    const float pop = (float)cards->select_ms / 320.0f;
    const float lift = pop < 1.0f ? -6.0f * u * sinf(PI * pop) : 0.0f;
    const float cx = (r->x0 + r->x1) * 0.5f;
    const float cy =
        (r->y0 + r->y1) * 0.5f + sinf(t * 0.0039f) * 2.5f * u + lift;
    halo(canvas, cx, cy, l.card_w, l.card_h, have ? rc : RGB(140, 140, 160),
         0.75f + 0.25f * sinf(t * 0.005f), u);
    const float edge = fmaxf(1.5f, 2.0f * u);
    h2_fruit_slice_draw_rounded_rect(
        canvas, cx - l.card_w * 0.5f - edge, cy - l.card_h * 0.5f - edge,
        cx + l.card_w * 0.5f + edge, cy + l.card_h * 0.5f + edge,
        l.card_w * 0.08f + edge, RGB(255, 255, 255), 230u);
    draw_card(canvas, game, (unsigned)id, have ? SIDE_FACE : SIDE_LOCKED, cx,
              cy, l.card_w, l.card_h, 1.0f, l.art_radius,
              have ? sinf(t * 0.0018f) * 0.35f : 0.0f, true);
    if (have) {
      const float p = fmodf(t, 2600.0f) / 800.0f;
      if (p < 1.0f)
        shimmer(canvas, cx - l.card_w * 0.5f + 2.0f,
                cy - l.card_h * 0.5f + 2.0f, cx + l.card_w * 0.5f - 2.0f,
                cy + l.card_h * 0.5f - 2.0f, p, l.card_w * 0.16f,
                70u + 30u * card->rarity);
    }
  }
  draw_sparks(canvas, game);
}
