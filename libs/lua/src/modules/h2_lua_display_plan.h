#ifndef H2_LUA_DISPLAY_PLAN_H
#define H2_LUA_DISPLAY_PLAN_H

#include <stdint.h>

#define H2_LUA_DISPLAY_PLAN_CAPACITY 128

/* Private, exclusive bounds. Retained displays are limited to 4096 per axis. */
typedef struct h2_lua_display_plan_rect {
  uint16_t left, top, right, bottom;
} h2_lua_display_plan_rect_t;

typedef struct h2_lua_display_plan {
  int count;
  h2_lua_display_plan_rect_t rects[H2_LUA_DISPLAY_PLAN_CAPACITY];
} h2_lua_display_plan_t;

/* All storage is caller owned (the Display adapter uses quota-counted userdata).
 * Inputs must be tightly packed RGB565 frames, 1..4096 per axis, and a clipped
 * exclusive dirty box. No input pixels are modified. Returns zero on bounded
 * work/storage exhaustion: the caller must then use its coarse tile planner.
 * On success every changed pixel in dirty is covered; zero changes yields an
 * empty plan. Costs are conservative pixel-equivalent heuristics, not PAL or
 * hardware timing guarantees. */
int h2_lua_display_plan_build(h2_lua_display_plan_t *plan,
    const uint16_t *current, const uint16_t *previous, int width,
    h2_lua_display_plan_rect_t dirty, int gap);

#endif
