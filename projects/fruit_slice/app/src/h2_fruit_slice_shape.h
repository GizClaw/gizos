#ifndef H2_FRUIT_SLICE_SHAPE_H
#define H2_FRUIT_SLICE_SHAPE_H

/* Bounded fruit-local geometry shared by texture preparation and hit tests.
 * Coordinates fit in the unit disc; decorative leaves are never hit targets.
 * Rendering uses the cached mask, so these distances are not frame work. */
#include "h2_fruit_slice_game.h"

#include <math.h>

typedef struct h2_fruit_slice_shape_circle {
  float x, y, radius;
} h2_fruit_slice_shape_circle_t;

static inline const h2_fruit_slice_shape_circle_t* h2_fruit_slice_shape_circles(
    unsigned kind, unsigned* count) {
  static const h2_fruit_slice_shape_circle_t strawberry[] = {
      {-0.27f, -0.24f, 0.40f},
      {0.27f, -0.24f, 0.40f},
      {0.0f, 0.06f, 0.48f},
      {0.0f, 0.43f, 0.27f},
      {0.0f, 0.68f, 0.12f}};
  static const h2_fruit_slice_shape_circle_t banana[] = {
      {-0.78f, -0.27f, 0.10f},
      {-0.46f, 0.16f, 0.18f},
      {0.0f, 0.34f, 0.20f},
      {0.46f, 0.16f, 0.18f},
      {0.78f, -0.27f, 0.10f}};
  static const h2_fruit_slice_shape_circle_t grapes[] = {
      {-0.29f, -0.34f, 0.34f},
      {0.29f, -0.34f, 0.34f},
      {-0.25f, 0.16f, 0.33f},
      {0.25f, 0.16f, 0.33f},
      {0.0f, 0.60f, 0.29f}};
  static const h2_fruit_slice_shape_circle_t pineapple[] = {
      {0.0f, -0.08f, 0.56f}, {0.0f, 0.28f, 0.61f}};
  *count = kind == H2_FRUIT_SLICE_PINEAPPLE ? 2u : 5u;
  switch (kind) {
    case H2_FRUIT_SLICE_STRAWBERRY:
      return strawberry;
    case H2_FRUIT_SLICE_BANANA:
      return banana;
    case H2_FRUIT_SLICE_GRAPES:
      return grapes;
    case H2_FRUIT_SLICE_PINEAPPLE:
      return pineapple;
    default:
      *count = 0u;
      return NULL;
  }
}

static inline float h2_fruit_slice_shape_body(unsigned kind, float x, float y) {
  unsigned count;
  const h2_fruit_slice_shape_circle_t* circles =
      h2_fruit_slice_shape_circles(kind, &count);
  float distance = 10.0f;
  if (kind == H2_FRUIT_SLICE_BANANA) {
    for (unsigned i = 1; i < count; ++i) {
      const float dx = circles[i].x - circles[i - 1u].x;
      const float dy = circles[i].y - circles[i - 1u].y;
      float t = ((x - circles[i - 1u].x) * dx + (y - circles[i - 1u].y) * dy) /
                (dx * dx + dy * dy);
      t = fmaxf(0.0f, fminf(1.0f, t));
      const float ex = x - circles[i - 1u].x - t * dx;
      const float ey = y - circles[i - 1u].y - t * dy;
      const float radius = circles[i - 1u].radius +
                           t * (circles[i].radius - circles[i - 1u].radius);
      distance = fminf(distance, sqrtf(ex * ex + ey * ey) - radius);
    }
    return distance;
  }
  for (unsigned i = 0; i < count; ++i) {
    const float dx = x - circles[i].x, dy = y - circles[i].y;
    distance = fminf(distance, sqrtf(dx * dx + dy * dy) - circles[i].radius);
  }
  return distance;
}

/* A pointed leaf as a tapered capsule; only startup texture work. */
static inline float h2_fruit_slice_shape_leaf(float x, float y, float tx,
                                              float ty, float width) {
  const float bx = 0.0f, by = -0.43f;
  const float dx = tx - bx, dy = ty - by;
  float t = ((x - bx) * dx + (y - by) * dy) / (dx * dx + dy * dy);
  t = fmaxf(0.0f, fminf(1.0f, t));
  const float ex = x - bx - t * dx, ey = y - by - t * dy;
  return sqrtf(ex * ex + ey * ey) - width * (1.0f - t);
}

static inline float h2_fruit_slice_shape_leaves(unsigned kind, float x,
                                                float y) {
  float distance = 10.0f;
  if (kind == H2_FRUIT_SLICE_PINEAPPLE) {
    distance = h2_fruit_slice_shape_leaf(x, y, -0.45f, -0.86f, 0.14f);
    distance =
        fminf(distance, h2_fruit_slice_shape_leaf(x, y, -0.18f, -0.98f, 0.14f));
    distance =
        fminf(distance, h2_fruit_slice_shape_leaf(x, y, 0.13f, -0.97f, 0.14f));
    distance =
        fminf(distance, h2_fruit_slice_shape_leaf(x, y, 0.43f, -0.82f, 0.14f));
  } else if (kind == H2_FRUIT_SLICE_STRAWBERRY) {
    distance = h2_fruit_slice_shape_leaf(x, y, -0.51f, -0.64f, 0.17f);
    distance =
        fminf(distance, h2_fruit_slice_shape_leaf(x, y, 0.03f, -0.90f, 0.17f));
    distance =
        fminf(distance, h2_fruit_slice_shape_leaf(x, y, 0.49f, -0.66f, 0.17f));
  } else if (kind == H2_FRUIT_SLICE_GRAPES) {
    distance = h2_fruit_slice_shape_leaf(x, y, 0.19f, -0.84f, 0.10f);
  }
  return distance;
}

#endif
