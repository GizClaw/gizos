#ifndef H2_LUA_DISPLAY_INTERNAL_H
#define H2_LUA_DISPLAY_INTERNAL_H

#include "../runtime/h2_lua_internal.h"
#include "h2_lua_display.h"

#include <float.h>
#include <math.h>
#include <string.h>

typedef struct display_cached_span {
  int32_t left, right, y, end_y; /* Negative end_y: span; otherwise a line. */
  uint16_t color;
} display_cached_span_t;

typedef struct display_span_cache {
  size_t count, capacity;
  int valid;
  /* Mesh caches only: replays since the last raster; whether recording ran
   * out of capacity; whether the cache was shrunk; and whether a shrunk cache
   * overflowed, which keeps it at full capacity. */
  int hits, overflow, shrunk, full;
  display_cached_span_t spans[];
} display_span_cache_t;

typedef struct h2_lua_display_mesh {
  size_t vertex_capacity, primitive_capacity;
  size_t vertex_count, primitive_count;
  double matrix[6];
  double transform[4], cosine, sine;
  int source_transform, trigonometry_valid;
  size_t span_vertex_count, span_primitive_count;
  int grid;
  int positions_valid;
  int spans_valid, span_result_valid;
  int span_left, span_right, span_top, span_bottom, span_width, span_height;
  int span_recolor;
  uint16_t span_color;
  double span_offset;
} h2_lua_display_mesh_t;

/* Exact llround(value * 2^24) for finite |value| <= 1e9. Scaling by this
 * power of two is exact. On binary64, round the significand as an integer to
 * avoid soft-double multiply/round helpers; ties still go away from zero.
 * The format probe folds at compile time and rejects mixed-word layouts. */
static inline int64_t h2_lua_display_q24(double value) {
#if FLT_RADIX == 2 && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024
  if (sizeof(double) == sizeof(uint64_t)) {
    const double one = 1.0;
    uint64_t format;
    memcpy(&format, &one, sizeof(format));
    if (format == UINT64_C(0x3ff0000000000000)) {
      uint64_t bits;
      memcpy(&bits, &value, sizeof(bits));
      unsigned exponent = (unsigned)(bits >> 52) & 0x7ffu;
      /* Includes signed zero and subnormals, all below half a Q24 unit. */
      if (exponent < 998u) return 0;
      uint64_t mantissa = (bits & UINT64_C(0x000fffffffffffff)) |
                          UINT64_C(0x0010000000000000);
      int shift = 1051 - (int)exponent;
      uint64_t magnitude;
      if (shift > 0)
        magnitude = (mantissa + (UINT64_C(1) << (shift - 1))) >> shift;
      else
        magnitude = mantissa << -shift;
      return bits >> 63 ? -(int64_t)magnitude : (int64_t)magnitude;
    }
  }
#endif
  return (int64_t)llround(value * 16777216.0);
}

int h2_lua_push_display_proxy(lua_State *state, h2_lua_job_t *job);

#endif
