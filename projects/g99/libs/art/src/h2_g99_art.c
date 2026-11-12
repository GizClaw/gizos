#include "h2_g99_art.h"

#include <math.h>
#include <string.h>

#define PI_F 3.14159265f
#define SAMPLES 4
#define MAX_SHAPES 16

typedef enum shape_kind {
  SHAPE_CIRCLE = 0,
  SHAPE_RRECT,
  SHAPE_POLY,
  SHAPE_RING,
} shape_kind_t;

typedef enum shape_op {
  OP_ADD = 0,
  OP_SUB,
} shape_op_t;

typedef struct shape {
  shape_kind_t kind;
  shape_op_t op;
  float v[6];
  const float* points;
  int count;
} shape_t;

typedef struct shape_list {
  shape_t items[MAX_SHAPES];
  int count;
  /* Scratch for generated polygons (the gear). */
  float scratch[64];
} shape_list_t;

/* ---------- shape builders (24x24 grid unless noted) ---------- */

static void push(shape_list_t* list, shape_kind_t kind, shape_op_t op, float a,
                 float b, float c, float d, float e, float f) {
  if (list->count >= MAX_SHAPES) return;
  shape_t* s = &list->items[list->count++];
  s->kind = kind;
  s->op = op;
  s->v[0] = a;
  s->v[1] = b;
  s->v[2] = c;
  s->v[3] = d;
  s->v[4] = e;
  s->v[5] = f;
  s->points = NULL;
  s->count = 0;
}

static void circle(shape_list_t* l, shape_op_t op, float cx, float cy,
                   float r) {
  push(l, SHAPE_CIRCLE, op, cx, cy, r, 0, 0, 0);
}

static void rrect(shape_list_t* l, shape_op_t op, float x, float y, float w,
                  float h, float r) {
  push(l, SHAPE_RRECT, op, x, y, w, h, r, 0);
}

static void ring(shape_list_t* l, shape_op_t op, float cx, float cy, float r0,
                 float r1, float a0, float a1) {
  push(l, SHAPE_RING, op, cx, cy, r0, r1, a0, a1);
}

static void poly(shape_list_t* l, shape_op_t op, const float* points,
                 int count) {
  if (l->count >= MAX_SHAPES) return;
  push(l, SHAPE_POLY, op, 0, 0, 0, 0, 0, 0);
  l->items[l->count - 1].points = points;
  l->items[l->count - 1].count = count;
}

/* ---------- hit tests ---------- */

static bool in_circle(const shape_t* s, float x, float y) {
  const float dx = x - s->v[0];
  const float dy = y - s->v[1];
  return dx * dx + dy * dy <= s->v[2] * s->v[2];
}

static bool in_rrect(const shape_t* s, float x, float y) {
  const float hw = s->v[2] / 2.0f;
  const float hh = s->v[3] / 2.0f;
  const float r = s->v[4];
  float qx = fabsf(x - (s->v[0] + hw)) - (hw - r);
  float qy = fabsf(y - (s->v[1] + hh)) - (hh - r);
  if (qx > r || qy > r) return false;
  if (qx < 0.0f) qx = 0.0f;
  if (qy < 0.0f) qy = 0.0f;
  return qx * qx + qy * qy <= r * r;
}

static bool in_poly(const shape_t* s, float x, float y) {
  bool inside = false;
  for (int i = 0, j = s->count - 1; i < s->count; j = i++) {
    const float xi = s->points[2 * i], yi = s->points[2 * i + 1];
    const float xj = s->points[2 * j], yj = s->points[2 * j + 1];
    if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi)
      inside = !inside;
  }
  return inside;
}

static bool in_ring(const shape_t* s, float x, float y) {
  const float dx = x - s->v[0];
  const float dy = y - s->v[1];
  const float d2 = dx * dx + dy * dy;
  if (d2 < s->v[2] * s->v[2] || d2 > s->v[3] * s->v[3]) return false;
  const float span = s->v[5] - s->v[4];
  if (span >= 360.0f) return true;
  float angle = atan2f(dy, dx) * 180.0f / PI_F - s->v[4];
  while (angle < 0.0f) angle += 360.0f;
  while (angle >= 360.0f) angle -= 360.0f;
  return angle <= span;
}

static bool hit(const shape_list_t* list, float x, float y) {
  bool inside = false;
  for (int i = 0; i < list->count; ++i) {
    const shape_t* s = &list->items[i];
    bool in = false;
    switch (s->kind) {
      case SHAPE_CIRCLE:
        in = in_circle(s, x, y);
        break;
      case SHAPE_RRECT:
        in = in_rrect(s, x, y);
        break;
      case SHAPE_POLY:
        in = in_poly(s, x, y);
        break;
      case SHAPE_RING:
        in = in_ring(s, x, y);
        break;
    }
    if (s->op == OP_ADD)
      inside = inside || in;
    else if (in)
      inside = false;
  }
  return inside;
}

/** Coverage in [0, 255] of a pixel whose design-space origin is (x, y) and
 * whose edge is unit design units. */
static uint8_t coverage(const shape_list_t* list, float x, float y,
                        float unit) {
  int count = 0;
  for (int sy = 0; sy < SAMPLES; ++sy) {
    for (int sx = 0; sx < SAMPLES; ++sx) {
      if (hit(list, x + ((float)sx + 0.5f) * unit / SAMPLES,
              y + ((float)sy + 0.5f) * unit / SAMPLES))
        ++count;
    }
  }
  return (uint8_t)(count * 255 / (SAMPLES * SAMPLES));
}

static void put(uint8_t* argb, int32_t index, uint32_t rgb, uint8_t alpha) {
  uint8_t* p = argb + (size_t)index * 4u;
  p[0] = (uint8_t)(rgb & 0xFFu);
  p[1] = (uint8_t)((rgb >> 8) & 0xFFu);
  p[2] = (uint8_t)((rgb >> 16) & 0xFFu);
  p[3] = alpha;
}

/* Source-over of a straight-alpha colour onto a straight-alpha pixel. */
static void blend(uint8_t* argb, int32_t index, uint32_t rgb, uint8_t alpha) {
  if (alpha == 0u) return;
  uint8_t* p = argb + (size_t)index * 4u;
  const float sa = (float)alpha / 255.0f;
  const float da = (float)p[3] / 255.0f;
  const float oa = sa + da * (1.0f - sa);
  if (oa <= 0.0f) return;
  const float src[3] = {(float)(rgb & 0xFFu), (float)((rgb >> 8) & 0xFFu),
                        (float)((rgb >> 16) & 0xFFu)};
  for (int c = 0; c < 3; ++c)
    p[c] =
        (uint8_t)lroundf((src[c] * sa + (float)p[c] * da * (1.0f - sa)) / oa);
  p[3] = (uint8_t)lroundf(oa * 255.0f);
}

/* ---------- icon library ---------- */

static const float kStoreBody[] = {3.5f,  8.0f,  20.5f, 8.0f,
                                   19.1f, 21.5f, 4.9f,  21.5f};
static const float kLab[] = {8.5f,  2.0f,  15.5f, 2.0f,  15.5f, 4.2f,  14.2f,
                             4.2f,  14.2f, 8.8f,  19.8f, 18.5f, 19.9f, 20.3f,
                             18.6f, 21.6f, 5.4f,  21.6f, 4.1f,  20.3f, 4.2f,
                             18.5f, 9.8f,  8.8f,  9.8f,  4.2f,  8.5f,  4.2f};
static const float kLabLiquid[] = {8.0f,  14.5f, 16.0f, 14.5f,
                                   14.5f, 11.9f, 9.5f,  11.9f};
static const float kSpeaker[] = {2.5f,  9.0f,  6.7f, 9.0f,  12.5f, 4.2f,
                                 12.5f, 19.8f, 6.7f, 15.0f, 2.5f,  15.0f};
static const float kUpdateHead[] = {15.4f, 2.6f, 21.6f, 6.8f, 14.6f, 9.8f};
static const float kResetHead[] = {8.6f, 2.6f, 2.4f, 6.8f, 9.4f, 9.8f};
static const float kCheck[] = {3.5f,  12.6f, 5.7f,  10.4f, 9.6f, 14.3f,
                               18.3f, 5.6f,  20.5f, 7.8f,  9.6f, 18.7f};
static const float kLanguageOutline[] = {
    2.0f,  22.0f, 9.8f,  2.0f,  14.2f, 2.0f,  22.0f, 22.0f,
    17.5f, 22.0f, 15.8f, 17.5f, 8.2f,  17.5f, 6.5f,  22.0f};
static const float kLanguageCounter[] = {9.6f,  13.5f, 14.4f,
                                         13.5f, 12.0f, 6.5f};

static void gear(shape_list_t* l) {
  /* Sixteen arcs alternating between the tooth and the root radius. */
  int n = 0;
  for (int i = 0; i < 16; ++i) {
    const float r = (i % 2) != 0 ? 7.4f : 10.0f;
    for (int e = 0; e < 2; ++e) {
      const float a = ((float)(i + e) * 22.5f - 90.0f) * PI_F / 180.0f;
      l->scratch[n++] = 12.0f + r * cosf(a);
      l->scratch[n++] = 12.0f + r * sinf(a);
    }
  }
  poly(l, OP_ADD, l->scratch, n / 2);
  circle(l, OP_SUB, 12.0f, 12.0f, 3.4f);
}

static void icon_shapes(h2_g99_icon_t icon, shape_list_t* l) {
  l->count = 0;
  switch (icon) {
    case H2_G99_ICON_ME:
      circle(l, OP_ADD, 12.0f, 7.5f, 4.5f);
      ring(l, OP_ADD, 12.0f, 21.5f, 0.0f, 8.5f, 180.0f, 360.0f);
      break;
    case H2_G99_ICON_COMMUNITY:
      circle(l, OP_ADD, 17.2f, 8.5f, 3.3f);
      ring(l, OP_ADD, 17.2f, 19.0f, 0.0f, 6.0f, 180.0f, 360.0f);
      circle(l, OP_SUB, 8.5f, 8.3f, 5.0f);
      ring(l, OP_SUB, 8.5f, 20.0f, 0.0f, 8.1f, 180.0f, 360.0f);
      circle(l, OP_ADD, 8.5f, 8.3f, 3.7f);
      ring(l, OP_ADD, 8.5f, 20.0f, 0.0f, 6.8f, 180.0f, 360.0f);
      break;
    case H2_G99_ICON_STORE:
      poly(l, OP_ADD, kStoreBody, 4);
      ring(l, OP_ADD, 12.0f, 6.5f, 2.3f, 4.5f, 180.0f, 360.0f);
      rrect(l, OP_ADD, 7.5f, 6.4f, 2.2f, 1.7f, 0.0f);
      rrect(l, OP_ADD, 14.3f, 6.4f, 2.2f, 1.7f, 0.0f);
      break;
    case H2_G99_ICON_LIBRARY:
      rrect(l, OP_ADD, 3.0f, 3.0f, 8.0f, 8.0f, 2.0f);
      rrect(l, OP_ADD, 13.0f, 3.0f, 8.0f, 8.0f, 2.0f);
      rrect(l, OP_ADD, 3.0f, 13.0f, 8.0f, 8.0f, 2.0f);
      rrect(l, OP_ADD, 13.0f, 13.0f, 8.0f, 8.0f, 2.0f);
      break;
    case H2_G99_ICON_LAB:
      poly(l, OP_ADD, kLab, 14);
      poly(l, OP_SUB, kLabLiquid, 4);
      break;
    case H2_G99_ICON_SETTINGS:
      gear(l);
      break;
    case H2_G99_ICON_WIFI:
      ring(l, OP_ADD, 12.0f, 19.0f, 13.0f, 16.5f, -135.0f, -45.0f);
      ring(l, OP_ADD, 12.0f, 19.0f, 7.2f, 10.6f, -135.0f, -45.0f);
      ring(l, OP_ADD, 12.0f, 19.0f, 0.0f, 3.6f, -135.0f, -45.0f);
      break;
    case H2_G99_ICON_VOLUME:
      poly(l, OP_ADD, kSpeaker, 6);
      ring(l, OP_ADD, 13.0f, 12.0f, 3.4f, 5.4f, -50.0f, 50.0f);
      ring(l, OP_ADD, 13.0f, 12.0f, 7.0f, 9.0f, -50.0f, 50.0f);
      break;
    case H2_G99_ICON_SUN:
      circle(l, OP_ADD, 12.0f, 12.0f, 4.6f);
      for (int i = 0; i < 8; ++i) {
        const float a = (float)i * 45.0f * PI_F / 180.0f;
        circle(l, OP_ADD, 12.0f + 8.6f * cosf(a), 12.0f + 8.6f * sinf(a), 1.5f);
      }
      break;
    case H2_G99_ICON_MOON:
      circle(l, OP_ADD, 11.9f, 12.1f, 9.0f);
      circle(l, OP_SUB, 14.85f, 9.15f, 8.0f);
      break;
    case H2_G99_ICON_INFO:
      circle(l, OP_ADD, 12.0f, 12.0f, 10.0f);
      rrect(l, OP_SUB, 10.8f, 10.3f, 2.4f, 7.2f, 1.2f);
      circle(l, OP_SUB, 12.0f, 7.4f, 1.5f);
      break;
    case H2_G99_ICON_HAPTICS:
      rrect(l, OP_ADD, 8.0f, 3.0f, 8.0f, 18.0f, 2.0f);
      rrect(l, OP_SUB, 9.6f, 5.0f, 4.8f, 11.5f, 0.8f);
      rrect(l, OP_ADD, 4.4f, 7.0f, 2.0f, 10.0f, 1.0f);
      rrect(l, OP_ADD, 17.6f, 7.0f, 2.0f, 10.0f, 1.0f);
      rrect(l, OP_ADD, 1.0f, 9.0f, 1.8f, 6.0f, 0.9f);
      rrect(l, OP_ADD, 21.2f, 9.0f, 1.8f, 6.0f, 0.9f);
      break;
    case H2_G99_ICON_PAD:
      rrect(l, OP_ADD, 1.5f, 6.5f, 21.0f, 11.0f, 5.5f);
      rrect(l, OP_SUB, 5.6f, 11.0f, 5.8f, 2.0f, 0.6f);
      rrect(l, OP_SUB, 7.5f, 9.1f, 2.0f, 5.8f, 0.6f);
      circle(l, OP_SUB, 15.8f, 10.6f, 1.3f);
      circle(l, OP_SUB, 18.4f, 13.2f, 1.3f);
      break;
    case H2_G99_ICON_DISK:
      rrect(l, OP_ADD, 3.5f, 3.0f, 17.0f, 5.0f, 2.5f);
      rrect(l, OP_ADD, 3.5f, 9.5f, 17.0f, 5.0f, 2.5f);
      rrect(l, OP_ADD, 3.5f, 16.0f, 17.0f, 5.0f, 2.5f);
      break;
    case H2_G99_ICON_UPDATE:
      ring(l, OP_ADD, 12.0f, 12.0f, 6.2f, 9.0f, -60.0f, 230.0f);
      poly(l, OP_ADD, kUpdateHead, 3);
      break;
    case H2_G99_ICON_ACCOUNT:
      rrect(l, OP_ADD, 2.0f, 5.0f, 20.0f, 14.0f, 2.5f);
      circle(l, OP_SUB, 8.0f, 10.5f, 2.4f);
      ring(l, OP_SUB, 8.0f, 17.2f, 0.0f, 3.8f, 180.0f, 360.0f);
      rrect(l, OP_SUB, 13.0f, 9.0f, 6.0f, 1.8f, 0.9f);
      rrect(l, OP_SUB, 13.0f, 13.0f, 4.5f, 1.8f, 0.9f);
      break;
    case H2_G99_ICON_CLOCK:
      circle(l, OP_ADD, 12.0f, 12.0f, 10.0f);
      rrect(l, OP_SUB, 11.0f, 5.5f, 2.0f, 7.5f, 1.0f);
      rrect(l, OP_SUB, 11.0f, 11.0f, 6.0f, 2.0f, 1.0f);
      break;
    case H2_G99_ICON_LANGUAGE:
      poly(l, OP_ADD, kLanguageOutline, 8);
      poly(l, OP_SUB, kLanguageCounter, 3);
      break;
    case H2_G99_ICON_RESET:
      ring(l, OP_ADD, 12.0f, 12.0f, 6.2f, 9.0f, -50.0f, 240.0f);
      poly(l, OP_ADD, kResetHead, 3);
      circle(l, OP_ADD, 12.0f, 12.0f, 2.2f);
      break;
    case H2_G99_ICON_LOCK:
      rrect(l, OP_ADD, 5.0f, 10.5f, 14.0f, 11.0f, 2.2f);
      ring(l, OP_ADD, 12.0f, 10.6f, 3.0f, 5.2f, 180.0f, 360.0f);
      break;
    case H2_G99_ICON_CHECK:
      poly(l, OP_ADD, kCheck, 6);
      break;
    default:
      break;
  }
}

bool h2_g99_art_icon(h2_g99_icon_t icon, uint32_t rgb, uint8_t* argb,
                     int32_t size) {
  if (argb == NULL || size <= 0 || (int)icon < 0 || icon >= H2_G99_ICON_COUNT)
    return false;
  shape_list_t list;
  icon_shapes(icon, &list);
  const float unit = 24.0f / (float)size;
  for (int32_t y = 0; y < size; ++y)
    for (int32_t x = 0; x < size; ++x)
      put(argb, y * size + x, rgb,
          coverage(&list, (float)x * unit, (float)y * unit, unit));
  return true;
}

/* ---------- layered artwork ---------- */

static void layer(uint8_t* argb, int32_t width, int32_t height, float unit_x,
                  float unit_y, const shape_list_t* list, uint32_t rgb) {
  const float unit = unit_x > unit_y ? unit_x : unit_y;
  for (int32_t y = 0; y < height; ++y)
    for (int32_t x = 0; x < width; ++x) {
      const uint8_t a =
          coverage(list, (float)x * unit_x, (float)y * unit_y, unit);
      blend(argb, y * width + x, rgb, a);
    }
}

static uint32_t mix_rgb(uint32_t a, uint32_t b, float t) {
  t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
  uint32_t out = 0u;
  for (int shift = 0; shift <= 16; shift += 8) {
    const float ca = (float)((a >> shift) & 0xFFu);
    const float cb = (float)((b >> shift) & 0xFFu);
    out |= (uint32_t)lroundf(ca + (cb - ca) * t) << shift;
  }
  return out;
}

bool h2_g99_art_cover(uint8_t* argb, int32_t size) {
  if (argb == NULL || size <= 0) return false;
  const float unit = 100.0f / (float)size;
  shape_list_t frame = {0};
  rrect(&frame, OP_ADD, 0.0f, 0.0f, 100.0f, 100.0f, 18.0f);
  for (int32_t y = 0; y < size; ++y)
    for (int32_t x = 0; x < size; ++x) {
      const float t = ((float)x + (float)y) / (2.0f * (float)size);
      put(argb, y * size + x, mix_rgb(0x14164Au, 0x6B3BD6u, t),
          coverage(&frame, (float)x * unit, (float)y * unit, unit));
    }
  shape_list_t stars = {0};
  circle(&stars, OP_ADD, 18.0f, 20.0f, 1.4f);
  circle(&stars, OP_ADD, 80.0f, 16.0f, 1.1f);
  circle(&stars, OP_ADD, 70.0f, 82.0f, 1.3f);
  circle(&stars, OP_ADD, 26.0f, 78.0f, 1.0f);
  circle(&stars, OP_ADD, 86.0f, 58.0f, 1.0f);
  layer(argb, size, size, unit, unit, &stars, 0xFFFFFFu);
  shape_list_t orbit = {0};
  ring(&orbit, OP_ADD, 50.0f, 52.0f, 26.0f, 29.0f, 0.0f, 360.0f);
  layer(argb, size, size, unit, unit, &orbit, 0xB49CFFu);
  shape_list_t planet = {0};
  circle(&planet, OP_ADD, 50.0f, 52.0f, 15.0f);
  layer(argb, size, size, unit, unit, &planet, 0xE9DCFFu);
  shape_list_t moon = {0};
  circle(&moon, OP_ADD, 75.0f, 38.0f, 5.5f);
  layer(argb, size, size, unit, unit, &moon, 0xFFB35Cu);
  /* Keep the rounded corners transparent after the motif layers. */
  for (int32_t y = 0; y < size; ++y)
    for (int32_t x = 0; x < size; ++x) {
      const uint8_t a =
          coverage(&frame, (float)x * unit, (float)y * unit, unit);
      uint8_t* p = argb + ((size_t)y * (size_t)size + (size_t)x) * 4u;
      p[3] = (uint8_t)((p[3] * a) / 255u);
    }
  return true;
}

bool h2_g99_art_brand_icon(uint8_t* argb, int32_t width, int32_t height) {
  if (argb == NULL || width <= 0 || height <= 0) return false;
  memset(argb, 0, (size_t)width * (size_t)height * 4u);
  /* Design box 116 x 116: the 112 x 112 body plus its 4-unit shadow. */
  const float ux = 116.0f / (float)width;
  const float uy = 116.0f / (float)height;
  shape_list_t l = {0};
  rrect(&l, OP_ADD, 4.0f, 4.0f, 112.0f, 112.0f, 30.0f);
  layer(argb, width, height, ux, uy, &l, H2_G99_COLOR_SHADOW);
  l.count = 0;
  rrect(&l, OP_ADD, 0.0f, 0.0f, 112.0f, 112.0f, 30.0f);
  layer(argb, width, height, ux, uy, &l, H2_G99_COLOR_PURPLE);
  l.count = 0;
  rrect(&l, OP_ADD, 14.0f, 16.0f, 84.0f, 66.0f, 16.0f);
  rrect(&l, OP_ADD, 42.0f, 92.0f, 28.0f, 7.0f, 3.5f);
  layer(argb, width, height, ux, uy, &l, H2_G99_COLOR_DEEP);
  l.count = 0;
  rrect(&l, OP_ADD, 34.0f, 32.0f, 10.0f, 18.0f, 4.0f);
  rrect(&l, OP_ADD, 68.0f, 32.0f, 10.0f, 18.0f, 4.0f);
  ring(&l, OP_ADD, 56.0f, 50.0f, 12.0f, 17.0f, 25.0f, 155.0f);
  layer(argb, width, height, ux, uy, &l, H2_G99_COLOR_GLOW);
  l.count = 0;
  circle(&l, OP_ADD, 28.0f, 60.0f, 4.0f);
  circle(&l, OP_ADD, 84.0f, 60.0f, 4.0f);
  layer(argb, width, height, ux, uy, &l, H2_G99_COLOR_BLUSH);
  return true;
}

/* 7-row pixel glyphs of "GizPocket"; widths sum to 42 with 1-cell gaps. */
static const char* const kGlyphs[9][7] = {
    {".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".###."},
    {"#", ".", "#", "#", "#", "#", "#"},
    {"....", "....", "####", "...#", ".##.", "#...", "####"},
    {"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."},
    {"....", "....", ".##.", "#..#", "#..#", "#..#", ".##."},
    {"....", "....", ".###", "#...", "#...", "#...", ".###"},
    {"#...", "#...", "#..#", "#.#.", "##..", "#.#.", "#..#"},
    {"....", "....", ".##.", "#..#", "####", "#...", ".###"},
    {".#.", ".#.", "###", ".#.", ".#.", ".#.", "..#"},
};

bool h2_g99_art_wordmark_cell(int32_t col, int32_t row) {
  if (row < 0 || row >= 7 || col < 0) return false;
  int32_t start = 0;
  for (int g = 0; g < 9; ++g) {
    const int32_t width = (int32_t)strlen(kGlyphs[g][0]);
    if (col < start + width) return kGlyphs[g][row][col - start] == '#';
    start += width + 1;
    if (col < start) return false;
  }
  return false;
}

bool h2_g99_art_wordmark(uint8_t* argb, int32_t cell, int32_t shadow) {
  if (argb == NULL || cell <= 0 || shadow < 0) return false;
  const int32_t width = 42 * cell + shadow;
  const int32_t height = 7 * cell + shadow;
  memset(argb, 0, (size_t)width * (size_t)height * 4u);
  for (int pass = 0; pass < 2; ++pass) {
    const int32_t offset = pass == 0 ? shadow : 0;
    const uint32_t rgb = pass == 0 ? H2_G99_COLOR_SHADOW : H2_G99_COLOR_PURPLE;
    for (int32_t row = 0; row < 7; ++row)
      for (int32_t col = 0; col < 42; ++col) {
        if (!h2_g99_art_wordmark_cell(col, row)) continue;
        for (int32_t y = 0; y < cell; ++y)
          for (int32_t x = 0; x < cell; ++x)
            put(argb,
                (row * cell + y + offset) * width + col * cell + x + offset,
                rgb, 255u);
      }
  }
  return true;
}

bool h2_g99_art_glow(uint8_t* argb, int32_t size) {
  if (argb == NULL || size <= 0) return false;
  const float c = (float)size / 2.0f;
  for (int32_t y = 0; y < size; ++y)
    for (int32_t x = 0; x < size; ++x) {
      const float dx = (float)x + 0.5f - c;
      const float dy = (float)y + 0.5f - c;
      float t = sqrtf(dx * dx + dy * dy) / c;
      t = (t - 0.3f) / 0.7f;
      t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
      const float s = 1.0f - t * t * (3.0f - 2.0f * t);
      put(argb, y * size + x, H2_G99_COLOR_PURPLE,
          (uint8_t)lroundf(s * 255.0f));
    }
  return true;
}

/* ---------- flowing background ---------- */

typedef struct wave {
  float y;
  float amplitude;
  float length;
  float phase;
  uint32_t rgb;
  float opacity;
  /* Horizontal flow in screen widths per second. */
  float speed;
} wave_t;

static const wave_t kWaves[3] = {
    {0.60f, 0.07f, 1.25f, 0.4f, 0xB48CFFu, 0.30f, 0.050f},
    {0.70f, 0.05f, 0.95f, 2.1f, 0x6FA8FFu, 0.22f, -0.035f},
    {0.80f, 0.06f, 1.60f, 4.0f, 0xFF7AD9u, 0.18f, 0.025f},
};

int32_t h2_g99_background_wave_top(const h2_g99_background_t* background) {
  if (background == NULL) return 0;
  float top = 1.0f;
  for (int i = 0; i < 3; ++i) {
    const float t = kWaves[i].y - kWaves[i].amplitude * 1.4f;
    if (t < top) top = t;
  }
  const int32_t row = (int32_t)floorf(top * (float)background->height) -
                      (int32_t)ceilf(2.0f * background->k) - 1;
  return row < 0 ? 0 : row;
}

static float wave_y(const wave_t* w, float x, float width, float height,
                    float shift) {
  const float two_pi = 2.0f * PI_F;
  const float xs = x + shift * width;
  return height * w->y +
         height * w->amplitude *
             sinf(two_pi * xs / (width * w->length) + w->phase) +
         height * w->amplitude * 0.4f *
             sinf(two_pi * xs / (width * w->length * 0.45f) + w->phase * 1.7f);
}

static void over(float* c, uint32_t rgb, float a) {
  if (a <= 0.0f) return;
  if (a > 1.0f) a = 1.0f;
  c[0] += ((float)((rgb >> 16) & 0xFFu) - c[0]) * a;
  c[1] += ((float)((rgb >> 8) & 0xFFu) - c[1]) * a;
  c[2] += ((float)(rgb & 0xFFu) - c[2]) * a;
}

bool h2_g99_background_render(const h2_g99_background_t* background,
                              uint32_t phase_ms, uint16_t* rgb565,
                              int32_t stride, int32_t row_begin,
                              int32_t row_end) {
  if (background == NULL || rgb565 == NULL || background->width <= 0 ||
      background->height <= 0 || stride < background->width)
    return false;
  const int32_t w = background->width;
  const int32_t h = background->height;
  if (row_begin < 0) row_begin = 0;
  if (row_end > h) row_end = h;
  const float fw = (float)w;
  const float fh = (float)h;
  const float seconds = (float)(phase_ms % 600000u) / 1000.0f;
  const float line_w = 1.2f * background->k + 0.6f;
  const float shade_w = fw - (float)background->shade_x;
  for (int32_t x = 0; x < w; ++x) {
    const float fx = (float)x + 0.5f;
    float wy[3];
    float lift[3];
    for (int i = 0; i < 3; ++i) {
      wy[i] = wave_y(&kWaves[i], fx, fw, fh, kWaves[i].speed * seconds);
      /* Highlight fades in from both screen edges, peaking at 45%. */
      const float u = fx / fw;
      lift[i] = u < 0.45f ? u / 0.45f : (1.0f - u) / 0.55f;
    }
    float shade = 0.0f;
    if (shade_w > 0.0f && fx >= (float)background->shade_x) {
      const float u = (fx - (float)background->shade_x) / shade_w;
      shade =
          u < 0.25f ? 0.55f * u / 0.25f : 0.55f + 0.15f * (u - 0.25f) / 0.75f;
    }
    for (int32_t y = row_begin; y < row_end; ++y) {
      const float fy = (float)y + 0.5f;
      const float g = (fx / fw + fy / fh) / 2.0f;
      const uint32_t base =
          g < 0.55f ? mix_rgb(0x0A0618u, 0x170C38u, g / 0.55f)
                    : mix_rgb(0x170C38u, 0x2A1260u, (g - 0.55f) / 0.45f);
      float c[3] = {(float)((base >> 16) & 0xFFu), (float)((base >> 8) & 0xFFu),
                    (float)(base & 0xFFu)};
      const float gx = (fx - 0.82f * fw) / (0.7f * fw);
      const float gy = (fy - 0.18f * fh) / (0.7f * fh);
      const float gd = sqrtf(gx * gx + gy * gy);
      if (gd < 1.0f) over(c, 0x7A45FFu, 0.45f * (1.0f - gd));
      for (int i = 0; i < 3; ++i) {
        if (fy >= wy[i] && wy[i] < fh)
          over(c, kWaves[i].rgb,
               kWaves[i].opacity * (1.0f - (fy - wy[i]) / (fh - wy[i])));
        const float d = fabsf(fy - wy[i]);
        if (d < line_w)
          over(c, 0xFFFFFFu, 0.6f * lift[i] * (1.0f - d / line_w));
      }
      over(c, 0x0B0620u, shade);
      const uint16_t r = (uint16_t)((uint32_t)lroundf(c[0]) >> 3);
      const uint16_t gg = (uint16_t)((uint32_t)lroundf(c[1]) >> 2);
      const uint16_t b = (uint16_t)((uint32_t)lroundf(c[2]) >> 3);
      rgb565[(size_t)y * (size_t)stride + (size_t)x] =
          (uint16_t)((r << 11) | (gg << 5) | b);
    }
  }
  return true;
}
