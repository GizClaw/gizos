#ifndef H2_LUA_DISPLAY_INTERNAL_H
#define H2_LUA_DISPLAY_INTERNAL_H

#include "../runtime/h2_lua_internal.h"
#include "h2_lua_display.h"

typedef struct display_cached_span {
  int32_t left, right, y, end_y; /* Negative end_y: span; otherwise a line. */
  uint16_t color;
} display_cached_span_t;

typedef struct display_span_cache {
  size_t count, capacity;
  int valid;
  /* Mesh caches only: replays since the last raster, and whether a compacted
   * cache overflowed so it stays at full capacity. */
  int hits, full;
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

int h2_lua_push_display_proxy(lua_State *state, h2_lua_job_t *job);

#endif
