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

/* Optionally replace a complete plan by its union box. A single rectangle
 * pass requires savings for every block cost in the empirical 222..286 pixel
 * equivalent range, giving no credit for reducing PAL calls. Returns whether
 * the plan changed; a rejected guard leaves it byte-for-byte unchanged. */
int h2_lua_display_plan_guard(h2_lua_display_plan_t *plan);

/* Build the original span plan or complete legacy tile fallback, then apply
 * the guard. tiles is caller-owned scratch of ceil(width/16)*ceil(height/16)
 * bytes. tiles_only bypasses span building; bounds still forces the legacy
 * tile-aligned box, including when tiles_only is set. Returns 1 for a complete
 * plan (including empty); 0 means a rejected guard and >128 fallback rects:
 * plan is empty and tiles is ready for next_tile with cursor=0. No partial
 * plan may be submitted. Tile pixel comparison runs on span failure or an
 * explicit bounds/tiles_only request; the guard never compares framebuffers
 * or allocates storage. */
int h2_lua_display_plan_select(h2_lua_display_plan_t *plan,
    const uint16_t *current, const uint16_t *previous, int width, int height,
    h2_lua_display_plan_rect_t dirty, int gap, uint8_t *tiles, int bounds, int tiles_only);

/* Iterate the complete tile fallback after select returns 0. The scratch map
 * is consumed; width/height/gap must match select. Returns 0 at exhaustion. */
int h2_lua_display_plan_next_tile(uint8_t *tiles, int width, int height,
    int gap, int *cursor, h2_lua_display_plan_rect_t *rect);

#endif
