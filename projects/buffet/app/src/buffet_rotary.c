#include "buffet_rotary.h"
#include <math.h>

const bf_footprint_t bf_islands[BF_LOOPS] = {{3, 4, 6, 6, 1.23f},
                                             {11, 4, 6, 6, 1.23f}};
const int bf_laps[BF_LOOPS] = {24000, 26400};
const bf_footprint_t bf_counters[] = {{.25f, .25f, 19.5f, 1.2f, .32f},
                                      {.25f, 1.45f, 1.2f, 9.0f, .32f},
                                      {18.55f, 1.45f, 1.2f, 9.0f, .32f},
                                      {.65f, 11.85f, 4.55f, .9f, .29f},
                                      {14.8f, 11.85f, 4.55f, .9f, .29f}};
const unsigned bf_counter_count = sizeof(bf_counters) / sizeof(bf_counters[0]);

bf_point_t bf_project(bf_world_t p, float z) {
  return (bf_point_t){84 + 34 * p.u - 5 * p.v,
                      48 + 3.6f * p.u + 24.48f * p.v - z};
}
bf_world_t bf_unproject(bf_point_t p) {
  float x = p.x - 84, y = p.y - 48;
  const float det = 34 * 24.48f + 5 * 3.6f;
  return (bf_world_t){(24.48f * x + 5 * y) / det, (34 * y - 3.6f * x) / det};
}
float bf_floor_height(bf_world_t p) {
  if (p.v <= 2.55f) return 8;
  if (p.u >= 8.5f && p.u <= 11.5f && p.v < 3.35f)
    return 8 * (3.35f - p.v) / .80f;
  return 0;
}
bf_footprint_t bf_inset(bf_footprint_t r, float off) {
  return (bf_footprint_t){r.u + off, r.v + off, r.w - 2 * off, r.h - 2 * off,
                          fmaxf(0.015f, r.r - off)};
}
bool bf_inside(bf_world_t p, bf_footprint_t r, float margin) {
  float x = fabsf(p.u - r.u - r.w * .5f) - (r.w * .5f - r.r);
  float y = fabsf(p.v - r.v - r.h * .5f) - (r.h * .5f - r.r);
  return hypotf(fmaxf(x, 0), fmaxf(y, 0)) + fminf(fmaxf(x, y), 0) <
         r.r + margin;
}
static bf_mark_t arc(float u, float v, float radius, float angle) {
  bf_world_t n = {cosf(angle), sinf(angle)};
  return (bf_mark_t){{u + radius * n.u, v + radius * n.v}, n};
}
bf_mark_t bf_perimeter(bf_footprint_t r, float phase) {
  const float half_pi = 1.57079632679f;
  float a = r.w - 2 * r.r, b = r.h - 2 * r.r, c = r.r * half_pi;
  float d = (phase - floorf(phase)) * (2 * (a + b) + 4 * c);
  float l = r.u, h = r.v, right = l + r.w, bottom = h + r.h;
  if (d < a) return (bf_mark_t){{l + r.r + d, h}, {0, -1}};
  d -= a;
  if (d < c) return arc(right - r.r, h + r.r, r.r, -half_pi + d / r.r);
  d -= c;
  if (d < b) return (bf_mark_t){{right, h + r.r + d}, {1, 0}};
  d -= b;
  if (d < c) return arc(right - r.r, bottom - r.r, r.r, d / r.r);
  d -= c;
  if (d < a) return (bf_mark_t){{right - r.r - d, bottom}, {0, 1}};
  d -= a;
  if (d < c) return arc(l + r.r, bottom - r.r, r.r, half_pi + d / r.r);
  d -= c;
  if (d < b) return (bf_mark_t){{l, bottom - r.r - d}, {-1, 0}};
  d -= b;
  return arc(l + r.r, h + r.r, r.r, 2 * half_pi + d / r.r);
}
float bf_loop_phase(int loop, int elapsed_ms) {
  return (loop == 0 ? 1.f : -1.f) * (elapsed_ms % bf_laps[loop]) /
         (float)bf_laps[loop];
}
