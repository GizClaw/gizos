#include "h2_lua_display.h"
#include "h2_raster2d.h"
#include "../../../raster2d/src/h2_raster2d_internal.h"
#include "h2_lua_numeric.h"
#include "h2_lua_geometry_batches_internal.h"
#include "h2_f32_math.h"
#include "h2_lua_display_internal.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static void set_function(lua_State *state, const char *name,
                         lua_CFunction function, h2_lua_job_t *job) {
  lua_pushlightuserdata(state, job);
  lua_pushcclosure(state, function, 1);
  lua_setfield(state, -2, name);
}

static uint16_t rgb_to_rgb565(unsigned r, unsigned g, unsigned b) {
  return (uint16_t)(((r & 0xf8u) << 8u) | ((g & 0xfcu) << 3u) |
                    ((b & 0xf8u) >> 3u));
}

static unsigned check_color_component(lua_State *state, int table_index,
                                      const char *name) {
  lua_getfield(state, table_index, name);
  lua_Integer value = luaL_checkinteger(state, -1);
  lua_pop(state, 1);
  if (value < 0 || value > 255) {
    luaL_error(state, "display color component '%s' must be in [0, 255]", name);
  }
  return (unsigned)value;
}

static uint16_t check_color(lua_State *state, int index) {
  index = lua_absindex(state, index);
  if (lua_istable(state, index)) {
    unsigned r = check_color_component(state, index, "r");
    unsigned g = check_color_component(state, index, "g");
    unsigned b = check_color_component(state, index, "b");
    return rgb_to_rgb565(r, g, b);
  }
  if (lua_type(state, index) == LUA_TSTRING) {
    const char *name = lua_tostring(state, index);
    if (strcmp(name, "white") == 0)
      return rgb_to_rgb565(255u, 255u, 255u);
    if (strcmp(name, "black") == 0)
      return 0u;
    if (strcmp(name, "red") == 0)
      return rgb_to_rgb565(255u, 0u, 0u);
    if (strcmp(name, "green") == 0)
      return rgb_to_rgb565(0u, 128u, 0u);
    if (strcmp(name, "blue") == 0)
      return rgb_to_rgb565(0u, 0u, 255u);
    (void)luaL_error(state, "unknown display color '%s'", name);
    return 0u;
  }
  (void)luaL_argerror(state, index, "display color must be a string or table");
  return 0u;
}

static int check_pixel_number(lua_State *state, int argument) {
  lua_Number value = luaL_checknumber(state, argument);
  if (!isfinite((double)value) || value < (lua_Number)INT_MIN ||
      value > (lua_Number)INT_MAX) {
    luaL_argerror(state, argument, "pixel value is out of range");
  }
  return (int)value;
}

#define H2_LUA_DISPLAY_REGION_META "h2.display.region"

typedef struct display_region_row {
  size_t offset;
  size_t first_run;
  int left, right, run_count;
} display_region_row_t;

typedef struct display_region_run {
  uint16_t left, right;
} display_region_run_t;

typedef struct display_region {
  int width, height, masked;
  uint16_t key;
  size_t pixel_count, run_count;
  /* Rows, compiled runs, pixels, then background damage bytes. */
  display_region_row_t rows[];
} display_region_t;

typedef struct display_presented {
  size_t pixel_count;
  /* Full last-successful frame followed by one comparison byte per tile. */
  uint16_t pixels[];
} display_presented_t;

static size_t display_tile_count(int width, int height) {
  return (size_t)((width + 15) / 16) * (size_t)((height + 15) / 16);
}

static display_region_run_t *display_region_runs(display_region_t *region) {
  return (display_region_run_t *)(region->rows + region->height);
}

static uint16_t *display_region_pixels(display_region_t *region) {
  return (uint16_t *)(display_region_runs(region) + region->run_count);
}

static uint8_t *display_region_damage(display_region_t *region) {
  return (uint8_t *)(display_region_pixels(region) + region->pixel_count);
}

static void display_damage_rect(h2_lua_job_t *job, int left, int top,
                                int right, int bottom) {
  display_region_t *region = job->display_background;
  if (region == NULL)
    return;
  int columns = (region->width + 15) / 16;
  uint8_t *damage = display_region_damage(region);
  for (int y = top / 16; y <= bottom / 16; ++y)
    memset(damage + (size_t)y * columns + left / 16, 1,
           (size_t)(right / 16 - left / 16 + 1));
}

static void display_dirty_full(h2_lua_job_t *job) {
  job->dirty_valid = 1;
  job->dirty_min_x = job->dirty_min_y = 0;
  job->dirty_max_x = job->display_info.width - 1;
  job->dirty_max_y = job->display_info.height - 1;
  display_damage_rect(job, 0, 0, job->dirty_max_x, job->dirty_max_y);
}

static h2_pal_result_t display_open(h2_lua_job_t *job) {
  size_t pixel_count;
  h2_pal_result_t result;
  if (job->display_shutting_down)
    return H2_PAL_ERR_INVALID_STATE;
  if (job->display_open)
    return H2_PAL_OK;
  if (!job->host->config.borrow_display) {
    result =
        (h2_pal_result_t)h2_pal_display_open(job->host->config.runtime->display);
    if (result != H2_PAL_OK)
      return result;
  }
  result = (h2_pal_result_t)h2_pal_display_get_info(
      job->host->config.runtime->display, &job->display_info);
  if (result != H2_PAL_OK || job->display_info.width <= 0 ||
      job->display_info.height <= 0) {
    if (!job->host->config.borrow_display)
      (void)h2_pal_display_close(job->host->config.runtime->display);
    return result == H2_PAL_OK ? H2_PAL_ERR_INVALID_STATE : result;
  }
  if ((size_t)job->display_info.width >
      SIZE_MAX / (size_t)job->display_info.height) {
    if (!job->host->config.borrow_display)
      (void)h2_pal_display_close(job->host->config.runtime->display);
    return H2_PAL_ERR_NO_SPACE;
  }
  pixel_count =
      (size_t)job->display_info.width * (size_t)job->display_info.height;
  if (pixel_count > SIZE_MAX / sizeof(*job->framebuffer)) {
    if (!job->host->config.borrow_display)
      (void)h2_pal_display_close(job->host->config.runtime->display);
    return H2_PAL_ERR_NO_SPACE;
  }
  job->framebuffer = h2_pal_mem_alloc(job->host->config.runtime->mem,
                                      pixel_count * sizeof(*job->framebuffer));
  if (job->framebuffer == NULL) {
    if (!job->host->config.borrow_display)
      (void)h2_pal_display_close(job->host->config.runtime->display);
    return H2_PAL_ERR_NO_MEMORY;
  }
  memset(job->framebuffer, 0, pixel_count * sizeof(*job->framebuffer));
  job->display_open = 1;
  job->dirty_valid = 1;
  job->dirty_min_x = 0;
  job->dirty_min_y = 0;
  job->dirty_max_x = job->display_info.width - 1;
  job->dirty_max_y = job->display_info.height - 1;
  return H2_PAL_OK;
}

static void set_pixel(h2_lua_job_t *job, int x, int y, uint16_t color) {
  if (x >= 0 && y >= 0 && x < job->display_info.width &&
      y < job->display_info.height) {
    job->framebuffer[(size_t)y * (size_t)job->display_info.width + (size_t)x] =
        color;
    display_damage_rect(job, x, y, x, y);
    if (job->dirty_valid && job->dirty_min_x == 0 && job->dirty_min_y == 0 &&
        job->dirty_max_x == job->display_info.width - 1 &&
        job->dirty_max_y == job->display_info.height - 1) {
      return;
    }
    if (!job->dirty_valid) {
      job->dirty_valid = 1;
      job->dirty_min_x = x;
      job->dirty_min_y = y;
      job->dirty_max_x = x;
      job->dirty_max_y = y;
    } else {
      if (x < job->dirty_min_x)
        job->dirty_min_x = x;
      if (y < job->dirty_min_y)
        job->dirty_min_y = y;
      if (x > job->dirty_max_x)
        job->dirty_max_x = x;
      if (y > job->dirty_max_y)
        job->dirty_max_y = y;
    }
  }
}

static void mark_dirty_rect(h2_lua_job_t *job, int x, int y, int width,
                            int height) {
  int min_x = x < 0 ? 0 : x;
  int min_y = y < 0 ? 0 : y;
  int max_x = x + width - 1;
  int max_y = y + height - 1;
  if (width <= 0 || height <= 0) {
    return;
  }
  if (max_x >= job->display_info.width) {
    max_x = job->display_info.width - 1;
  }
  if (max_y >= job->display_info.height) {
    max_y = job->display_info.height - 1;
  }
  if (min_x > max_x || min_y > max_y) {
    return;
  }
  display_damage_rect(job, min_x, min_y, max_x, max_y);
  if (!job->dirty_valid) {
    job->dirty_valid = 1;
    job->dirty_min_x = min_x;
    job->dirty_min_y = min_y;
    job->dirty_max_x = max_x;
    job->dirty_max_y = max_y;
    return;
  }
  if (min_x < job->dirty_min_x)
    job->dirty_min_x = min_x;
  if (min_y < job->dirty_min_y)
    job->dirty_min_y = min_y;
  if (max_x > job->dirty_max_x)
    job->dirty_max_x = max_x;
  if (max_y > job->dirty_max_y)
    job->dirty_max_y = max_y;
}

static void fill_span(h2_lua_job_t *job, int y, int min_x, int max_x,
                      uint16_t color) {
  uint16_t *pixels;
  size_t count;
  if (y < 0 || y >= job->display_info.height || max_x < 0 ||
      min_x >= job->display_info.width || min_x > max_x) {
    return;
  }
  if (min_x < 0)
    min_x = 0;
  if (max_x >= job->display_info.width)
    max_x = job->display_info.width - 1;
  pixels = job->framebuffer + (size_t)y * (size_t)job->display_info.width +
           (size_t)min_x;
  count = (size_t)(max_x - min_x + 1);
  h2_raster2d_fill_span_unchecked(pixels, count, color);
}

static void write_pixel(h2_lua_job_t *job, int x, int y, uint16_t color) {
  if (x >= 0 && y >= 0 && x < job->display_info.width &&
      y < job->display_info.height) {
    job->framebuffer[(size_t)y * (size_t)job->display_info.width + (size_t)x] =
        color;
  }
}

static void blend_pixel(h2_lua_job_t *job, int x, int y, uint16_t color,
                        unsigned alpha) {
  uint16_t *pixel;
  uint16_t background;
  unsigned inverse;
  unsigned red;
  unsigned green;
  unsigned blue;
  if (alpha == 0u || x < 0 || y < 0 || x >= job->display_info.width ||
      y >= job->display_info.height) {
    return;
  }
  if (alpha >= 255u) {
    write_pixel(job, x, y, color);
    return;
  }
  pixel = job->framebuffer + (size_t)y * (size_t)job->display_info.width +
          (size_t)x;
  background = *pixel;
  inverse = 255u - alpha;
  red = (((color >> 11u) & 0x1fu) * alpha +
         ((background >> 11u) & 0x1fu) * inverse + 127u) /
        255u;
  green = (((color >> 5u) & 0x3fu) * alpha +
           ((background >> 5u) & 0x3fu) * inverse + 127u) /
          255u;
  blue =
      ((color & 0x1fu) * alpha + (background & 0x1fu) * inverse + 127u) / 255u;
  *pixel = (uint16_t)((red << 11u) | (green << 5u) | blue);
}

static int rounded_rect_inset(int height, int radius, int row) {
  int dy;
  int extent = 0;
  int64_t radius_squared;
  if (radius == 0 || (row >= radius && row < height - radius))
    return 0;
  dy = row < radius ? radius - row : row - (height - radius - 1);
  radius_squared = (int64_t)radius * radius;
  while (extent < radius &&
         (int64_t)(extent + 1) * (extent + 1) + (int64_t)dy * dy <=
             radius_squared)
    ++extent;
  return radius - extent;
}

static int point_is_bounded(const h2_lua_job_t *job, int x, int y) {
  return (int64_t)x >= -(int64_t)job->display_info.width &&
         (int64_t)x <= (int64_t)job->display_info.width * 2 &&
         (int64_t)y >= -(int64_t)job->display_info.height &&
         (int64_t)y <= (int64_t)job->display_info.height * 2;
}

static int rect_is_bounded(const h2_lua_job_t *job, int x, int y, int width,
                           int height) {
  return width >= 0 && height >= 0 && width <= job->display_info.width &&
         height <= job->display_info.height && point_is_bounded(job, x, y);
}

static void display_clear_pixels(h2_lua_job_t *job, uint16_t color) {
  size_t count;
  uint16_t *pixels = job->framebuffer;
  count = (size_t)job->display_info.width * (size_t)job->display_info.height;
  while (count >= 4u) {
    pixels[0] = color;
    pixels[1] = color;
    pixels[2] = color;
    pixels[3] = color;
    pixels += 4;
    count -= 4u;
  }
  while (count != 0u) {
    *pixels++ = color;
    --count;
  }
  display_dirty_full(job);
}

static int display_clear(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  uint16_t color = check_color(state, 1);
  if (!job->display_open) {
    return luaL_error(state, "display is not open");
  }
  display_clear_pixels(job, color);
  return 0;
}

static double check_geometry_value(lua_State *state, int index, double value) {
  if (!isfinite(value) || fabs(value) > 100000.0)
    luaL_argerror(state, index, "geometry value is out of range");
  return value;
}

static double check_geometry_number(lua_State *state, int index) {
  return check_geometry_value(state, index, luaL_checknumber(state, index));
}

static double optional_geometry_number(lua_State *state, int index,
                                         double fallback) {
  return lua_isnoneornil(state, index) ? fallback
                                      : check_geometry_number(state, index);
}

static void display_check_clip(lua_State *state, h2_lua_job_t *job,
                                 int top_index, int bottom_index,
                                 int *top, int *bottom) {
  lua_Integer first = luaL_optinteger(state, top_index, 0);
  lua_Integer end = luaL_optinteger(state, bottom_index, job->display_info.height);
  /* Validate in Lua's integer width before narrowing on 32-bit targets. */
  if (!job->display_open || first < 0 || first > end ||
      end > job->display_info.height)
    luaL_error(state, "invalid display clip or closed display");
  *top = (int)first;
  *bottom = (int)end;
}

static void display_cache_record(display_span_cache_t *cache, int left,
                                  int right, int y, int end_y, uint16_t color) {
  if (cache == NULL || !cache->valid) return;
  if (cache->count == cache->capacity) { cache->valid = 0; cache->overflow = 1; }
  else cache->spans[cache->count++] =
      (display_cached_span_t){left, right, y, end_y, color};
}

static void display_raster_polygon_rect_capture(h2_lua_job_t *job, const double *x,
                                     const double *y, size_t count,
                                     uint16_t color, double offset,
                                     int top, int bottom, int clip_left,
                                     int clip_right, display_span_cache_t *cache) {
  double intersections[128];
  float edge_x[128], edge_y[128], slope[128], error[128];
  int edge_first[128], edge_end[128];
  for (size_t i = 0; i < count; ++i) {
    size_t j = (i + 1) % count;
    edge_first[i] = (int)ceil(fmin(y[i], y[j]));
    edge_end[i] = (int)ceil(fmax(y[i], y[j]));
    edge_x[i] = (float)x[i];
    edge_y[i] = (float)y[i];
    float dx = (float)x[j] - edge_x[i], dy = (float)y[j] - edge_y[i];
    slope[i] = fabsf(dy) < 1e-5f ? 0 : dx / dy;
    error[i] = fabsf(dy) < 1e-5f ? 1 :
        32 * FLT_EPSILON * (fabsf(edge_x[i]) + fabsf(dx) *
        (1 + (fabsf(edge_y[i]) + job->display_info.height) / fabsf(dy))) + 1e-7f;
  }
  double low = y[0], high = y[0];
  for (size_t i = 1u; i < count; ++i) {
    low = fmin(low, y[i]);
    high = fmax(high, y[i]);
  }
  int first = (int)fmax(top, ceil(low));
  int last = (int)fmin(bottom - 1, floor(high));
  for (int row = first; row <= last; ++row) {
    size_t used = 0u;
    for (size_t i = 0u; i < count; ++i) {
      size_t next = (i + 1u) % count;
      if (row >= edge_first[i] && row < edge_end[i]) {
        double cross;
        if (x[i] == x[next]) cross = x[i];
        else {
          float fast = edge_x[i] + ((float)row - edge_y[i]) * slope[i];
          float fraction = fast - floorf(fast);
          if (error[i] < .25f && fraction > error[i] && fraction < 1 - error[i])
            cross = fast;
          else cross = x[i] + (row - y[i]) * (x[next] - x[i]) / (y[next] - y[i]);
        }
        size_t at = used++;
        while (at > 0u && intersections[at - 1u] > cross) {
          intersections[at] = intersections[at - 1u];
          --at;
        }
        intersections[at] = cross;
      }
    }
    /* Even-odd, half-open edges; inclusive horizontal integer spans. */
    for (size_t i = 0u; i + 1u < used; i += 2u) {
      int left = (int)floor(ceil(intersections[i]) + offset + 0.5);
      int width = (int)(floor(intersections[i + 1u]) -
                        ceil(intersections[i]) + 1);
      if (width > 0) {
        int right = left + width - 1;
        if (left < clip_left) left = clip_left;
        if (right >= clip_right) right = clip_right - 1;
        if (left <= right) {
          fill_span(job, row, left, right, color);
          mark_dirty_rect(job, left, row, right - left + 1, 1);
          display_cache_record(cache, left, right, row, -1, color);
        }
      }
    }
  }
}

static void display_raster_polygon_rect(h2_lua_job_t *job, const double *x,
                                        const double *y, size_t count,
                                        uint16_t color, double offset,
                                        int top, int bottom, int left, int right) {
  display_raster_polygon_rect_capture(job, x, y, count, color, offset,
                                       top, bottom, left, right, NULL);
}

static void display_raster_polygon(h2_lua_job_t *job, const double *x,
                                  const double *y, size_t count, uint16_t color,
                                  double offset, int top, int bottom) {
  display_raster_polygon_rect(job, x, y, count, color, offset, top, bottom,
                             0, job->display_info.width);
}

static int display_fill_polygon(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  double x[128], y[128];
  luaL_checktype(state, 1, LUA_TTABLE);
  size_t count = lua_rawlen(state, 1);
  uint16_t color = check_color(state, 2);
  double offset = optional_geometry_number(state, 3, 0);
  double scale = optional_geometry_number(state, 6, 1);
  int top, bottom;
  display_check_clip(state, job, 4, 5, &top, &bottom);
  if (count < 3u || count > 128u || scale <= 0 || scale > 16)
    return luaL_error(state, "invalid polygon count or scale");
  for (size_t i = 0u; i < count; ++i) {
    lua_rawgeti(state, 1, (lua_Integer)i + 1);
    luaL_checktype(state, -1, LUA_TTABLE);
    lua_rawgeti(state, -1, 1);
    x[i] = check_geometry_number(state, -1) * scale;
    lua_pop(state, 1);
    lua_rawgeti(state, -1, 2);
    y[i] = check_geometry_number(state, -1) * scale;
    lua_pop(state, 2);
  }
  display_raster_polygon(job, x, y, count, color, offset, top, bottom);
  return 0;
}

static int display_fill_ellipse(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  double x = check_geometry_number(state, 1);
  double y = check_geometry_number(state, 2);
  double rx = check_geometry_number(state, 3);
  double ry = check_geometry_number(state, 4);
  uint16_t color = check_color(state, 5);
  double offset = optional_geometry_number(state, 6, 0);
  int top, bottom;
  display_check_clip(state, job, 7, 8, &top, &bottom);
  if (rx < 0 || ry <= 0 || ry > 2048)
    return luaL_error(state, "invalid ellipse radii");
  for (double local_y = -ry; local_y <= ry; local_y += 1) {
    int row = (int)floor(y + local_y + 0.5);
    if (row < top || row >= bottom)
      continue;
    double extent = rx * sqrt(fmax(0, 1 - local_y * local_y / (ry * ry)));
    int left = (int)floor(x - extent + offset + 0.5);
    int width = (int)floor(2 * extent + 1.5);
    fill_span(job, row, left, left + width - 1, color);
    mark_dirty_rect(job, left, row, width, 1);
  }
  return 0;
}

/* Clip before Bresenham so even very distant command endpoints do bounded
 * work. This helper does not modify the existing draw_line API's bounds. */
static void display_clipped_line_rect_capture(h2_lua_job_t *job, double x, double y,
                                   double end_x, double end_y, uint16_t color,
                                   int top, int bottom, int left, int right,
                                   display_span_cache_t *cache) {
  if (top == bottom || left == right)
    return;
  double dx = end_x - x, dy = end_y - y, low = 0, high = 1;
  /* Entirely visible segments need no software double divisions. Keep the
   * endpoint rounding below identical to the clipping path. */
  if (x < left || end_x < left || x > right - 1 ||
      end_x > right - 1 || y < top || end_y < top ||
      y > bottom - 1 || end_y > bottom - 1) {
    if (dx == 0) {
      if (x < left || x > right - 1)
        return;
    } else {
      double a = (left - x) / dx, b = (right - 1 - x) / dx;
      low = fmax(low, fmin(a, b));
      high = fmin(high, fmax(a, b));
    }
    if (dy == 0) {
      if (y < top || y > bottom - 1)
        return;
    } else {
      double a = (top - y) / dy, b = (bottom - 1 - y) / dy;
      low = fmax(low, fmin(a, b));
      high = fmin(high, fmax(a, b));
    }
  }
  if (low > high)
    return;
  int x0 = (int)floor(x + dx * low + 0.5);
  int y0 = (int)floor(y + dy * low + 0.5);
  int x1 = (int)floor(x + dx * high + 0.5);
  int y1 = (int)floor(y + dy * high + 0.5);
  display_cache_record(cache, x0, x1, y0, y1, color);
  int width = abs(x1 - x0), height = abs(y1 - y0);
  int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
  int64_t error = (int64_t)width - height;
  mark_dirty_rect(job, x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1,
                  width + 1, height + 1);
  for (;;) {
    write_pixel(job, x0, y0, color);
    if (x0 == x1 && y0 == y1)
      break;
    int64_t twice = error * 2;
    if (twice >= -height) { error -= height; x0 += sx; }
    if (twice <= width) { error += width; y0 += sy; }
  }
}

static void display_clipped_line_rect(h2_lua_job_t *job, double x, double y,
                                       double end_x, double end_y, uint16_t color,
                                       int top, int bottom, int left, int right) {
  display_clipped_line_rect_capture(job, x, y, end_x, end_y, color,
                                     top, bottom, left, right, NULL);
}

static void display_cache_replay(h2_lua_job_t *job, display_span_cache_t *cache) {
  for (size_t i = 0; i < cache->count; ++i) {
    const display_cached_span_t *span = &cache->spans[i];
    if (span->end_y < 0) {
      fill_span(job, span->y, span->left, span->right, span->color);
      mark_dirty_rect(job, span->left, span->y, span->right - span->left + 1, 1);
    } else {
      display_clipped_line_rect(job, span->left, span->y, span->right, span->end_y,
          span->color, 0, job->display_info.height, 0, job->display_info.width);
    }
  }
}

static void display_clipped_line(h2_lua_job_t *job, double x, double y,
                                 double end_x, double end_y, uint16_t color,
                                 int top, int bottom) {
  display_clipped_line_rect(job, x, y, end_x, end_y, color, top, bottom,
                           0, job->display_info.width);
}

typedef struct h2_lua_pixel_command {
  int kind, x, y, a, b;
  uint16_t color;
} h2_lua_pixel_command_t;

typedef struct h2_lua_pixel_commands {
  size_t count;
  h2_lua_pixel_command_t commands[];
} h2_lua_pixel_commands_t;

#define H2_LUA_COMMANDS_META "h2.display.pixel_commands"

static int display_compile_commands(lua_State *state) {
  luaL_checktype(state, 1, LUA_TTABLE);
  size_t count = lua_rawlen(state, 1);
  if (count > 16384u ||
      count > (SIZE_MAX - sizeof(h2_lua_pixel_commands_t)) /
                  sizeof(h2_lua_pixel_command_t))
    return luaL_error(state, "too many pixel commands");
  h2_lua_pixel_commands_t *list = lua_newuserdatauv(
      state, sizeof(*list) + count * sizeof(*list->commands), 0);
  list->count = count;
  for (size_t i = 0u; i < count; ++i) {
    lua_rawgeti(state, 1, (lua_Integer)i + 1);
    luaL_checktype(state, -1, LUA_TTABLE);
    lua_rawgeti(state, -1, 1);
    lua_Integer kind = luaL_checkinteger(state, -1);
    lua_pop(state, 1);
    if (kind != 0 && kind != 1)
      return luaL_error(state, "invalid pixel command kind");
    int values[4];
    for (int j = 0; j < 4; ++j) {
      lua_rawgeti(state, -1, j + 2);
      double value = check_geometry_number(state, -1);
      if (kind == 0 && j >= 2 && value < 0)
        return luaL_error(state, "negative command rectangle size");
      values[j] = (int)value;
      lua_pop(state, 1);
    }
    lua_rawgeti(state, -1, 6);
    uint16_t color = check_color(state, -1);
    lua_pop(state, 2);
    list->commands[i] = (h2_lua_pixel_command_t){
        (int)kind, values[0], values[1], values[2], values[3], color};
  }
  luaL_newmetatable(state, H2_LUA_COMMANDS_META);
  lua_setmetatable(state, -2);
  return 1;
}

static int display_draw_commands(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  const h2_lua_pixel_commands_t *list =
      luaL_checkudata(state, 1, H2_LUA_COMMANDS_META);
  int top, bottom;
  double ox = optional_geometry_number(state, 4, 0);
  double oy = optional_geometry_number(state, 5, 0);
  double sx = optional_geometry_number(state, 6, 1);
  double sy = optional_geometry_number(state, 7, sx);
  int recolor = !lua_isnoneornil(state, 8);
  uint16_t ink = recolor ? check_color(state, 8) : 0;
  /* RGB table getters may call Lua, including Display deinit. Validate the
   * acquisition after decoding every argument that can invoke user code. */
  display_check_clip(state, job, 2, 3, &top, &bottom);
  if (sx <= 0 || sy <= 0 || sx > 1000 || sy > 1000)
    return luaL_error(state, "invalid command scale");
  if (top == bottom)
    return 0;
  int transformed = ox != 0 || oy != 0 || sx != 1 || sy != 1;
  /* Compile bounds and scale limits keep every transformed endpoint below
   * 201 million, including rectangle end coordinates, before int narrowing. */
  for (size_t i = 0u; i < list->count; ++i) {
    const h2_lua_pixel_command_t *command = &list->commands[i];
    uint16_t color = recolor ? ink : command->color;
    /* Pixel-aligned retained art does not need repeated double transforms. */
    if (!transformed) {
      if (command->kind == 0) {
        int first = command->y > top ? command->y : top;
        int end = command->y + command->b;
        int last = end < bottom ? end : bottom;
        if (command->a == 0 || last <= first)
          continue;
        for (int row = first; row < last; ++row)
          fill_span(job, row, command->x, command->x + command->a - 1, color);
        mark_dirty_rect(job, command->x, first, command->a, last - first);
      } else {
        display_clipped_line(job, command->x, command->y, command->a,
                             command->b, color, top, bottom);
      }
      continue;
    }
    int x = (int)floor(ox + command->x * sx + 0.5);
    int y = (int)floor(oy + command->y * sy + 0.5);
    if (command->kind == 0) {
      int right = (int)floor(ox + (command->x + command->a) * sx + 0.5);
      int end = (int)floor(oy + (command->y + command->b) * sy + 0.5);
      int first = y > top ? y : top, last = end < bottom ? end : bottom;
      if (right <= x || last <= first)
        continue;
      for (int row = first; row < last; ++row)
        fill_span(job, row, x, right - 1, color);
      mark_dirty_rect(job, x, first, right - x, last - first);
    } else {
      double end_x = floor(ox + command->a * sx + 0.5);
      double end_y = floor(oy + command->b * sy + 0.5);
      display_clipped_line(job, x, y, end_x, end_y, color, top, bottom);
    }
  }
  return 0;
}

#define H2_LUA_RECTS_META "h2.display.rects"
#define H2_LUA_PALETTE_META "h2.display.palette"

typedef struct display_rect_batch {
  size_t count;
  h2_raster2d_rect_t rects[];
} display_rect_batch_t;

typedef struct display_palette {
  size_t count;
  uint16_t colors[];
} display_palette_t;

static size_t display_dense_count(lua_State *state, size_t limit) {
  luaL_checktype(state, 1, LUA_TTABLE);
  size_t count = lua_rawlen(state, 1);
  if (count > limit)
    luaL_error(state, "display batch limit exceeded");
  lua_pushnil(state);
  while (lua_next(state, 1)) {
    if (!lua_isinteger(state, -2) || lua_tointeger(state, -2) < 1 ||
        (lua_Unsigned)lua_tointeger(state, -2) > count)
      luaL_error(state, "expected a dense display list");
    lua_pop(state, 1);
  }
  return count;
}

static lua_Integer display_integer_field(lua_State *state, const char *name,
                                         lua_Integer min, lua_Integer max) {
  lua_getfield(state, -1, name);
  luaL_checktype(state, -1, LUA_TNUMBER);
  lua_Integer value = luaL_checkinteger(state, -1);
  if (value < min || value > max)
    luaL_error(state, "rectangle field '%s' out of range", name);
  lua_pop(state, 1);
  return value;
}

static int display_compile_rects(lua_State *state) {
  size_t count = display_dense_count(state, H2_RASTER2D_RECT_LIMIT);
  display_rect_batch_t *batch = lua_newuserdatauv(
      state, sizeof(*batch) + count * sizeof(*batch->rects), 0);
  batch->count = count;
  for (size_t i = 0; i < count; ++i) {
    lua_rawgeti(state, 1, (lua_Integer)i + 1);
    luaL_checktype(state, -1, LUA_TTABLE);
    h2_raster2d_rect_t *rect = &batch->rects[i];
    rect->x = (int32_t)display_integer_field(state, "x", INT32_MIN, INT32_MAX);
    rect->y = (int32_t)display_integer_field(state, "y", INT32_MIN, INT32_MAX);
    rect->width =
        (uint32_t)display_integer_field(state, "width", 0, UINT32_MAX);
    rect->height =
        (uint32_t)display_integer_field(state, "height", 0, UINT32_MAX);
    rect->palette_index =
        (uint32_t)display_integer_field(state, "color_index", 1,
                                        H2_RASTER2D_PALETTE_LIMIT) -
        1u;
    lua_pop(state, 1);
  }
  luaL_newmetatable(state, H2_LUA_RECTS_META);
  lua_setmetatable(state, -2);
  return 1;
}

static int display_compile_palette(lua_State *state) {
  size_t count = display_dense_count(state, H2_RASTER2D_PALETTE_LIMIT);
  display_palette_t *palette = lua_newuserdatauv(
      state, sizeof(*palette) + count * sizeof(*palette->colors), 0);
  palette->count = count;
  for (size_t i = 0; i < count; ++i) {
    lua_rawgeti(state, 1, (lua_Integer)i + 1);
    palette->colors[i] = check_color(state, -1);
    lua_pop(state, 1);
  }
  luaL_newmetatable(state, H2_LUA_PALETTE_META);
  lua_setmetatable(state, -2);
  return 1;
}

static int display_blend_palette(lua_State *state) {
  display_palette_t *out = luaL_checkudata(state, 1, H2_LUA_PALETTE_META);
  const display_palette_t *a = luaL_checkudata(state, 2, H2_LUA_PALETTE_META);
  const display_palette_t *b = luaL_checkudata(state, 3, H2_LUA_PALETTE_META);
  luaL_checktype(state, 4, LUA_TNUMBER);
  lua_Integer progress = luaL_checkinteger(state, 4);
  if (a->count != b->count || a->count != out->count || progress < 0 ||
      progress > 256)
    return luaL_error(state, "invalid palette lengths or progress");
  h2_pal_result_t result =
      h2_raster2d_palette_blend(a->colors, b->colors, a->count,
                                (unsigned)progress, out->colors, out->count);
  if (result != H2_PAL_OK)
    return luaL_error(state, "palette blend failed: %d", result);
  return 0;
}

static int display_draw_rects(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  const display_rect_batch_t *batch =
      luaL_checkudata(state, 1, H2_LUA_RECTS_META);
  const display_palette_t *palette =
      luaL_checkudata(state, 2, H2_LUA_PALETTE_META);
  int argc = lua_gettop(state);
  if (!job->display_open || (argc != 2 && argc != 6))
    return luaL_error(state, "closed display or incomplete rectangle clip");
  h2_raster2d_surface_t surface = {
      job->framebuffer,
      (size_t)job->display_info.width * (size_t)job->display_info.height,
      (size_t)job->display_info.width, (size_t)job->display_info.height,
      (size_t)job->display_info.width};
  h2_raster2d_clip_t clip = {0, 0, surface.width, surface.height};
  if (argc == 6) {
    size_t values[4];
    for (int i = 0; i < 4; ++i) {
      luaL_checktype(state, i + 3, LUA_TNUMBER);
      lua_Integer value = luaL_checkinteger(state, i + 3);
      if (value < 0 || value > INT32_MAX)
        return luaL_error(state, "rectangle clip out of range");
      values[i] = (size_t)value;
    }
    clip = (h2_raster2d_clip_t){values[0], values[1], values[2], values[3]};
  }
  h2_pal_result_t result =
      h2_raster2d_draw_rects(&surface, batch->rects, batch->count,
                             palette->colors, palette->count, &clip);
  if (result != H2_PAL_OK)
    return luaL_error(state, "rectangle replay failed: %d", result);
  /* Core validated everything before writing. This pass only marks clipped
   * rectangles, preserving existing tile precision without callbacks/scratch.
   */
  for (size_t i = 0; i < batch->count; ++i) {
    const h2_raster2d_rect_t *rect = &batch->rects[i];
    int64_t left = rect->x, top = rect->y;
    int64_t right = left + rect->width, bottom = top + rect->height;
    if (left < (int64_t)clip.left)
      left = (int64_t)clip.left;
    if (top < (int64_t)clip.top)
      top = (int64_t)clip.top;
    if (right > (int64_t)clip.right)
      right = (int64_t)clip.right;
    if (bottom > (int64_t)clip.bottom)
      bottom = (int64_t)clip.bottom;
    if (left < right && top < bottom)
      mark_dirty_rect(job, (int)left, (int)top, (int)(right - left),
                      (int)(bottom - top));
  }
  return 0;
}

/* One representation for Lua tables and allocation-free native updates. */
static const char s_display_mesh_meta = 0;

_Static_assert(_Alignof(h2_lua_display_mesh_t) >=
                   _Alignof(h2_lua_display_vertex_t), "mesh vertex alignment");
_Static_assert(_Alignof(h2_lua_display_vertex_t) >=
                   _Alignof(h2_lua_display_primitive_t), "mesh primitive alignment");

static h2_lua_display_vertex_t *mesh_vertices(h2_lua_display_mesh_t *mesh) {
  return (h2_lua_display_vertex_t *)(mesh + 1);
}

static h2_lua_display_vertex_t *mesh_positions(h2_lua_display_mesh_t *mesh) {
  return mesh_vertices(mesh) + mesh->vertex_capacity;
}

static h2_lua_display_primitive_t *mesh_primitives(h2_lua_display_mesh_t *mesh) {
  return (h2_lua_display_primitive_t *)(mesh_positions(mesh) + mesh->vertex_capacity);
}

static int mesh_data_valid(const h2_lua_display_mesh_data_t *data,
                          size_t vertex_capacity, size_t primitive_capacity) {
  if (data == NULL || data->vertex_count > vertex_capacity ||
      data->primitive_count > primitive_capacity ||
      (data->vertex_count != 0u && data->vertices == NULL) ||
      (data->primitive_count != 0u && data->primitives == NULL))
    return 0;
  for (size_t i = 0u; i < data->vertex_count; ++i) {
    if (!isfinite(data->vertices[i].x) || !isfinite(data->vertices[i].y) ||
        fabs(data->vertices[i].x) > 1000000 || fabs(data->vertices[i].y) > 1000000)
      return 0;
  }
  for (size_t i = 0u; i < data->primitive_count; ++i) {
    const h2_lua_display_primitive_t *p = &data->primitives[i];
    if (p->first > data->vertex_count || p->count > data->vertex_count - p->first)
      return 0;
    if (p->kind == H2_LUA_DISPLAY_POLYGON) {
      if (p->count < 3u || p->count > 128u)
        return 0;
    } else if (p->kind != H2_LUA_DISPLAY_LINE || p->count != 2u) {
      return 0;
    }
  }
  return 1;
}

static void mesh_replace(h2_lua_display_mesh_t *mesh,
                         const h2_lua_display_mesh_data_t *data) {
  if (data->vertex_count != 0u)
    memcpy(mesh_vertices(mesh), data->vertices,
           data->vertex_count * sizeof(*data->vertices));
  if (data->primitive_count != 0u)
    memcpy(mesh_primitives(mesh), data->primitives,
           data->primitive_count * sizeof(*data->primitives));
  mesh->vertex_count = data->vertex_count;
  mesh->primitive_count = data->primitive_count;
  mesh->positions_valid = 0;
  mesh->span_result_valid = 0;
}

/* Two spare slots, no allocator or metamethod calls, even for wrong types. */
static h2_lua_display_mesh_t *mesh_test(lua_State *state, int index) {
  int top = lua_gettop(state);
  if (index == 0 || index > top || index < -top ||
      lua_type(state, index) != LUA_TUSERDATA)
    return NULL;
  index = lua_absindex(state, index);
  if (!lua_getmetatable(state, index))
    return NULL;
  lua_rawgetp(state, LUA_REGISTRYINDEX, &s_display_mesh_meta);
  int matches = lua_rawequal(state, -1, -2);
  lua_pop(state, 2);
  return matches ? lua_touserdata(state, index) : NULL;
}

static h2_lua_display_mesh_t *mesh_allocate(lua_State *state, size_t vertices,
                                           size_t primitives) {
  if (vertices > H2_LUA_DISPLAY_VERTEX_LIMIT ||
      primitives > H2_LUA_DISPLAY_PRIMITIVE_LIMIT ||
      vertices > (SIZE_MAX - sizeof(h2_lua_display_mesh_t)) /
                     (2u * sizeof(h2_lua_display_vertex_t)))
    luaL_error(state, "invalid mesh capacity");
  size_t size = sizeof(h2_lua_display_mesh_t) +
                vertices * 2u * sizeof(h2_lua_display_vertex_t);
  if (primitives > (SIZE_MAX - size) / sizeof(h2_lua_display_primitive_t))
    luaL_error(state, "mesh capacity overflow");
  size += primitives * sizeof(h2_lua_display_primitive_t);
  h2_lua_display_mesh_t *mesh = lua_newuserdatauv(state, size, 1);
  memset(mesh, 0, sizeof(*mesh));
  mesh->vertex_capacity = vertices;
  mesh->primitive_capacity = primitives;
  lua_rawgetp(state, LUA_REGISTRYINDEX, &s_display_mesh_meta);
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    lua_newtable(state);
    lua_pushliteral(state, "display mesh");
    lua_setfield(state, -2, "__metatable");
    lua_pushvalue(state, -1);
    lua_rawsetp(state, LUA_REGISTRYINDEX, &s_display_mesh_meta);
  }
  lua_setmetatable(state, -2);
  return mesh;
}

static int mesh_push_protected(lua_State *state) {
  const h2_lua_display_mesh_config_t *config = lua_touserdata(state, 1);
  h2_lua_display_mesh_t *mesh = mesh_allocate(
      state, config->vertex_capacity, config->primitive_capacity);
  mesh_replace(mesh, &config->initial);
  return 1;
}

h2_pal_result_t h2_lua_display_mesh_push(
    void *lua_state, const h2_lua_display_mesh_config_t *config) {
  lua_State *state = lua_state;
  if (state == NULL || config == NULL ||
      config->vertex_capacity > H2_LUA_DISPLAY_VERTEX_LIMIT ||
      config->primitive_capacity > H2_LUA_DISPLAY_PRIMITIVE_LIMIT ||
      !mesh_data_valid(&config->initial, config->vertex_capacity,
                       config->primitive_capacity))
    return H2_PAL_ERR_INVALID_ARG;
  int top = lua_gettop(state);
  if (!lua_checkstack(state, 3))
    return H2_PAL_ERR_NO_MEMORY;
  lua_pushcfunction(state, mesh_push_protected);
  lua_pushlightuserdata(state, (void *)config);
  int result = lua_pcall(state, 1, 1, 0);
  if (result == LUA_OK)
    return H2_PAL_OK;
  lua_settop(state, top);
  return result == LUA_ERRMEM ? H2_PAL_ERR_NO_MEMORY : H2_PAL_ERR_INVALID_STATE;
}

h2_pal_result_t h2_lua_display_mesh_update(
    void *lua_state, int stack_index, const h2_lua_display_mesh_data_t *data) {
  lua_State *state = lua_state;
  if (state == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_lua_display_mesh_t *mesh = mesh_test(state, stack_index);
  if (mesh == NULL || !mesh_data_valid(data, mesh->vertex_capacity,
                                      mesh->primitive_capacity))
    return H2_PAL_ERR_INVALID_ARG;
  mesh_replace(mesh, data);
  return H2_PAL_OK;
}

static h2_lua_display_mesh_t *mesh_check(lua_State *state, int index) {
  h2_lua_display_mesh_t *mesh = mesh_test(state, index);
  if (mesh == NULL)
    luaL_argerror(state, index, "expected display mesh");
  return mesh;
}

static size_t mesh_table_count(lua_State *state, int index, size_t limit) {
  luaL_checktype(state, index, LUA_TTABLE);
  size_t count = lua_rawlen(state, index);
  if (count > limit)
    luaL_argerror(state, index, "mesh count exceeds capacity");
  return count;
}

static double mesh_number(lua_State *state, int index) {
  double value = luaL_checknumber(state, index);
  if (!isfinite(value) || fabs(value) > 1000000)
    luaL_argerror(state, index, "mesh coordinate out of range");
  return value;
}

static void mesh_decode(lua_State *state, h2_lua_display_mesh_t *mesh,
                         int vertices, int primitives, size_t nv, size_t np) {
  h2_lua_display_vertex_t *v = mesh_vertices(mesh);
  h2_lua_display_primitive_t *p = mesh_primitives(mesh);
  for (size_t i = 0; i < nv; ++i) {
    lua_rawgeti(state, vertices, (lua_Integer)i + 1);
    luaL_checktype(state, -1, LUA_TTABLE);
    lua_rawgeti(state, -1, 1);
    v[i].x = mesh_number(state, -1);
    lua_pop(state, 1);
    lua_rawgeti(state, -1, 2);
    v[i].y = mesh_number(state, -1);
    lua_pop(state, 2);
  }
  for (size_t i = 0; i < np; ++i) {
    lua_rawgeti(state, primitives, (lua_Integer)i + 1);
    luaL_checktype(state, -1, LUA_TTABLE);
    lua_Integer fields[3];
    for (int j = 0; j < 3; ++j) {
      lua_rawgeti(state, -1, j + 1);
      fields[j] = luaL_checkinteger(state, -1);
      lua_pop(state, 1);
    }
    if ((fields[0] != 0 && fields[0] != 1) || fields[1] < 1 ||
        (lua_Unsigned)fields[1] > nv || fields[2] < 2 || fields[2] > 128)
      luaL_error(state, "invalid mesh primitive");
    lua_rawgeti(state, -1, 4);
    uint16_t color = check_color(state, -1);
    lua_pop(state, 2);
    p[i] = (h2_lua_display_primitive_t){
        (h2_lua_display_primitive_kind_t)fields[0], (size_t)fields[1] - 1u,
        (size_t)fields[2], color};
  }
  h2_lua_display_mesh_data_t data = {v, nv, p, np};
  if (!mesh_data_valid(&data, mesh->vertex_capacity, mesh->primitive_capacity))
    luaL_error(state, "invalid mesh vertex range");
  mesh->vertex_count = nv;
  mesh->primitive_count = np;
}

static int display_compile_mesh(lua_State *state) {
  size_t nv = mesh_table_count(state, 1, H2_LUA_DISPLAY_VERTEX_LIMIT);
  size_t np = mesh_table_count(state, 2, H2_LUA_DISPLAY_PRIMITIVE_LIMIT);
  lua_Integer vc = luaL_optinteger(state, 3, (lua_Integer)nv);
  lua_Integer pc = luaL_optinteger(state, 4, (lua_Integer)np);
  if (vc < (lua_Integer)nv || vc > H2_LUA_DISPLAY_VERTEX_LIMIT ||
      pc < (lua_Integer)np || pc > H2_LUA_DISPLAY_PRIMITIVE_LIMIT)
    return luaL_error(state, "invalid mesh capacity");
  h2_lua_display_mesh_t *mesh = mesh_allocate(state, (size_t)vc, (size_t)pc);
  mesh_decode(state, mesh, 1, 2, nv, np);
  return 1;
}

static int display_update_mesh(lua_State *state) {
  h2_lua_display_mesh_t *mesh = mesh_check(state, 1);
  size_t nv = mesh_table_count(state, 2, mesh->vertex_capacity);
  size_t np = mesh_table_count(state, 3, mesh->primitive_capacity);
  h2_lua_display_mesh_t *staged = mesh_allocate(state, nv, np);
  mesh_decode(state, staged, 2, 3, nv, np);
  h2_lua_display_mesh_data_t data = {
      mesh_vertices(staged), nv, mesh_primitives(staged), np};
  mesh_replace(mesh, &data);
  return 0;
}

/* Raw keys avoid invoking option-table policy during a draw. RGB colors
 * retain their existing getter semantics and are decoded before any writes. */
static void mesh_option(lua_State *state, const char *key) {
  if (lua_isnoneornil(state, 2)) {
    lua_pushnil(state);
  } else {
    lua_pushstring(state, key);
    lua_rawget(state, 2);
  }
}

static lua_Integer mesh_integer_option(lua_State *state, const char *key,
                                       lua_Integer fallback) {
  mesh_option(state, key);
  lua_Integer value = luaL_optinteger(state, -1, fallback);
  lua_pop(state, 1);
  return value;
}

static h2_lua_display_vertex_t mesh_transform(
    h2_lua_display_vertex_t v, const double matrix[6], int grid) {
  h2_lua_display_vertex_t p = {
      (matrix[0] * v.x + matrix[2] * v.y) + matrix[4],
      (matrix[1] * v.x + matrix[3] * v.y) + matrix[5]};
  if (grid != 0) {
    p.x = floor(p.x / grid + 0.5) * grid;
    p.y = floor(p.y / grid + 0.5) * grid;
  }
  return p;
}

/* A new mesh cache starts small and grows fourfold after an overflow, up to
 * the maximum. Once a static mesh replays, the cache is reallocated to the
 * spans it actually produced. */
#define MESH_SPAN_INITIAL_CAPACITY 512u
#define MESH_SPAN_CAPACITY 8192u
#define MESH_SPAN_COMPACT_SLACK 256u

/* Mesh span snapshots follow the span array. Their lifetime is the cache
 * userdata's, independently of source updates or non-retained draws. */
static size_t mesh_span_snapshot_offset(size_t capacity) {
  size_t size = sizeof(display_span_cache_t) + capacity * sizeof(display_cached_span_t);
  size_t alignment = _Alignof(h2_lua_display_vertex_t);
  return (size + alignment - 1u) / alignment * alignment;
}

static h2_lua_display_vertex_t *mesh_span_positions(display_span_cache_t *cache) {
  return (h2_lua_display_vertex_t *)((char *)cache +
                                     mesh_span_snapshot_offset(cache->capacity));
}

static size_t mesh_span_snapshot_bytes(const h2_lua_display_mesh_t *mesh) {
  return mesh->vertex_capacity * sizeof(h2_lua_display_vertex_t) +
         mesh->primitive_capacity * sizeof(h2_lua_display_primitive_t);
}

static h2_lua_display_primitive_t *mesh_span_primitives(
    h2_lua_display_mesh_t *mesh, display_span_cache_t *cache) {
  return (h2_lua_display_primitive_t *)(mesh_span_positions(cache) +
                                       mesh->vertex_capacity);
}

typedef struct mesh_source_f32 {
  float x, y, scale, ca, sa, grid;
} mesh_source_f32_t;

static h2_lua_display_vertex_t mesh_source_transform(
    h2_lua_display_vertex_t v, const double t[4], double ca, double sa, int grid,
    const mesh_source_f32_t *f) {
  double x = t[0], y = t[1], scale = t[2];
  float fx = (f->x + ((float)v.x * f->ca -
              (float)v.y * f->sa) * f->scale) / f->grid;
  float fy = (f->y + ((float)v.x * f->sa +
              (float)v.y * f->ca) * f->scale) / f->grid;
  float error = 64 * FLT_EPSILON * (fabsf(f->x) + fabsf(f->y) +
      (fabsf((float)v.x) + fabsf((float)v.y)) * f->scale + 1);
  /* Wider public mesh inputs use the source double fallback. Do not convert
   * to an integer before the caller has checked the complete result bounds. */
  int fast = fabs(v.x) <= 100000 && fabs(v.y) <= 100000 && error < .25f;
  h2_lua_display_vertex_t p;
  p.x = fast && fabsf(fx - floorf(fx) - .5f) > error
      ? (double)(floorf(fx + .5f) * f->grid)
      : floor((x + (v.x * ca - v.y * sa) * scale) / grid + .5) * grid;
  p.y = fast && fabsf(fy - floorf(fy) - .5f) > error
      ? (double)(floorf(fy + .5f) * f->grid)
      : floor((y + (v.x * sa + v.y * ca) * scale) / grid + .5) * grid;
  return p;
}

static int mesh_span_result_equal(h2_lua_display_mesh_t *mesh,
                                  display_span_cache_t *cache,
                                  const h2_lua_display_vertex_t *positions) {
  if (mesh->span_vertex_count != mesh->vertex_count ||
      mesh->span_primitive_count != mesh->primitive_count)
    return 0;
  const h2_lua_display_vertex_t *old = mesh_span_positions(cache);
  for (size_t i = 0; i < mesh->vertex_count; ++i)
    if (old[i].x != positions[i].x || old[i].y != positions[i].y)
      return 0;
  const h2_lua_display_primitive_t *before = mesh_span_primitives(mesh, cache);
  const h2_lua_display_primitive_t *after = mesh_primitives(mesh);
  for (size_t i = 0; i < mesh->primitive_count; ++i)
    if (before[i].kind != after[i].kind || before[i].first != after[i].first ||
        before[i].count != after[i].count || before[i].color != after[i].color)
      return 0;
  return 1;
}

#define MESH_STAGE_LIMIT 1024u
static const char s_mesh_stage_key = 0;

/* Leave the chosen userdata strongly rooted on the calling Lua stack. The
 * registry owns one high-water buffer, not one buffer per mesh. */
static h2_lua_display_vertex_t *mesh_stage(lua_State *state, h2_lua_job_t *job,
                                          size_t capacity) {
  size_t count = capacity < MESH_STAGE_LIMIT ? capacity : MESH_STAGE_LIMIT;
  size_t bytes = count * sizeof(h2_lua_display_vertex_t);
  lua_rawgetp(state, LUA_REGISTRYINDEX, &s_mesh_stage_key);
  if (lua_isuserdata(state, -1) && lua_rawlen(state, -1) >= bytes)
    return lua_touserdata(state, -1);
  lua_pop(state, 1);
  h2_lua_display_vertex_t *candidate = lua_newuserdatauv(state, bytes, 0);
  int at = lua_gettop(state);
  /* Allocation can finalize a mesh that draws with a larger scratch buffer.
   * Keep that newer allocation instead of shrinking the shared high-water. */
  lua_rawgetp(state, LUA_REGISTRYINDEX, &s_mesh_stage_key);
  if (lua_isuserdata(state, -1) && lua_rawlen(state, -1) >= bytes) {
    h2_lua_display_vertex_t *current = lua_touserdata(state, -1);
    lua_remove(state, at);
    return current;
  }
  lua_pop(state, 1);
  if (job->display_open) {
    lua_pushvalue(state, at);
    lua_rawsetp(state, LUA_REGISTRYINDEX, &s_mesh_stage_key);
  }
  return candidate;
}

static int display_draw_mesh(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  h2_lua_display_mesh_t *mesh = mesh_check(state, 1);
  if (!lua_isnoneornil(state, 2)) luaL_checktype(state, 2, LUA_TTABLE);
  double matrix[6] = {1, 0, 0, 1, 0, 0};
  mesh_option(state, "matrix");
  int has_matrix = !lua_isnil(state, -1);
  if (has_matrix) {
    luaL_checktype(state, -1, LUA_TTABLE);
    for (int i = 0; i < 6; ++i) {
      lua_rawgeti(state, -1, i + 1);
      matrix[i] = mesh_number(state, -1);
      lua_pop(state, 1);
    }
  }
  lua_pop(state, 1);
  double transform[4] = {0, 0, 1, 0};
  mesh_option(state, "transform");
  int source_transform = !lua_isnil(state, -1);
  if (source_transform) {
    if (has_matrix) return luaL_error(state, "matrix and transform are exclusive");
    luaL_checktype(state, -1, LUA_TTABLE);
    const char *names[] = {"x", "y", "scale", "angle"};
    for (int i = 0; i < 4; ++i) {
      lua_pushstring(state, names[i]);
      lua_rawget(state, -2);
      transform[i] = check_geometry_number(state, -1);
      lua_pop(state, 1);
    }
    if (transform[2] <= 0 || transform[2] > 100)
      return luaL_error(state, "mesh transform scale out of range");
  }
  lua_pop(state, 1);
  lua_Integer grid = mesh_integer_option(state, "grid", 0);
  if (source_transform && (grid < 1 || grid > 16))
    return luaL_error(state, "mesh transform requires grid in 1..16");
  lua_Integer left = mesh_integer_option(state, "left", 0);
  lua_Integer right = mesh_integer_option(state, "right", job->display_info.width);
  lua_Integer top = mesh_integer_option(state, "top", 0);
  lua_Integer bottom = mesh_integer_option(state, "bottom", job->display_info.height);
  mesh_option(state, "offset_x");
  double offset = optional_geometry_number(state, -1, 0);
  lua_pop(state, 1);
  mesh_option(state, "color");
  int recolor = !lua_isnil(state, -1);
  uint16_t ink = recolor ? check_color(state, -1) : 0;
  lua_pop(state, 1);
  mesh_option(state, "cache");
  if (!lua_isnil(state, -1)) luaL_checktype(state, -1, LUA_TBOOLEAN);
  int retain_spans = lua_toboolean(state, -1);
  lua_pop(state, 1);
  int identity = !source_transform && grid == 0 && matrix[0] == 1 && matrix[1] == 0 &&
                 matrix[2] == 0 && matrix[3] == 1 && matrix[4] == 0 &&
                 matrix[5] == 0;
  h2_lua_display_vertex_t *staged = NULL;
  if (!identity && mesh->vertex_count != 0 && mesh->vertex_count <= MESH_STAGE_LIMIT)
    staged = mesh_stage(state, job, mesh->vertex_capacity);
  display_span_cache_t *cache = NULL;
  int cache_at = 0, new_cache = 0;
  if (retain_spans) {
    lua_getiuservalue(state, 1, 1);
    if (lua_isnil(state, -1)) {
      lua_pop(state, 1);
      cache = lua_newuserdatauv(state, mesh_span_snapshot_offset(MESH_SPAN_INITIAL_CAPACITY) +
          mesh_span_snapshot_bytes(mesh), 0);
      memset(cache, 0, sizeof(*cache));
      cache->capacity = MESH_SPAN_INITIAL_CAPACITY;
      cache_at = lua_gettop(state);
      /* A finalizer may have installed a complete candidate during allocation.
       * Reuse it; do not overwrite it with the outer call's empty cache. */
      lua_getiuservalue(state, 1, 1);
      if (!lua_isnil(state, -1)) {
        cache = lua_touserdata(state, -1);
        lua_remove(state, cache_at);
      } else {
        lua_pop(state, 1);
        new_cache = 1;
      }
    } else {
      cache = lua_touserdata(state, -1);
      /* Grow a cache that overflowed; shrink a replayed cache to its spans. A
       * shrunk cache that overflows returns to full capacity and stays there,
       * so an animated mesh does not keep reallocating. */
      int shrink = cache->valid && mesh->spans_valid && cache->hits != 0 &&
                   !cache->full &&
                   cache->capacity - cache->count >= MESH_SPAN_COMPACT_SLACK;
      int regrow = cache->overflow && cache->capacity < MESH_SPAN_CAPACITY;
      if (shrink || regrow) {
        display_span_cache_t *old = cache;
        const size_t count = old->count, capacity = old->capacity;
        const int valid = old->valid;
        size_t next_capacity = count;
        if (regrow) {
          next_capacity = old->shrunk || capacity > MESH_SPAN_CAPACITY / 4u
              ? MESH_SPAN_CAPACITY : capacity * 4u;
        }
        display_span_cache_t *next = lua_newuserdatauv(state,
            mesh_span_snapshot_offset(next_capacity) + mesh_span_snapshot_bytes(mesh), 0);
        /* Allocation may run finalizers that draw this mesh; only replace an
         * unchanged cache, otherwise continue with the cache they left. */
        lua_getiuservalue(state, 1, 1);
        int unchanged = lua_touserdata(state, -1) == old && old->count == count &&
                        old->capacity == capacity && old->valid == valid;
        if (!unchanged) {
          lua_remove(state, -2);
          lua_remove(state, -2);
          cache = lua_touserdata(state, -1);
        } else {
          lua_pop(state, 1);
          memcpy(next, old, sizeof(*old) + (regrow ? 0u : count) * sizeof(old->spans[0]));
          next->capacity = next_capacity;
          if (regrow) {
            next->count = 0;
            next->valid = 0;
            next->hits = 0;
            next->overflow = 0;
            next->full = old->shrunk;
            mesh->spans_valid = 0;
          } else {
            next->shrunk = 1;
            memcpy(mesh_span_positions(next), mesh_span_positions(old),
                   mesh_span_snapshot_bytes(mesh));
          }
          lua_pushvalue(state, -1);
          lua_setiuservalue(state, 1, 1);
          lua_remove(state, -2);
          cache = next;
        }
      }
    }
  }
  if (!job->display_open || grid < 0 || grid > 16 || left < 0 ||
      left > right || right > job->display_info.width || top < 0 ||
      top > bottom || bottom > job->display_info.height)
    return luaL_error(state, "invalid mesh options or closed display");
  h2_lua_display_vertex_t *positions = mesh_positions(mesh);
  const h2_lua_display_vertex_t *vertices = mesh_vertices(mesh);
  /* No allocation or callback from this final view through publication.
   * Reentry can change active size, so a now-larger mesh uses the old path. */
  if (mesh->vertex_count > MESH_STAGE_LIMIT) staged = NULL;
  int same_parameters = mesh->trigonometry_valid;
  for (int i = 0; i < 4; ++i)
    if (mesh->transform[i] != transform[i]) same_parameters = 0;
  double ca = mesh->cosine, sa = mesh->sine;
  if (source_transform && !same_parameters) {
    ca = cos(transform[3]);
    sa = sin(transform[3]);
  }
  int same_matrix = 1;
  for (int i = 0; i < 6; ++i)
    if (mesh->matrix[i] != matrix[i]) same_matrix = 0;
  if (!mesh->positions_valid || mesh->grid != grid ||
      mesh->source_transform != source_transform ||
      (source_transform ? !same_parameters : !same_matrix)) {
    if (!identity) {
      /* Prepare source invariants after the last allocation/reentrant callback.
       * Both transactional passes use these same rounded scalar values. */
      mesh_source_f32_t source_f32 = {0};
      if (source_transform)
        source_f32 = (mesh_source_f32_t){(float)transform[0], (float)transform[1],
            (float)transform[2], (float)ca, (float)sa, (float)grid};
      /* Validate everything before changing derived state or a span candidate. */
      for (size_t i = 0; i < mesh->vertex_count; ++i) {
        h2_lua_display_vertex_t p = source_transform
            ? mesh_source_transform(vertices[i], transform, ca, sa, (int)grid, &source_f32)
            : mesh_transform(vertices[i], matrix, (int)grid);
        if (!isfinite(p.x) || !isfinite(p.y) || fabs(p.x) > 16000000 ||
            fabs(p.y) > 16000000)
          return luaL_error(state, "transformed mesh coordinate out of range");
        if (staged != NULL) staged[i] = p;
      }
      if (staged != NULL)
        memcpy(positions, staged, mesh->vertex_count * sizeof(*positions));
      else
        for (size_t i = 0; i < mesh->vertex_count; ++i)
          positions[i] = source_transform
              ? mesh_source_transform(vertices[i], transform, ca, sa, (int)grid, &source_f32)
              : mesh_transform(vertices[i], matrix, (int)grid);
    }
    mesh->span_result_valid = 0;
    memcpy(mesh->matrix, matrix, sizeof(matrix));
    if (source_transform) {
      memcpy(mesh->transform, transform, sizeof(transform));
      mesh->cosine = ca;
      mesh->sine = sa;
      mesh->trigonometry_valid = 1;
    }
    mesh->source_transform = source_transform;
    mesh->grid = (int)grid;
    mesh->positions_valid = 1;
  }
  if (new_cache) {
    lua_pushvalue(state, cache_at);
    lua_setiuservalue(state, 1, 1);
    mesh->spans_valid = 0;
  }
  const h2_lua_display_vertex_t *draw_positions = identity ? vertices : positions;
  if (top == bottom || left == right) return 0;
  if (cache != NULL) {
    if (mesh->spans_valid && cache->valid && !mesh->span_result_valid)
      mesh->span_result_valid = mesh_span_result_equal(mesh, cache, draw_positions);
    if (mesh->spans_valid && cache->valid && mesh->span_result_valid &&
        mesh->span_left == left &&
        mesh->span_right == right && mesh->span_top == top &&
        mesh->span_bottom == bottom && mesh->span_width == job->display_info.width &&
        mesh->span_height == job->display_info.height && mesh->span_offset == offset &&
        mesh->span_recolor == recolor && (!recolor || mesh->span_color == ink)) {
      display_cache_replay(job, cache);
      if (cache->hits < INT_MAX) ++cache->hits;
      return 0;
    }
    mesh->spans_valid = 0;
    cache->valid = 1;
    cache->count = 0;
    cache->hits = 0;
    cache->overflow = 0;
    mesh->span_left = (int)left;
    mesh->span_right = (int)right;
    mesh->span_top = (int)top;
    mesh->span_bottom = (int)bottom;
    mesh->span_width = job->display_info.width;
    mesh->span_height = job->display_info.height;
    mesh->span_offset = offset;
    mesh->span_recolor = recolor;
    mesh->span_color = ink;
  }
  /* Exact identity reads the mesh's own validated, bounded vertices, without
   * copying to positions. Select on cache hits too: positions may still hold
   * an older general transform. Signed zeros are raster-equivalent. */
  const h2_lua_display_primitive_t *primitives = mesh_primitives(mesh);
  for (size_t i = 0; i < mesh->primitive_count; ++i) {
    const h2_lua_display_primitive_t *p = &primitives[i];
    const h2_lua_display_vertex_t *v = draw_positions + p->first;
    uint16_t color = recolor ? ink : p->color;
    if (p->kind == H2_LUA_DISPLAY_LINE) {
      display_clipped_line_rect_capture(job, v[0].x + offset, v[0].y,
                               v[1].x + offset, v[1].y, color,
                               (int)top, (int)bottom, (int)left, (int)right, cache);
    } else {
      double x[128], y[128];
      for (size_t j = 0; j < p->count; ++j) { x[j] = v[j].x; y[j] = v[j].y; }
      display_raster_polygon_rect_capture(job, x, y, p->count, color, offset,
                                 (int)top, (int)bottom, (int)left, (int)right, cache);
    }
  }
  if (cache != NULL) {
    if (cache->valid) {
      memcpy(mesh_span_positions(cache), draw_positions,
             mesh->vertex_count * sizeof(*draw_positions));
      memcpy(mesh_span_primitives(mesh, cache), primitives,
             mesh->primitive_count * sizeof(*primitives));
      mesh->span_result_valid = 1;
      mesh->span_vertex_count = mesh->vertex_count;
      mesh->span_primitive_count = mesh->primitive_count;
    }
    mesh->spans_valid = cache->valid;
  }
  return 0;
}

#define POLYLINE_META "h2.display.polyline"
#define LINE_STYLE_META "h2.display.line_style"
typedef struct projected_fragment {
  double xy[4];
  size_t source;
  int side;
  uint16_t color;
} projected_fragment_t;
typedef struct projected_polyline {
  size_t capacity, n, generation;
  int channels;
  double *points, *scalars, *projected;
  projected_fragment_t *fragments;
} projected_polyline_t;
typedef struct polyline_style {
  size_t count;
  int axis, minimum, divide;
  double gradient[11];
  uint16_t colors[];
} polyline_style_t;
static const double *display_f64(lua_State *s, int at, size_t n) {
  h2_numeric_buffer_t *b = h2_numeric_check(s, at);
  if (b->is_f32)
    luaL_error(s, "prepared drawing requires f64 buffers");
  h2_numeric_capacity(s, b, n);
  return b->data.f64;
}
static double polyline_value(lua_State *s, double x) {
  if (!isfinite(x) || fabs(x) > 1e6)
    luaL_error(s, "polyline value out of bounds");
  return x;
}
static int polyline_load(lua_State *s) {
  projected_polyline_t *p = luaL_checkudata(s, 1, POLYLINE_META);
  size_t n = h2_numeric_size(s, 3, p->capacity);
  const double *points = display_f64(s, 2, 3 * n);
  const double *channels =
      lua_isnoneornil(s, 4) ? NULL : display_f64(s, 4, 3 * n);
  for (size_t i = 0; i < 3 * n; ++i) {
    polyline_value(s, points[i]);
    if (channels)
      polyline_value(s, channels[i]);
  }
  memcpy(p->points, points, 3 * n * sizeof(double));
  if (channels)
    memcpy(p->scalars, channels, 3 * n * sizeof(double));
  p->n = n;
  p->channels = channels != NULL;
  ++p->generation;
  return 0;
}
static int display_polyline_new(lua_State *s) {
  size_t n = h2_numeric_size(s, 1, 256);
  projected_polyline_t *p = lua_newuserdatauv(s, sizeof(*p), 2);
  memset(p, 0, sizeof(*p));
  p->capacity = n;
  if (luaL_newmetatable(s, POLYLINE_META)) {
    lua_pushcfunction(s, polyline_load);
    lua_setfield(s, -2, "load");
    lua_pushvalue(s, -1);
    lua_setfield(s, -2, "__index");
    lua_pushliteral(s, "projected polyline");
    lua_setfield(s, -2, "__metatable");
  }
  lua_setmetatable(s, -2);
  int at = lua_gettop(s);
  p->points = lua_newuserdatauv(s, 8 * n * sizeof(double), 0);
  p->scalars = p->points + 3 * n;
  p->projected = p->scalars + 3 * n;
  lua_setiuservalue(s, at, 1);
  p->fragments = lua_newuserdatauv(
      s, (n ? 2 * (n - 1) : 0) * sizeof(projected_fragment_t), 0);
  lua_setiuservalue(s, at, 2);
  return 1;
}
static int display_line_style(lua_State *s) {
  h2_numeric_buffer_t *input = h2_numeric_check(s, 1);
  int gradient = !lua_isnoneornil(s, 2);
  size_t count = gradient ? 0 : input->count;
  if (count > 255)
    return luaL_error(s, "too many source colors");
  const double *values = display_f64(s, 1, gradient ? 11 : count);
  int axis = 0, minimum = 0, divide = 0;
  if (gradient) {
    axis = (int)h2_numeric_size(s, 2, 6);
    if (!axis)
      return luaL_error(s, "gradient axis/channel is one-based");
    static const char *const reducers[] = {"mean", "min", NULL};
    static const char *const operations[] = {"multiply", "divide", NULL};
    minimum = luaL_checkoption(s, 3, NULL, reducers);
    divide = luaL_checkoption(s, 4, NULL, operations);
  }
  polyline_style_t *style =
      lua_newuserdatauv(s, sizeof(*style) + count * sizeof(uint16_t), 0);
  memset(style, 0, sizeof(*style));
  style->count = count;
  style->axis = axis;
  style->minimum = minimum;
  style->divide = divide;
  if (gradient) {
    for (size_t i = 0; i < 11; ++i)
      style->gradient[i] = polyline_value(s, values[i]);
    for (size_t i = 0; i < 6; ++i)
      if (values[i] < 0 || values[i] > 255)
        return luaL_error(s, "invalid RGB888 channel");
    if ((divide && values[7] == 0) || values[9] < 0 || values[10] > 1 ||
        values[9] > values[10])
      return luaL_error(s, "invalid gradient range");
  } else
    for (size_t i = 0; i < count; ++i) {
      double c = values[i];
      if (c < 0 || c > 65535 || floor(c) != c)
        return luaL_error(s, "invalid RGB565 color");
      style->colors[i] = (uint16_t)c;
    }
  luaL_newmetatable(s, LINE_STYLE_META);
  lua_pushliteral(s, "compiled line style");
  lua_setfield(s, -2, "__metatable");
  lua_setmetatable(s, -2);
  return 1;
}
static uint16_t polyline_color(lua_State *s, const polyline_style_t *style,
                               const projected_polyline_t *p, size_t source) {
  if (!style->axis)
    return style->colors[source];
  const double *values = style->axis <= 3 ? p->points : p->scalars;
  size_t axis = (size_t)(style->axis - 1) % 3;
  double a = values[3 * source + axis], b = values[3 * (source + 1) + axis];
  double value = style->minimum ? fmin(a, b) : (a + b) * .5;
  const double *g = style->gradient;
  double delta = value - g[6],
         factor = (style->divide ? delta / g[7] : delta * g[7]) + g[8];
  if (!isfinite(factor))
    luaL_error(s, "gradient result is not finite");
  double t = fmax(g[9], fmin(g[10], factor));
  uint8_t rgb[3];
  for (size_t i = 0; i < 3; ++i)
    rgb[i] = (uint8_t)floor(g[i] * (1 - t) + g[i + 3] * t);
  return rgb_to_rgb565(rgb[0], rgb[1], rgb[2]);
}
static void polyline_project(lua_State *s, const double *p,
                             const double *camera, double *out) {
  out[0] = polyline_value(s, camera[0] + camera[2] * p[0] / p[2]);
  out[1] = polyline_value(s, camera[1] + camera[2] * (camera[3] - p[1]) / p[2]);
}
static int polyline_fragment(lua_State *s, const double *from, const double *to,
                             const double *camera, const double *cached_a,
                             const double *cached_b, projected_fragment_t *f) {
  double a[3], b[3];
  memcpy(a, from, sizeof(a));
  memcpy(b, to, sizeof(b));
  double near = camera[4];
  if (a[2] < near && b[2] < near)
    return 0;
  if (a[2] < near || b[2] < near) {
    double t = (near - a[2]) / (b[2] - a[2]);
    double x = a[0] + (b[0] - a[0]) * t, y = a[1] + (b[1] - a[1]) * t;
    double *cut = a[2] < near ? a : b;
    if (a[2] < near)
      cached_a = NULL;
    else
      cached_b = NULL;
    cut[0] = x;
    cut[1] = y;
    cut[2] = near;
  }
  if (cached_a)
    memcpy(f->xy, cached_a, 2 * sizeof(double));
  else
    polyline_project(s, a, camera, f->xy);
  if (cached_b)
    memcpy(f->xy + 2, cached_b, 2 * sizeof(double));
  else
    polyline_project(s, b, camera, f->xy + 2);
  return 1;
}
static int display_draw_polyline(lua_State *s) {
  h2_lua_job_t *job = lua_touserdata(s, lua_upvalueindex(1));
  projected_polyline_t *p = luaL_checkudata(s, 1, POLYLINE_META);
  const double *camera = display_f64(s, 2, 5);
  size_t axis = h2_numeric_size(s, 3, 3);
  double offset = h2_numeric_number(s, 4);
  luaL_checktype(s, 5, LUA_TBOOLEAN);
  int reverse = lua_toboolean(s, 5);
  polyline_style_t *styles[3];
  for (int i = 0; i < 3; ++i)
    styles[i] = luaL_checkudata(s, 6 + i, LINE_STYLE_META);
  lua_Integer left = luaL_checkinteger(s, 9), top = luaL_checkinteger(s, 10);
  lua_Integer right = luaL_checkinteger(s, 11),
              bottom = luaL_checkinteger(s, 12);
  if (!job->display_open || !axis || left < 0 || top < 0 || right < left ||
      bottom < top || right > job->display_info.width ||
      bottom > job->display_info.height)
    return luaL_error(s, "invalid polyline clip or closed display");
  --axis;
  for (size_t i = 0; i < 5; ++i)
    polyline_value(s, camera[i]);
  if (camera[2] <= 0 || camera[4] < .001)
    return luaL_error(s, "invalid projection camera");
  for (size_t i = 0; i < 3; ++i) {
    if ((!styles[i]->axis && styles[i]->count != (p->n ? p->n - 1 : 0)) ||
        (styles[i]->axis > 3 && !p->channels))
      return luaL_error(s, "invalid source style extent/channel");
  }
  for (size_t i = 0; i < p->n; ++i)
    if (p->points[3 * i + 2] >= camera[4])
      polyline_project(s, p->points + 3 * i, camera, p->projected + 2 * i);
  size_t fragments = 0;
  for (size_t k = 0; k + 1 < p->n; ++k) {
    size_t source = reverse ? p->n - 2 - k : k;
    const double *a = p->points + 3 * source, *b = a + 3;
    double sign_a = a[axis] - offset, sign_b = b[axis] - offset;
    /* Validate even hidden/unused styles before any raster write. Each style
     * is evaluated at most once per original source segment. */
    uint16_t colors[3];
    for (size_t i = 0; i < 3; ++i) {
      size_t same = 0;
      while (same < i && styles[same] != styles[i])
        ++same;
      colors[i] =
          same < i ? colors[same] : polyline_color(s, styles[i], p, source);
    }
    if ((sign_a < 0 && sign_b > 0) || (sign_a > 0 && sign_b < 0)) {
      double t = (offset - a[axis]) / (b[axis] - a[axis]), cut[3];
      for (size_t j = 0; j < 3; ++j)
        cut[j] = a[j] + t * (b[j] - a[j]);
      cut[axis] = offset;
      for (size_t j = 0; j < 2; ++j) {
        projected_fragment_t *f = p->fragments + fragments;
        int negative = (j ? sign_b : sign_a) < 0;
        f->source = source;
        f->side = negative ? -1 : 1;
        f->color = colors[negative];
        fragments += (size_t)polyline_fragment(
            s, j ? cut : a, j ? b : cut, camera,
            !j && a[2] >= camera[4] ? p->projected + 2 * source : NULL,
            j && b[2] >= camera[4] ? p->projected + 2 * (source + 1) : NULL, f);
      }
    } else {
      size_t style =
          sign_a < 0 && sign_b < 0 ? 1 : (sign_a > 0 && sign_b > 0 ? 0 : 2);
      projected_fragment_t *f = p->fragments + fragments;
      f->source = source;
      f->side = style == 2 ? 0 : (style ? -1 : 1);
      f->color = colors[style];
      if (a[2] >= camera[4] && b[2] >= camera[4]) {
        memcpy(f->xy, p->projected + 2 * source, 4 * sizeof(double));
        ++fragments;
      } else
        fragments += (size_t)polyline_fragment(
            s, a, b, camera,
            a[2] >= camera[4] ? p->projected + 2 * source : NULL,
            b[2] >= camera[4] ? p->projected + 2 * (source + 1) : NULL, f);
    }
  }
  if (left == right || top == bottom)
    return 0;
  for (size_t i = 0; i < fragments; ++i) {
    const projected_fragment_t *f = p->fragments + i;
    display_clipped_line_rect(job, f->xy[0], f->xy[1], f->xy[2], f->xy[3],
                              f->color, (int)top, (int)bottom, (int)left,
                              (int)right);
  }
  return 0;
}

static int display_draw_pose(lua_State *s) {
  h2_lua_job_t *job = lua_touserdata(s, lua_upvalueindex(1));
  h2_geometry_pose_t *pose = luaL_checkudata(s, 1, H2_GEOMETRY_POSE_META);
  h2_numeric_buffer_t *colors = h2_numeric_check(s, 2);
  double origin = h2_numeric_number(s, 3), ox = h2_numeric_number(s, 4),
         oy = h2_numeric_number(s, 5);
  double layer = h2_numeric_number(s, 6), scale = h2_numeric_number(s, 7),
         offset = h2_numeric_number(s, 8);
  lua_Integer left = luaL_checkinteger(s, 9), top = luaL_checkinteger(s, 10);
  lua_Integer right = luaL_checkinteger(s, 11),
              bottom = luaL_checkinteger(s, 12);
  int tinted = !lua_isnoneornil(s, 13);
  uint16_t tint = tinted ? check_color(s, 13) : 0;
  /* Color getters may close/reopen Display or evaluate the pose. Validate all
   * borrowed values and acquisition after the last reentrant argument decode.
   */
  if (!pose->valid || !job->display_open || colors->is_f32 || scale <= 0 ||
      scale > 16 || left < 0 || top < 0 || right < left || bottom < top ||
      right > job->display_info.width || bottom > job->display_info.height)
    return luaL_error(s, "invalid pose draw or closed display");
  const h2_geometry_batch_t *g = pose->geometry;
  h2_numeric_capacity(s, colors, g->parts);
  for (size_t i = 0; i < g->parts; ++i) {
    double c = colors->data.f64[i];
    if (c < 0 || c > 65535 || floor(c) != c)
      return luaL_error(s, "invalid primitive color");
  }
  for (size_t i = 0; i < g->n; ++i) {
    const double *p = pose->positions + 3 * i;
    double x = p[0], y = p[1];
    if (pose->projected) {
      x = (origin + x) + ox;
      y = (y - layer * p[2]) + oy;
    }
    x *= scale;
    y *= scale;
    if (!isfinite(x) || !isfinite(y) || fabs(x) > 1e6 || fabs(y) > 1e6)
      return luaL_error(s, "pose draw coordinate out of bounds");
    pose->screen[2 * i] = x;
    pose->screen[2 * i + 1] = y;
  }
  if (left == right || top == bottom)
    return 0;
  for (size_t i = 0; i < g->parts; ++i) {
    const h2_geometry_part_t *p = g->topology + i;
    uint16_t color = tinted ? tint : (uint16_t)colors->data.f64[i];
    if (p->kind) {
      const double *v = pose->screen + 2 * p->first;
      display_clipped_line_rect(job, v[0] + offset, v[1], v[2] + offset, v[3],
                                color, (int)top, (int)bottom, (int)left,
                                (int)right);
    } else {
      double x[128], y[128];
      for (size_t j = 0; j < p->count; ++j) {
        x[j] = pose->screen[2 * (p->first + j)];
        y[j] = pose->screen[2 * (p->first + j) + 1];
      }
      display_raster_polygon_rect_capture(job, x, y, p->count, color, offset,
                                          (int)top, (int)bottom, (int)left,
                                          (int)right, NULL);
    }
  }
  return 0;
}

typedef struct display_stroke_data {
  size_t count;
  double x[256], y[256], width[256];
  uint16_t color[256];
} display_stroke_data_t;

typedef struct display_stroke_cache {
  display_stroke_data_t data;
  double offset;
  int top, bottom, width, height, fast;
  /* Aligned display_span_cache_t follows this header. */
} display_stroke_cache_t;

typedef struct display_stroke_normals {
  size_t count;
  double data[]; /* x, y, lengths, unit-normal-x, unit-normal-y */
} display_stroke_normals_t;

typedef struct display_smooth_scratch {
  size_t bytes;
  uint16_t pixels[];
} display_smooth_scratch_t;

static const char s_stroke_cache_key = 0, s_stroke_normals_key = 0;
#define H2_LUA_STROKE_META "h2.display.stroke"
#define H2_LUA_NORMALS_META "h2.display.normals"

static int display_optional_boolean(lua_State *state, int index) {
  if (!lua_isnoneornil(state, index)) luaL_checktype(state, index, LUA_TBOOLEAN);
  return lua_toboolean(state, index);
}

/* Conservative legacy guard: retain spans in the validation pass; otherwise
 * use double geometry. It never snaps the input path to a pixel grid. */
static int display_stroke_fast_quad(h2_lua_job_t *job, double ax, double ay,
                                     double bx, double by, double width,
                                     int top, int bottom, uint16_t color,
                                     double offset, display_span_cache_t *cache) {
  enum { k_rows = 512 };
  if (bottom - top > k_rows) return 0;
  float left[k_rows], right[k_rows];
  double extent = fmax(fmax(fabs(ax), fabs(ay)), fmax(fabs(bx), fabs(by)));
  if (extent > 4096 || width > 64 || width <= 0) return 0;
  float dx = (float)(bx - ax), dy = (float)(by - ay);
  float length = sqrtf(dx * dx + dy * dy);
  if (length < .02f) return 0;
  float scale = h2_f32_div((float)width * .5f, length);
  float nx = -dy * scale, ny = dx * scale;
  float x[] = {(float)ax + nx, (float)bx + nx, (float)bx - nx, (float)ax - nx};
  float y[] = {(float)ay + ny, (float)by + ny, (float)by - ny, (float)ay - ny};
  float bound = 64 * FLT_EPSILON * ((float)extent + (float)width + 1);
  for (int i = 0; i < 4; ++i) {
    float fraction = y[i] - floorf(y[i]);
    if (fraction <= bound || fraction >= 1 - bound) return 0;
  }
  int first = (int)ceilf(fminf(fminf(y[0], y[1]), fminf(y[2], y[3])));
  int end = (int)ceilf(fmaxf(fmaxf(y[0], y[1]), fmaxf(y[2], y[3])));
  if (first < top) first = top;
  if (end > bottom) end = bottom;
  if (first >= end) return 1;
  for (int row = first; row < end; ++row) {
    left[row - top] = FLT_MAX;
    right[row - top] = -FLT_MAX;
  }
  for (int i = 0; i < 4; ++i) {
    int j = (i + 1) % 4;
    int begin = (int)ceilf(fminf(y[i], y[j]));
    int stop = (int)ceilf(fmaxf(y[i], y[j]));
    if (begin < top) begin = top;
    if (stop > bottom) stop = bottom;
    if (begin >= stop) continue;
    float ex = x[j] - x[i], ey = y[j] - y[i];
    float denominator = fabsf(ey) - 2 * bound;
    if (denominator <= 0) return 0;
    float slope = h2_f32_div(ex, ey);
    float error = 4 * bound * (1 + h2_f32_div(fabsf(ex), denominator)) +
                  16 * FLT_EPSILON * (fabsf(x[i]) + fabsf(ex) + 1);
    if (error >= .25f) return 0;
    for (int row = begin; row < stop; ++row) {
      float hit = x[i] + ((float)row - y[i]) * slope;
      float fraction = hit - floorf(hit);
      if (fraction <= error || fraction >= 1 - error) return 0;
      if (hit < left[row - top]) left[row - top] = hit;
      if (hit > right[row - top]) right[row - top] = hit;
    }
  }
  for (int row = first; row < end; ++row) {
    if (left[row - top] == FLT_MAX) continue;
    int edge = (int)ceilf(left[row - top]);
    int a = (int)floor((double)edge + offset + .5);
    int b = a + (int)floorf(right[row - top]) - edge;
    if (a < 0) a = 0;
    if (b >= job->display_info.width) b = job->display_info.width - 1;
    if (a > b) continue;
    fill_span(job, row, a, b, color);
    mark_dirty_rect(job, a, row, b - a + 1, 1);
    display_cache_record(cache, a, b, row, -1, color);
  }
  return 1;
}

static void display_stroke_simplify(display_stroke_data_t *path, double tolerance) {
  size_t write = 0;
  for (size_t first = 0; first + 1 < path->count;) {
    size_t end = first + 1;
    for (size_t candidate = end + 1; candidate < path->count; ++candidate) {
      if (path->width[candidate - 1] != path->width[first] ||
          path->color[candidate - 1] != path->color[first]) break;
      double dx = path->x[candidate] - path->x[first];
      double dy = path->y[candidate] - path->y[first];
      double length2 = dx * dx + dy * dy;
      if (length2 < 1e-12) break;
      int valid = 1;
      for (size_t j = first + 1; j < candidate; ++j) {
        double px = path->x[j] - path->x[first], py = path->y[j] - path->y[first];
        double dot = px * dx + py * dy, cross = px * dy - py * dx;
        if (dot < 0 || dot > length2 || cross * cross > tolerance * tolerance * length2) {
          valid = 0;
          break;
        }
      }
      if (!valid) break;
      end = candidate;
    }
    path->width[write] = path->width[first];
    path->color[write] = path->color[first];
    path->x[write + 1] = path->x[end];
    path->y[write + 1] = path->y[end];
    ++write;
    first = end;
  }
  path->count = write + 1;
}

static void display_stroke_smooth(lua_State *state, h2_lua_job_t *job,
                                   const display_stroke_data_t *path,
                                   double offset, int top, int bottom) {
  double min_x = job->display_info.width, max_x = 0, min_y = bottom, max_y = top;
  for (size_t i = 0; i + 1 < path->count; ++i) {
    double radius = path->width[i] * .5 + 1;
    min_x = fmin(min_x, fmin(path->x[i], path->x[i+1]) + offset - radius);
    max_x = fmax(max_x, fmax(path->x[i], path->x[i+1]) + offset + radius);
    min_y = fmin(min_y, fmin(path->y[i], path->y[i+1]) - radius);
    max_y = fmax(max_y, fmax(path->y[i], path->y[i+1]) + radius);
  }
  int width = job->display_info.width, height = job->display_info.height;
  int left = (int)fmax(0, fmin(width, floor(min_x)));
  int right = (int)fmax(0, fmin(width, ceil(max_x)));
  int first = (int)fmax(top, fmin(bottom, floor(min_y)));
  int end = (int)fmax(top, fmin(bottom, ceil(max_y)));
  if (right <= left || end <= first) return;
  size_t stride = (size_t)(right - left);
  if ((size_t)(end - first) > SIZE_MAX / stride)
    luaL_error(state, "smooth stroke size overflow");
  size_t count = stride * (size_t)(end - first);
  if (count > (SIZE_MAX - sizeof(display_smooth_scratch_t)) / 3u)
    luaL_error(state, "smooth stroke size overflow");
  size_t bytes = count * 3u;
  display_smooth_scratch_t *scratch = job->display_smooth;
  if (scratch == NULL || scratch->bytes < bytes) {
    scratch = lua_newuserdatauv(state, sizeof(*scratch) + bytes, 0);
    scratch->bytes = bytes;
    int ref = luaL_ref(state, LUA_REGISTRYINDEX);
    if (!job->display_open || job->display_info.width != width ||
        job->display_info.height != height) {
      luaL_unref(state, LUA_REGISTRYINDEX, ref);
      luaL_error(state, "display changed during smooth stroke allocation");
    }
    int old = job->display_smooth_ref;
    job->display_smooth = scratch;
    job->display_smooth_ref = ref;
    if (old > 0) luaL_unref(state, LUA_REGISTRYINDEX, old);
  }
  uint16_t *ink = scratch->pixels;
  uint8_t *coverage = (uint8_t *)(ink + count);
  memset(coverage, 0, count);
  for (size_t i = 0; i + 1 < path->count; ++i) {
    if (path->width[i] <= 0) continue;
    double ax = path->x[i] + offset, ay = path->y[i];
    double bx = path->x[i+1] + offset, by = path->y[i+1];
    double dx = bx - ax, dy = by - ay, length2 = dx*dx + dy*dy;
    double radius = path->width[i] * .5;
    int l = (int)fmax(left, fmin(right, floor(fmin(ax,bx) - radius - 1)));
    int r = (int)fmax(left, fmin(right, ceil(fmax(ax,bx) + radius + 1)));
    int t = (int)fmax(first, fmin(end, floor(fmin(ay,by) - radius - 1)));
    int b = (int)fmax(first, fmin(end, ceil(fmax(ay,by) + radius + 1)));
    int local_float = fmax(fmax(fabs(ax),fabs(ay)),fmax(fabs(bx),fabs(by))) <= 4096;
    float fax = (float)ax, fay = (float)ay, fdx = (float)dx, fdy = (float)dy;
    float length2f = fdx*fdx + fdy*fdy;
    float inverse = length2f > 1e-12f ? h2_f32_div(1, length2f) : 0;
    float edge = (float)radius + .5f, inner = (float)radius - .5f;
    for (int yy = t; yy < b; ++yy) for (int xx = l; xx < r; ++xx) {
      unsigned alpha;
      if (local_float) {
        float px = (float)xx + .5f - fax, py = (float)yy + .5f - fay;
        float u = (px*fdx + py*fdy)*inverse;
        if (u < 0) u = 0; else if (u > 1) u = 1;
        float ex = px - u*fdx, ey = py - u*fdy, squared = ex*ex + ey*ey;
        if (squared >= edge*edge) continue;
        if (inner >= 0 && squared <= inner*inner) alpha = 255;
        else {
          float a = (edge - sqrtf(squared))*255 + .5f;
          alpha = a <= 0 ? 0 : (a >= 255 ? 255 : (unsigned)a);
        }
      } else {
        double u = length2 > 1e-12 ? ((xx+.5-ax)*dx + (yy+.5-ay)*dy)/length2 : 0;
        u = fmax(0, fmin(1, u));
        double ex = xx+.5-ax-u*dx, ey = yy+.5-ay-u*dy;
        alpha = (unsigned)(fmax(0, fmin(1, radius+.5-sqrt(ex*ex+ey*ey)))*255+.5);
      }
      size_t at = (size_t)(yy-first)*stride + (size_t)(xx-left);
      if (alpha > coverage[at]) { coverage[at] = (uint8_t)alpha; ink[at] = path->color[i]; }
    }
  }
  for (int y = first; y < end; ++y) for (int x = left; x < right; ++x) {
    size_t at = (size_t)(y-first)*stride + (size_t)(x-left);
    if (coverage[at]) blend_pixel(job, x, y, ink[at], coverage[at]);
  }
  mark_dirty_rect(job, left, first, right-left, end-first);
}

static int display_stroke_path(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  display_stroke_data_t path;
  luaL_checktype(state, 1, LUA_TTABLE);
  luaL_checktype(state, 2, LUA_TTABLE);
  lua_settop(state, 11);
  /* Raw descriptor fields cannot run getters. Keep the buffer rooted on the
   * stack while colors may call Lua or collect/mutate the descriptor. */
  lua_pushliteral(state, "buffer");
  lua_rawget(state, 1);
  int buffer_at = lua_gettop(state);
  lua_pushliteral(state, "count");
  lua_rawget(state, 1);
  int packed = !lua_isnil(state, buffer_at) || !lua_isnil(state, -1);
  h2_numeric_buffer_t *xy = NULL;
  if (packed) {
    path.count = h2_numeric_size(state, -1, 256);
    xy = h2_numeric_check(state, buffer_at);
    if (xy->is_f32)
      return luaL_error(state, "stroke coordinates require f64");
    h2_numeric_capacity(state, xy, 2 * path.count);
    /* rawlen alone misses sparse numeric point entries. */
    lua_pushnil(state);
    while (lua_next(state, 1)) {
      if (lua_type(state, -2) == LUA_TNUMBER)
        return luaL_error(state, "mixed stroke points and buffer descriptor");
      lua_pop(state, 1);
    }
  } else
    path.count = lua_rawlen(state, 1);
  lua_pop(state, 1);
  if (path.count < 2 || path.count > 256 || lua_rawlen(state, 2) != path.count-1)
    return luaL_error(state, "invalid stroke point/width count");
  double offset = optional_geometry_number(state, 4, 0);
  double scale = optional_geometry_number(state, 10, 1);
  double tolerance = optional_geometry_number(state, 11, 0);
  int retain = display_optional_boolean(state, 7);
  int fast = display_optional_boolean(state, 8);
  int smooth = display_optional_boolean(state, 9);
  if (scale <= 0 || scale > 16 || tolerance < 0 || tolerance > .25)
    return luaL_error(state, "invalid stroke scale/tolerance");
  int colors = lua_istable(state, 3) && lua_rawlen(state, 3) > 0;
  uint16_t single = colors ? 0 : check_color(state, 3);
  if (colors && lua_rawlen(state, 3) != path.count-1)
    return luaL_error(state, "invalid stroke color count");
  for (size_t i = 0; i < path.count; ++i) {
    if (packed) {
      path.x[i] = check_geometry_value(state, 1, xy->data.f64[2 * i]);
      path.y[i] = check_geometry_value(state, 1, xy->data.f64[2 * i + 1]);
    } else {
      lua_rawgeti(state, 1, (lua_Integer)i+1);
      luaL_checktype(state, -1, LUA_TTABLE);
      lua_rawgeti(state, -1, 1); path.x[i] = check_geometry_number(state, -1); lua_pop(state, 1);
      lua_rawgeti(state, -1, 2); path.y[i] = check_geometry_number(state, -1); lua_pop(state, 2);
    }
    if (i + 1 < path.count) {
      lua_rawgeti(state, 2, (lua_Integer)i+1);
      path.width[i] = check_geometry_number(state, -1);
      lua_pop(state, 1);
      if (path.width[i] < 0 || path.width[i] > 1000)
        return luaL_error(state, "invalid stroke width");
      if (colors) {
        lua_rawgeti(state, 3, (lua_Integer)i+1);
        path.color[i] = check_color(state, -1);
        lua_pop(state, 1);
      } else path.color[i] = single;
      path.width[i] *= scale;
    }
    path.x[i] *= scale;
    path.y[i] *= scale;
  }
  int top, bottom;
  display_check_clip(state, job, 5, 6, &top, &bottom);
  if (smooth) {
    if (tolerance > 0) display_stroke_simplify(&path, tolerance);
    display_stroke_smooth(state, job, &path, offset, top, bottom);
    lua_pushboolean(state, 0); lua_pushinteger(state, 0);
    return 2;
  }
  display_stroke_cache_t *retained = NULL;
  display_span_cache_t *cache = NULL;
  if (retain) {
    lua_rawgetp(state, 2, &s_stroke_cache_key);
    retained = luaL_testudata(state, -1, H2_LUA_STROKE_META);
    if (retained == NULL) {
      lua_pop(state, 1);
      retained = lua_newuserdatauv(state, sizeof(*retained) + sizeof(*cache) +
          2048u*sizeof(display_cached_span_t), 0);
      memset(retained, 0, sizeof(*retained) + sizeof(*cache));
      if (luaL_newmetatable(state, H2_LUA_STROKE_META)) {
        lua_pushliteral(state, "display stroke cache");
        lua_setfield(state, -2, "__metatable");
      }
      lua_setmetatable(state, -2);
      lua_pushvalue(state, -1); lua_rawsetp(state, 2, &s_stroke_cache_key);
    }
    cache = (display_span_cache_t *)(retained + 1);
  }
  display_stroke_normals_t *normals = NULL;
  if (fast) {
    lua_rawgetp(state, 1, &s_stroke_normals_key);
    normals = luaL_testudata(state, -1, H2_LUA_NORMALS_META);
    if (normals == NULL || normals->count != path.count) {
      lua_pop(state, 1);
      normals = lua_newuserdatauv(state, sizeof(*normals) + 5*path.count*sizeof(double), 0);
      normals->count = 0;
      if (luaL_newmetatable(state, H2_LUA_NORMALS_META)) {
        lua_pushliteral(state, "display stroke normals");
        lua_setfield(state, -2, "__metatable");
      }
      lua_setmetatable(state, -2);
      lua_pushvalue(state, -1); lua_rawsetp(state, 1, &s_stroke_normals_key);
    }
  }
  /* All allocation/getters finished; recheck acquisition before cache or draw. */
  display_check_clip(state, job, 5, 6, &top, &bottom);
  if (cache != NULL) {
    const display_stroke_data_t *old = &retained->data;
    if (cache->valid && old->count == path.count && retained->offset == offset &&
        retained->top == top && retained->bottom == bottom && retained->fast == fast &&
        retained->width == job->display_info.width && retained->height == job->display_info.height &&
        !memcmp(old->x, path.x, path.count*sizeof(double)) &&
        !memcmp(old->y, path.y, path.count*sizeof(double)) &&
        !memcmp(old->width, path.width, (path.count-1)*sizeof(double)) &&
        !memcmp(old->color, path.color, (path.count-1)*sizeof(uint16_t))) {
      display_cache_replay(job, cache);
      lua_pushboolean(state, 1); lua_pushinteger(state, 0);
      return 2;
    }
    retained->data.count = path.count;
    memcpy(retained->data.x, path.x, path.count*sizeof(double));
    memcpy(retained->data.y, path.y, path.count*sizeof(double));
    memcpy(retained->data.width, path.width, (path.count-1)*sizeof(double));
    memcpy(retained->data.color, path.color, (path.count-1)*sizeof(uint16_t));
    retained->offset = offset; retained->top = top; retained->bottom = bottom;
    retained->width = job->display_info.width; retained->height = job->display_info.height;
    retained->fast = fast;
    cache->capacity = 2048; cache->count = 0; cache->valid = 1;
  }
  double *lengths = NULL, *normal_x = NULL, *normal_y = NULL;
  if (normals != NULL) {
    lengths = normals->data + 2*path.count;
    normal_x = normals->data + 3*path.count;
    normal_y = normals->data + 4*path.count;
    if (normals->count != path.count || memcmp(normals->data,path.x,path.count*sizeof(double)) ||
        memcmp(normals->data+path.count,path.y,path.count*sizeof(double))) {
      normals->count = path.count;
      memcpy(normals->data,path.x,path.count*sizeof(double));
      memcpy(normals->data+path.count,path.y,path.count*sizeof(double));
      for (size_t i = 0; i + 1 < path.count; ++i) lengths[i] = -1;
    }
  }
  int fast_count = 0;
  for (size_t i = 0; i + 1 < path.count; ++i) {
    double ax = path.x[i], ay = path.y[i], bx = path.x[i+1], by = path.y[i+1];
    double w = path.width[i];
    if (fast && display_stroke_fast_quad(job,ax,ay,bx,by,w,top,bottom,path.color[i],offset,cache)) {
      ++fast_count;
    } else {
      double dx = bx-ax, dy = by-ay, length, ux = 0, uy = 0;
      if (lengths != NULL && lengths[i] >= 0) {
        length = lengths[i]; ux = normal_x[i]; uy = normal_y[i];
      } else {
        length = sqrt(dx*dx + dy*dy);
        if (length >= .01) { ux = -dy/length; uy = dx/length; }
        if (lengths != NULL) { lengths[i] = length; normal_x[i] = ux; normal_y[i] = uy; }
      }
      if (length < .01) {
        int left = (int)floor(ax-w/2+offset+.5), row = (int)floor(ay-w/2+.5);
        int side = (int)floor(w+.5);
        int a = left < 0 ? 0 : left;
        int b = left+side < job->display_info.width ? left+side : job->display_info.width;
        int first = row < top ? top : row, end = row+side < bottom ? row+side : bottom;
        for (int y = first; y < end && a < b; ++y) {
          fill_span(job,y,a,b-1,path.color[i]); mark_dirty_rect(job,a,y,b-a,1);
          display_cache_record(cache,a,b-1,y,-1,path.color[i]);
        }
        continue;
      }
      double nx = ux*w/2, ny = uy*w/2;
      double x[] = {ax+nx,bx+nx,bx-nx,ax-nx}, y[] = {ay+ny,by+ny,by-ny,ay-ny};
      display_raster_polygon_rect_capture(job,x,y,4,path.color[i],offset,top,bottom,
                                           0,job->display_info.width,cache);
    }
    display_clipped_line_rect_capture(job,ax+offset,ay,bx+offset,by,path.color[i],
                                       top,bottom,0,job->display_info.width,cache);
  }
  lua_pushboolean(state, 0); lua_pushinteger(state, fast_count);
  return 2;
}

static int display_fill_rect(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  int x = check_pixel_number(state, 1);
  int y = check_pixel_number(state, 2);
  int width = check_pixel_number(state, 3);
  int height = check_pixel_number(state, 4);
  uint16_t color = check_color(state, 5);
  int py;
  if (!job->display_open || !rect_is_bounded(job, x, y, width, height)) {
    return luaL_error(state, "invalid fill_rect");
  }
  mark_dirty_rect(job, x, y, width, height);
  for (py = y; py < y + height; ++py)
    fill_span(job, py, x, x + width - 1, color);
  return 0;
}

static int display_draw_line(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  int x0 = check_pixel_number(state, 1);
  int y0 = check_pixel_number(state, 2);
  int x1 = check_pixel_number(state, 3);
  int y1 = check_pixel_number(state, 4);
  uint16_t color = check_color(state, 5);
  int64_t dx;
  int64_t dy;
  int64_t error;
  int sx;
  int sy;
  if (!job->display_open || !point_is_bounded(job, x0, y0) ||
      !point_is_bounded(job, x1, y1)) {
    return luaL_error(state, "invalid draw_line");
  }
  dx = llabs((int64_t)x1 - x0);
  sx = x0 < x1 ? 1 : -1;
  dy = -llabs((int64_t)y1 - y0);
  sy = y0 < y1 ? 1 : -1;
  mark_dirty_rect(job, x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1, (int)dx + 1,
                  (int)(-dy) + 1);
  error = dx + dy;
  for (;;) {
    int64_t twice;
    write_pixel(job, x0, y0, color);
    if (x0 == x1 && y0 == y1) {
      break;
    }
    twice = 2 * error;
    if (twice >= dy) {
      error += dy;
      x0 += sx;
    }
    if (twice <= dx) {
      error += dx;
      y0 += sy;
    }
  }
  return 0;
}

static int display_fill_circle(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  int cx = check_pixel_number(state, 1);
  int cy = check_pixel_number(state, 2);
  int radius = check_pixel_number(state, 3);
  uint16_t color = check_color(state, 4);
  int extent = 0;
  int y;
  int64_t radius_squared;
  if (!job->display_open || radius < 0 || radius > job->display_info.width ||
      radius > job->display_info.height || !point_is_bounded(job, cx, cy)) {
    return luaL_error(state, "invalid fill_circle");
  }
  mark_dirty_rect(job, cx - radius, cy - radius, radius * 2 + 1,
                  radius * 2 + 1);
  radius_squared = (int64_t)radius * radius;
  for (y = -radius; y <= radius; ++y) {
    while (extent < radius &&
           (int64_t)(extent + 1) * (extent + 1) + (int64_t)y * y <=
               radius_squared)
      ++extent;
    while (extent > 0 &&
           (int64_t)extent * extent + (int64_t)y * y > radius_squared)
      --extent;
    fill_span(job, cy + y, cx - extent, cx + extent, color);
  }
  return 0;
}

static int display_draw_circle(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  int cx = check_pixel_number(state, 1);
  int cy = check_pixel_number(state, 2);
  int radius = check_pixel_number(state, 3);
  uint16_t color = check_color(state, 4);
  int x;
  int y;
  int error;
  if (!job->display_open || radius < 0 || radius > job->display_info.width ||
      radius > job->display_info.height || !point_is_bounded(job, cx, cy)) {
    return luaL_error(state, "invalid draw_circle");
  }
  mark_dirty_rect(job, cx - radius, cy - radius, radius * 2 + 1,
                  radius * 2 + 1);
  x = radius;
  y = 0;
  error = 1 - radius;
  while (x >= y) {
    write_pixel(job, cx + x, cy + y, color);
    write_pixel(job, cx + y, cy + x, color);
    write_pixel(job, cx - y, cy + x, color);
    write_pixel(job, cx - x, cy + y, color);
    write_pixel(job, cx - x, cy - y, color);
    write_pixel(job, cx - y, cy - x, color);
    write_pixel(job, cx + y, cy - x, color);
    write_pixel(job, cx + x, cy - y, color);
    ++y;
    if (error < 0) {
      error += 2 * y + 1;
    } else {
      --x;
      error += 2 * (y - x) + 1;
    }
  }
  return 0;
}

static void draw_circle_aa_pixels(h2_lua_job_t *job, int cx, int cy,
                                  int radius, uint16_t color) {
  int radius_q4 = radius * 4 + 2;
  int radius_squared_q8 = radius_q4 * radius_q4;
  int extent = radius + 1;
  int y;
  for (y = cy - extent; y <= cy + extent; ++y) {
    int dy0_q4 = (y - cy) * 4 - 1;
    int dy1_q4 = dy0_q4 + 2;
    int dy0_squared_q8 = dy0_q4 * dy0_q4;
    int dy1_squared_q8 = dy1_q4 * dy1_q4;
    int x;
    for (x = cx - extent; x <= cx + extent; ++x) {
      int dx0_q4 = (x - cx) * 4 - 1;
      int dx1_q4 = dx0_q4 + 2;
      int dx0_squared_q8 = dx0_q4 * dx0_q4;
      int dx1_squared_q8 = dx1_q4 * dx1_q4;
      unsigned coverage =
          (unsigned)(dx0_squared_q8 + dy0_squared_q8 <= radius_squared_q8) +
          (unsigned)(dx1_squared_q8 + dy0_squared_q8 <= radius_squared_q8) +
          (unsigned)(dx0_squared_q8 + dy1_squared_q8 <= radius_squared_q8) +
          (unsigned)(dx1_squared_q8 + dy1_squared_q8 <= radius_squared_q8);
      if (coverage != 0u) {
        blend_pixel(job, x, y, color, coverage * 255u / 4u);
      }
    }
  }
}

static int display_fill_circle_aa(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  int cx = check_pixel_number(state, 1);
  int cy = check_pixel_number(state, 2);
  int radius = check_pixel_number(state, 3);
  uint16_t color = check_color(state, 4);
  if (!job->display_open || radius < 0 || radius > 64 ||
      !point_is_bounded(job, cx, cy)) {
    return luaL_error(state, "invalid fill_circle_aa");
  }
  mark_dirty_rect(job, cx - radius - 1, cy - radius - 1, radius * 2 + 3,
                  radius * 2 + 3);
  draw_circle_aa_pixels(job, cx, cy, radius, color);
  return 0;
}

static uint16_t fade_rgb565_to_black(uint16_t color,
                                     const uint16_t red_lut[32],
                                     const uint16_t green_lut[64],
                                     const uint16_t blue_lut[32]) {
  return (uint16_t)(red_lut[(color >> 11u) & 0x1fu] |
                    green_lut[(color >> 5u) & 0x3fu] |
                    blue_lut[color & 0x1fu]);
}

static void fade_region_to_black(h2_lua_job_t *job, int x, int y, int width,
                                 int height, unsigned amount) {
  static const unsigned k_fade_quantum = 38u;
  unsigned inverse;
  uint16_t red_lut[32];
  uint16_t green_lut[64];
  uint16_t blue_lut[32];
  if (amount == 0u) {
    return;
  }
  if (amount == 255u) {
    for (int row = 0; row < height; ++row) {
      uint16_t *pixels =
          job->framebuffer + (size_t)(y + row) *
                                 (size_t)job->display_info.width +
          (size_t)x;
      memset(pixels, 0, (size_t)width * sizeof(*pixels));
    }
    mark_dirty_rect(job, x, y, width, height);
    return;
  }
  /* At very high Desktop frame rates a time-correct alpha can be smaller than
   * one RGB565 channel step. Applying that amount with integer rounding either
   * erases trails too quickly or leaves dim pixels stuck forever. Spatially
   * dither a 15% reference fade instead: every pixel receives the same average
   * decay over time while each individual update remains representable. */
  inverse = 255u - (amount < k_fade_quantum ? k_fade_quantum : amount);
  for (unsigned value = 0u; value < 32u; ++value) {
    unsigned faded = value * inverse / 255u;
    red_lut[value] = (uint16_t)(faded << 11u);
    blue_lut[value] = (uint16_t)faded;
  }
  for (unsigned value = 0u; value < 64u; ++value) {
    green_lut[value] = (uint16_t)((value * inverse / 255u) << 5u);
  }
  if (amount < k_fade_quantum) {
    unsigned selector = job->display_fade_phase;
    for (int row = 0; row < height; ++row) {
      uint16_t *pixels =
          job->framebuffer + (size_t)(y + row) *
                                 (size_t)job->display_info.width +
          (size_t)x;
      for (int column = 0; column < width; ++column) {
        if (selector < amount) {
          pixels[column] = fade_rgb565_to_black(
              pixels[column], red_lut, green_lut, blue_lut);
        }
        selector += 17u;
        if (selector >= k_fade_quantum) {
          selector -= k_fade_quantum;
        }
      }
    }
    job->display_fade_phase =
        (uint8_t)((job->display_fade_phase + amount) % k_fade_quantum);
  } else {
    for (int row = 0; row < height; ++row) {
      uint16_t *pixels =
          job->framebuffer + (size_t)(y + row) *
                                 (size_t)job->display_info.width +
          (size_t)x;
      size_t count = (size_t)width;
      while (count >= 4u) {
        pixels[0] =
            fade_rgb565_to_black(pixels[0], red_lut, green_lut, blue_lut);
        pixels[1] =
            fade_rgb565_to_black(pixels[1], red_lut, green_lut, blue_lut);
        pixels[2] =
            fade_rgb565_to_black(pixels[2], red_lut, green_lut, blue_lut);
        pixels[3] =
            fade_rgb565_to_black(pixels[3], red_lut, green_lut, blue_lut);
        pixels += 4;
        count -= 4u;
      }
      while (count != 0u) {
        *pixels =
            fade_rgb565_to_black(*pixels, red_lut, green_lut, blue_lut);
        ++pixels;
        --count;
      }
    }
  }
  mark_dirty_rect(job, x, y, width, height);
}

/* Apply a translucent black overlay to the retained RGB565 framebuffer.
 * This is the embedded equivalent of Canvas2D filling each animation frame
 * with rgba(0, 0, 0, alpha), which produces smooth particle afterimages
 * without allocating or redrawing explicit trail geometry in Lua. */
static int display_fade_to_black(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  lua_Integer requested_amount = luaL_checkinteger(state, 1);
  if (!job->display_open || requested_amount < 0 || requested_amount > 255) {
    return luaL_error(state, "invalid fade_to_black");
  }
  fade_region_to_black(job, 0, 0, job->display_info.width,
                       job->display_info.height, (unsigned)requested_amount);
  return 0;
}

static int display_fade_rect_to_black(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  int x = check_pixel_number(state, 1);
  int y = check_pixel_number(state, 2);
  int width = check_pixel_number(state, 3);
  int height = check_pixel_number(state, 4);
  lua_Integer requested_amount = luaL_checkinteger(state, 5);
  if (!job->display_open || x < 0 || y < 0 || width <= 0 || height <= 0 ||
      x > job->display_info.width - width ||
      y > job->display_info.height - height || requested_amount < 0 ||
      requested_amount > 255) {
    return luaL_error(state, "invalid fade_rect_to_black");
  }
  fade_region_to_black(job, x, y, width, height,
                       (unsigned)requested_amount);
  return 0;
}

static int display_fill_round_rect(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  int x = check_pixel_number(state, 1);
  int y = check_pixel_number(state, 2);
  int width = check_pixel_number(state, 3);
  int height = check_pixel_number(state, 4);
  int radius = check_pixel_number(state, 5);
  uint16_t color = check_color(state, 6);
  int py;
  if (!job->display_open || !rect_is_bounded(job, x, y, width, height) ||
      radius < 0 || radius > width / 2 || radius > height / 2) {
    return luaL_error(state, "invalid fill_round_rect");
  }
  mark_dirty_rect(job, x, y, width, height);
  for (py = 0; py < height; ++py) {
    int inset = rounded_rect_inset(height, radius, py);
    fill_span(job, y + py, x + inset, x + width - inset - 1, color);
  }
  return 0;
}

static int display_draw_round_rect(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  int x = check_pixel_number(state, 1);
  int y = check_pixel_number(state, 2);
  int width = check_pixel_number(state, 3);
  int height = check_pixel_number(state, 4);
  int radius = check_pixel_number(state, 5);
  uint16_t color = check_color(state, 6);
  int py;
  if (!job->display_open || width <= 0 || height <= 0 ||
      !rect_is_bounded(job, x, y, width, height) || radius < 0 ||
      radius > width / 2 || radius > height / 2) {
    return luaL_error(state, "invalid draw_round_rect");
  }
  mark_dirty_rect(job, x, y, width, height);
  for (py = 0; py < height; ++py) {
    int outer_inset = rounded_rect_inset(height, radius, py);
    int inner_width = width - 2;
    int inner_height = height - 2;
    int inner_radius = radius > 0 ? radius - 1 : 0;
    int inner_row = py - 1;
    if (inner_width <= 0 || inner_height <= 0 || inner_row < 0 ||
        inner_row >= inner_height) {
      fill_span(job, y + py, x + outer_inset, x + width - outer_inset - 1,
                color);
    } else {
      int inner_inset =
          rounded_rect_inset(inner_height, inner_radius, inner_row);
      int inner_min_x = x + 1 + inner_inset;
      int inner_max_x = x + width - inner_inset - 2;
      fill_span(job, y + py, x + outer_inset, inner_min_x - 1, color);
      fill_span(job, y + py, inner_max_x + 1, x + width - outer_inset - 1,
                color);
    }
  }
  return 0;
}

static int64_t triangle_sign(int px, int py, int ax, int ay, int bx, int by) {
  return ((int64_t)px - bx) * ((int64_t)ay - by) -
         ((int64_t)ax - bx) * ((int64_t)py - by);
}

static int display_fill_triangle(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  int x0 = check_pixel_number(state, 1);
  int y0 = check_pixel_number(state, 2);
  int x1 = check_pixel_number(state, 3);
  int y1 = check_pixel_number(state, 4);
  int x2 = check_pixel_number(state, 5);
  int y2 = check_pixel_number(state, 6);
  uint16_t color = check_color(state, 7);
  int min_x = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
  int max_x = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
  int min_y = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
  int max_y = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
  int x;
  int y;
  if (!job->display_open || !point_is_bounded(job, x0, y0) ||
      !point_is_bounded(job, x1, y1) || !point_is_bounded(job, x2, y2)) {
    return luaL_error(state, "display is not open");
  }
  for (y = min_y; y <= max_y; ++y) {
    for (x = min_x; x <= max_x; ++x) {
      int64_t d0 = triangle_sign(x, y, x0, y0, x1, y1);
      int64_t d1 = triangle_sign(x, y, x1, y1, x2, y2);
      int64_t d2 = triangle_sign(x, y, x2, y2, x0, y0);
      if ((d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0)) {
        set_pixel(job, x, y, color);
      }
    }
  }
  return 0;
}

static const uint8_t *glyph_rows(unsigned char character) {
  static const uint8_t unknown[7] = {14, 17, 1, 2, 4, 0, 4};
  static const uint8_t digits[10][7] = {
      {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14},
      {14, 17, 1, 2, 4, 8, 31},     {30, 1, 1, 14, 1, 1, 30},
      {2, 6, 10, 18, 31, 2, 2},     {31, 16, 16, 30, 1, 1, 30},
      {14, 16, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8},
      {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 1, 14},
  };
  static const uint8_t letters[26][7] = {
      {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30},
      {14, 17, 16, 16, 16, 17, 14}, {30, 17, 17, 17, 17, 17, 30},
      {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16},
      {14, 17, 16, 23, 17, 17, 15}, {17, 17, 17, 31, 17, 17, 17},
      {14, 4, 4, 4, 4, 4, 14},      {7, 2, 2, 2, 18, 18, 12},
      {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31},
      {17, 27, 21, 21, 17, 17, 17}, {17, 25, 21, 19, 17, 17, 17},
      {14, 17, 17, 17, 17, 17, 14}, {30, 17, 17, 30, 16, 16, 16},
      {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17},
      {15, 16, 16, 14, 1, 1, 30},   {31, 4, 4, 4, 4, 4, 4},
      {17, 17, 17, 17, 17, 17, 14}, {17, 17, 17, 17, 17, 10, 4},
      {17, 17, 17, 21, 21, 21, 10}, {17, 17, 10, 4, 10, 17, 17},
      {17, 17, 10, 4, 4, 4, 4},     {31, 1, 2, 4, 8, 16, 31},
  };
  if (character >= '0' && character <= '9')
    return digits[character - '0'];
  if (character >= 'a' && character <= 'z')
    character -= 'a' - 'A';
  if (character >= 'A' && character <= 'Z')
    return letters[character - 'A'];
  return unknown;
}

static void draw_glyph(h2_lua_job_t *job, int x, int y, unsigned char character,
                       int scale, uint16_t color) {
  const uint8_t *rows = glyph_rows(character);
  int row;
  int column;
  int sx;
  int sy;
  if (character == ' ')
    return;
  for (row = 0; row < 7; ++row) {
    for (column = 0; column < 5; ++column) {
      if ((rows[row] & (1u << (4 - column))) == 0u)
        continue;
      for (sy = 0; sy < scale; ++sy) {
        for (sx = 0; sx < scale; ++sx) {
          set_pixel(job, x + column * scale + sx, y + row * scale + sy, color);
        }
      }
    }
  }
}

static int draw_text_at(lua_State *state, h2_lua_job_t *job, int x, int y,
                        const char *text, size_t length, uint16_t color,
                        int scale) {
  size_t i;
  if (!job->display_open || scale < 1 || scale > 8 ||
      length > job->host->config.output_limit_bytes ||
      length > (size_t)INT_MAX / (6u * (size_t)scale) ||
      !point_is_bounded(job, x, y)) {
    return luaL_error(state, "invalid draw_text");
  }
  for (i = 0u; i < length; ++i) {
    draw_glyph(job, x + (int)i * 6 * scale, y, (unsigned char)text[i], scale,
               color);
  }
  return 0;
}

static int display_draw_text(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  int x = check_pixel_number(state, 1);
  int y = check_pixel_number(state, 2);
  size_t length;
  const char *text = luaL_checklstring(state, 3, &length);
  uint16_t color = rgb_to_rgb565(255u, 255u, 255u);
  int font_size = 24;
  int scale;
  if (!lua_isnoneornil(state, 4)) {
    luaL_checktype(state, 4, LUA_TTABLE);
    lua_getfield(state, 4, "color");
    if (!lua_isnil(state, -1))
      color = check_color(state, -1);
    lua_pop(state, 1);
    lua_getfield(state, 4, "font_size");
    if (!lua_isnil(state, -1))
      font_size = (int)luaL_checkinteger(state, -1);
    lua_pop(state, 1);
  }
  if (font_size < 1 || font_size > 64)
    return luaL_error(state, "display font_size must be between 1 and 64");
  scale = (font_size + 6) / 7;
  return draw_text_at(state, job, x, y, text, length, color, scale);
}

static int display_draw_text_aligned(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  int x = check_pixel_number(state, 1);
  int y = check_pixel_number(state, 2);
  int width = check_pixel_number(state, 3);
  int height = check_pixel_number(state, 4);
  size_t length;
  const char *text = luaL_checklstring(state, 5, &length);
  uint16_t color = rgb_to_rgb565(255u, 255u, 255u);
  int font_size = 24;
  int align = 0;
  int valign = 0;
  int scale;
  int text_width;
  int text_height;
  int64_t aligned_x;
  int64_t aligned_y;
  if (!lua_isnoneornil(state, 6)) {
    const char *value;
    luaL_checktype(state, 6, LUA_TTABLE);
    lua_getfield(state, 6, "color");
    if (!lua_isnil(state, -1))
      color = check_color(state, -1);
    lua_pop(state, 1);
    lua_getfield(state, 6, "font_size");
    if (!lua_isnil(state, -1))
      font_size = (int)luaL_checkinteger(state, -1);
    lua_pop(state, 1);
    lua_getfield(state, 6, "align");
    value = lua_tostring(state, -1);
    if (value != NULL) {
      if (strcmp(value, "center") == 0 || strcmp(value, "centre") == 0)
        align = 1;
      else if (strcmp(value, "right") == 0)
        align = 2;
      else if (strcmp(value, "left") != 0)
        return luaL_error(state,
                          "display align must be left, center, or right");
    }
    lua_pop(state, 1);
    lua_getfield(state, 6, "valign");
    value = lua_tostring(state, -1);
    if (value != NULL) {
      if (strcmp(value, "middle") == 0 || strcmp(value, "center") == 0)
        valign = 1;
      else if (strcmp(value, "bottom") == 0)
        valign = 2;
      else if (strcmp(value, "top") != 0)
        return luaL_error(state,
                          "display valign must be top, middle, or bottom");
    }
    lua_pop(state, 1);
  }
  if (font_size < 1 || font_size > 64)
    return luaL_error(state, "display font_size must be between 1 and 64");
  scale = (font_size + 6) / 7;
  if (width < 0 || height < 0 || align < 0 || align > 2 || scale < 1 ||
      scale > 10 || length > (size_t)INT_MAX / (6u * (size_t)scale)) {
    return luaL_error(state, "invalid draw_text_aligned");
  }
  text_width = (int)(length * 6u * (size_t)scale);
  text_height = 7 * scale;
  aligned_x = x;
  aligned_y = y;
  if (align == 1)
    aligned_x += ((int64_t)width - text_width) / 2;
  if (align == 2)
    aligned_x += (int64_t)width - text_width;
  if (valign == 1)
    aligned_y += ((int64_t)height - text_height) / 2;
  if (valign == 2)
    aligned_y += (int64_t)height - text_height;
  if (aligned_x < INT_MIN || aligned_x > INT_MAX || aligned_y < INT_MIN ||
      aligned_y > INT_MAX) {
    return luaL_error(state, "invalid draw_text_aligned");
  }
  return draw_text_at(state, job, (int)aligned_x, (int)aligned_y, text, length,
                      color, scale);
}

static int display_integer(lua_State *state, int index, int fallback,
                            int minimum, int maximum) {
  lua_Integer value = luaL_optinteger(state, index, fallback);
  if (value < minimum || value > maximum)
    luaL_argerror(state, index, "display integer out of range");
  return (int)value;
}

static display_region_t *display_new_region(lua_State *state, int width,
                                            int height, size_t pixels,
                                            size_t runs, int masked,
                                            uint16_t key) {
  size_t bytes = sizeof(display_region_t) +
                 (size_t)height * sizeof(display_region_row_t);
  size_t tiles = display_tile_count(width, height);
  if (runs > (SIZE_MAX - bytes) / sizeof(display_region_run_t))
    luaL_error(state, "region size overflow");
  bytes += runs * sizeof(display_region_run_t);
  if (pixels > (SIZE_MAX - bytes) / sizeof(uint16_t))
    luaL_error(state, "region size overflow");
  bytes += pixels * sizeof(uint16_t);
  if (tiles > SIZE_MAX - bytes)
    luaL_error(state, "region size overflow");
  /* Create metatable first: no allocation follows returning the userdata. */
  if (luaL_newmetatable(state, H2_LUA_DISPLAY_REGION_META)) {
    lua_pushliteral(state, "display region");
    lua_setfield(state, -2, "__metatable");
  }
  display_region_t *region = lua_newuserdatauv(state, bytes + tiles, 0);
  region->width = width;
  region->height = height;
  region->masked = masked;
  region->key = key;
  region->pixel_count = pixels;
  region->run_count = runs;
  lua_pushvalue(state, -2);
  lua_setmetatable(state, -2);
  lua_remove(state, -2);
  memset(display_region_damage(region), 1, tiles);
  return region;
}

/* Streaming Python b85 alphabet reader; only four decoded bytes are buffered. */
typedef struct display_b85_reader {
  lua_State *state;
  const char *text;
  size_t length, position;
  uint32_t word;
} display_b85_reader_t;

static unsigned display_b85_byte(display_b85_reader_t *r) {
  /* Python b85 digits indexed by unsigned input byte; 255 rejects all
   * non-alphabet bytes, including NUL and bytes above ASCII. Read-only storage. */
  static const uint8_t digits[256] = {
      255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
      255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
      255,  62, 255,  63,  64,  65,  66, 255,  67,  68,  69,  70, 255,  71, 255, 255,
        0,   1,   2,   3,   4,   5,   6,   7,   8,   9, 255,  72,  73,  74,  75,  76,
       77,  10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,
       25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35, 255, 255, 255,  78,  79,
       80,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,  48,  49,  50,
       51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  81,  82,  83,  84, 255,
      255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
      255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
      255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
      255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
      255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
      255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
      255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
      255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  };
  if (r->position == r->length)
    luaL_error(r->state, "truncated region LZ4 block");
  unsigned slot = (unsigned)(r->position % 4);
  if (slot == 0) {
    uint32_t word = 0;
    const char *group = r->text + (r->position / 4) * 5;
    for (int i = 0; i < 5; ++i) {
      unsigned digit = digits[(unsigned char)group[i]];
      if (digit == 255 || word > (UINT32_MAX - digit) / 85)
        luaL_error(r->state, "invalid region base85 group");
      word = word * 85 + digit;
    }
    r->word = word;
    size_t remaining = r->length - r->position;
    if (remaining < 4 && (word & (UINT32_MAX >> (remaining * 8))) != 0)
      luaL_error(r->state, "nonzero region base85 padding");
  }
  ++r->position;
  return (r->word >> (24 - slot * 8)) & 255;
}

static size_t display_lz4_length(display_b85_reader_t *r, size_t n,
                                 size_t available) {
  if (n == 15) {
    unsigned extra;
    do {
      extra = display_b85_byte(r);
      if (n > available || extra > available - n)
        luaL_error(r->state, "region LZ4 output overflow");
      n += extra;
    } while (extra == 255);
  }
  if (n > available) luaL_error(r->state, "region LZ4 output overflow");
  return n;
}

static int display_region_from_string(lua_State *state) {
  int width = display_integer(state, 1, 0, 1, 4096);
  int height = display_integer(state, 2, 0, 1, 4096);
  size_t size;
  luaL_checktype(state, 3, LUA_TSTRING);
  const char *data = lua_tolstring(state, 3, &size);
  static const char *const encodings[] = {"rgb565be", "rgb565be-lz4-b85", NULL};
  int encoding = luaL_checkoption(state, 4, "rgb565be", encodings);
  if (!lua_isnoneornil(state, 4) && lua_rawlen(state, 4) != strlen(encodings[encoding]))
    return luaL_argerror(state, 4, "invalid region encoding");
  size_t count = (size_t)width * height;
  size_t bytes = count * 2;
  display_b85_reader_t reader = {state, NULL, 0, 0, 0};
  if (!encoding) {
    if (size != bytes) return luaL_error(state, "region RGB565 length mismatch");
  } else {
    if (size < 8) return luaL_error(state, "truncated region header");
    uint32_t length = 0;
    for (int i = 0; i < 8; ++i) {
      unsigned c = (unsigned char)data[i];
      unsigned digit = c >= '0' && c <= '9' ? c - '0' :
          c >= 'a' && c <= 'f' ? c - 'a' + 10 :
          c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
      if (digit == 16) return luaL_error(state, "invalid region hex header");
      length = length * 16 + digit;
    }
    /* Compare groups without overflowing size_t on 32-bit targets. */
    size_t groups = length / 4 + (length % 4 != 0);
    if (length == 0 || (size - 8) % 5 || (size - 8) / 5 != groups)
      return luaL_error(state, "region base85 length mismatch");
    reader.text = data + 8;
    reader.length = length;
  }
  display_region_t *region = display_new_region(state, width, height, count, 0, 0, 0);
  uint16_t *pixels = display_region_pixels(region);
  unsigned char *out = (unsigned char *)pixels;
  if (!encoding) {
    memcpy(out, data, bytes);
  } else {
    size_t position = 0, last_match_start = 0;
    int matched = 0;
    for (;;) {
      unsigned token = display_b85_byte(&reader);
      size_t literals = display_lz4_length(&reader, token >> 4, bytes - position);
      if (literals > reader.length - reader.position)
        return luaL_error(state, "truncated region LZ4 literals");
      for (size_t i = 0; i < literals; ++i) out[position++] = display_b85_byte(&reader);
      if (reader.position == reader.length) {
        if ((token & 15) != 0 || position != bytes ||
            (matched && (literals < 5 || bytes - last_match_start < 12)))
          return luaL_error(state, "invalid region LZ4 final sequence");
        break;
      }
      unsigned offset = display_b85_byte(&reader);
      offset |= display_b85_byte(&reader) << 8;
      if (offset == 0 || offset > position)
        return luaL_error(state, "invalid region LZ4 offset");
      if (bytes - position < 4)
        return luaL_error(state, "region LZ4 output overflow");
      matched = 1;
      last_match_start = position;
      size_t match = display_lz4_length(&reader, token & 15, bytes - position - 4) + 4;
      for (size_t i = 0; i < match; ++i) {
        out[position] = out[position - offset];
        ++position;
      }
    }
  }
  for (size_t i = 0; i < count; ++i)
    pixels[i] = (uint16_t)((unsigned)out[2 * i] << 8 | out[2 * i + 1]);
  for (int row = 0; row < height; ++row)
    region->rows[row] = (display_region_row_t){(size_t)row * width, 0, 0, width, 0};
  return 1;
}

static void display_check_capture(lua_State *state, h2_lua_job_t *job,
                                   int x, int y, int width, int height) {
  if (!job->display_open || width > job->display_info.width ||
      height > job->display_info.height ||
      x > job->display_info.width - width ||
      y > job->display_info.height - height)
    luaL_error(state, "invalid capture region or closed display");
}

static int display_capture_region(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  int x = display_integer(state, 1, -1, 0, 100000);
  int y = display_integer(state, 2, -1, 0, 100000);
  int width = display_integer(state, 3, 0, 1, 4096);
  int height = display_integer(state, 4, 0, 1, 4096);
  int masked = !lua_isnoneornil(state, 5);
  uint16_t key = masked ? check_color(state, 5) : 0;
  display_region_t *reuse = lua_isnoneornil(state, 6) ? NULL :
      luaL_checkudata(state, 6, H2_LUA_DISPLAY_REGION_META);
  if (reuse != NULL && (masked || reuse->masked || reuse->width != width ||
                        reuse->height != height))
    return luaL_error(state, "region reuse requires matching opaque storage");
  display_check_capture(state, job, x, y, width, height);
  size_t count = (size_t)width * height;
  display_region_t *region = reuse;
  uint16_t *captured;
  if (masked) {
    captured = lua_newuserdatauv(state, count * sizeof(uint16_t), 0);
  } else {
    if (region == NULL)
      region = display_new_region(state, width, height, count, 0, 0, 0);
    else
      lua_pushvalue(state, 6);
    captured = display_region_pixels(region);
  }
  /* Allocation may run arbitrary finalizers, including deinit/reacquire. */
  display_check_capture(state, job, x, y, width, height);
  for (int row = 0; row < height; ++row)
    memcpy(captured + (size_t)row * width,
           job->framebuffer + (size_t)(row + y) * job->display_info.width + x,
           (size_t)width * sizeof(uint16_t));
  if (masked) {
    size_t packed = 0, runs = 0;
    for (int row = 0; row < height; ++row) {
      const uint16_t *line = captured + (size_t)row * width;
      int left = 0, right = width;
      while (left < right && line[left] == key) ++left;
      while (right > left && line[right - 1] == key) --right;
      packed += (size_t)(right - left);
      for (int col = left; col < right; ++col)
        if (line[col] != key && (col == left || line[col - 1] == key)) ++runs;
    }
    region = display_new_region(state, width, height, packed, runs, 1, key);
    size_t offset = 0, run_index = 0;
    for (int row = 0; row < height; ++row) {
      const uint16_t *line = captured + (size_t)row * width;
      int left = 0, right = width;
      while (left < right && line[left] == key) ++left;
      while (right > left && line[right - 1] == key) --right;
      display_region_row_t *r = &region->rows[row];
      *r = (display_region_row_t){offset, run_index, left, right, 0};
      memcpy(display_region_pixels(region) + offset, line + left,
             (size_t)(right - left) * sizeof(uint16_t));
      offset += (size_t)(right - left);
      for (int col = left; col < right;) {
        if (line[col] == key) { ++col; continue; }
        int first = col++;
        while (col < right && line[col] != key) ++col;
        display_region_runs(region)[run_index++] =
            (display_region_run_t){(uint16_t)first, (uint16_t)col};
        ++r->run_count;
      }
    }
  } else {
    for (int row = 0; row < height; ++row)
      region->rows[row] = (display_region_row_t){(size_t)row * width, 0,
                                                0, width, 0};
    if (job->display_background == region)
      job->display_background_valid = 0;
  }
  return 1;
}

static void display_copy_region_span(h2_lua_job_t *job, int x, int y,
                                      const uint16_t *pixels, int count) {
  if (count <= 0) return;
  memcpy(job->framebuffer + (size_t)y * job->display_info.width + x, pixels,
         (size_t)count * sizeof(uint16_t));
  mark_dirty_rect(job, x, y, count, 1);
}

static int display_draw_region(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  display_region_t *region = luaL_checkudata(state, 1, H2_LUA_DISPLAY_REGION_META);
  int x = display_integer(state, 2, 0, -100000, 100000);
  int y = display_integer(state, 3, 0, -100000, 100000);
  int top = display_integer(state, 4, 0, 0, job->display_info.height);
  int bottom = display_integer(state, 5, job->display_info.height, top,
                               job->display_info.height);
  int keyed = !lua_isnoneornil(state, 6);
  uint16_t key = keyed ? check_color(state, 6) : 0;
  int left = display_integer(state, 7, 0, 0, job->display_info.width);
  int right = display_integer(state, 8, job->display_info.width, left,
                              job->display_info.width);
  if (!job->display_open || bottom > job->display_info.height)
    return luaL_error(state, "display is not open or clip changed");
  int first_x = left > x ? left - x : 0;
  int last_x = right - x < region->width ? right - x : region->width;
  int first_y = top > y ? top - y : 0;
  int last_y = bottom - y < region->height ? bottom - y : region->height;
  if (first_x >= last_x) return 0;
  uint16_t *pixels = display_region_pixels(region);
  if (!region->masked && !keyed) {
    /* Every clipped pixel is copied; one mark covers the same rectangle. */
    for (int row = first_y; row < last_y; ++row)
      memcpy(job->framebuffer + (size_t)(y + row) * job->display_info.width + x + first_x,
             pixels + region->rows[row].offset + first_x,
             (size_t)(last_x - first_x) * sizeof(uint16_t));
    mark_dirty_rect(job, x + first_x, y + first_y,
                    last_x - first_x, last_y - first_y);
    return 0;
  }
  for (int row = first_y; row < last_y; ++row) {
    display_region_row_t *r = &region->rows[row];
    if (region->masked && keyed && key == region->key) {
      for (int i = 0; i < r->run_count; ++i) {
        display_region_run_t run = display_region_runs(region)[r->first_run + i];
        int a = run.left > first_x ? run.left : first_x;
        int b = run.right < last_x ? run.right : last_x;
        if (a < b)
          display_copy_region_span(job, x + a, y + row,
              pixels + r->offset + a - r->left, b - a);
      }
    } else {
      for (int col = first_x; col < last_x; ++col) {
        uint16_t color = col < r->left || col >= r->right ? region->key :
            pixels[r->offset + (size_t)(col - r->left)];
        if (!keyed || color != key) set_pixel(job, x + col, y + row, color);
      }
    }
  }
  return 0;
}

static void display_release_background(lua_State *state, h2_lua_job_t *job) {
  int ref = job->display_background_ref;
  job->display_background = NULL;
  job->display_background_ref = 0;
  job->display_background_valid = 0;
  if (ref > 0) luaL_unref(state, LUA_REGISTRYINDEX, ref);
}

static int display_release_background_lua(lua_State *state) {
  display_release_background(state, lua_touserdata(state, lua_upvalueindex(1)));
  return 0;
}

static int display_restore_background(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  display_region_t *region = luaL_checkudata(state, 1, H2_LUA_DISPLAY_REGION_META);
  if (!job->display_open || region->masked ||
      region->width != job->display_info.width ||
      region->height != job->display_info.height)
    return luaL_error(state, "background must be an opaque full-screen region");
  if (job->display_background != region) {
    lua_pushvalue(state, 1);
    int ref = luaL_ref(state, LUA_REGISTRYINDEX);
    if (!job->display_open || region->width != job->display_info.width ||
        region->height != job->display_info.height) {
      luaL_unref(state, LUA_REGISTRYINDEX, ref);
      return luaL_error(state, "display changed while binding background");
    }
    display_release_background(state, job);
    job->display_background = region;
    job->display_background_ref = ref;
  }
  uint16_t *pixels = display_region_pixels(region);
  uint8_t *damage = display_region_damage(region);
  if (!job->display_background_valid) {
    memcpy(job->framebuffer, pixels, region->pixel_count * sizeof(uint16_t));
    display_dirty_full(job);
  } else {
    int columns = (region->width + 15) / 16;
    int rows = (region->height + 15) / 16;
    for (int ty = 0; ty < rows; ++ty) {
      for (int tx = 0; tx < columns;) {
        if (!damage[(size_t)ty * columns + tx]) { ++tx; continue; }
        int first = tx++;
        while (tx < columns && damage[(size_t)ty * columns + tx]) ++tx;
        int right = tx * 16 < region->width ? tx * 16 : region->width;
        int bottom = (ty + 1) * 16 < region->height ? (ty + 1) * 16 : region->height;
        if (first == 0 && right == region->width) {
          /* Opaque full-width rows are contiguous in both buffers. */
          size_t start = (size_t)ty * 16 * region->width;
          memcpy(job->framebuffer + start, pixels + start,
                 (size_t)region->width * (bottom - ty * 16) * sizeof(uint16_t));
          mark_dirty_rect(job, 0, ty * 16, region->width, bottom - ty * 16);
        } else {
          for (int y = ty * 16; y < bottom; ++y)
            memcpy(job->framebuffer + (size_t)y * region->width + first * 16,
                pixels + (size_t)y * region->width + first * 16,
                (size_t)(right - first * 16) * sizeof(uint16_t));
          mark_dirty_rect(job, first * 16, ty * 16, right - first * 16,
                          bottom - ty * 16);
        }
      }
    }
  }
  memset(damage, 0, display_tile_count(region->width, region->height));
  job->display_background_valid = 1;
  return 0;
}

static int display_begin_frame(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  int clear = 0;
  uint16_t color = 0u;
  if (!lua_isnoneornil(state, 1)) {
    luaL_checktype(state, 1, LUA_TTABLE);
    lua_getfield(state, 1, "clear");
    clear = lua_toboolean(state, -1);
    lua_pop(state, 1);
    if (clear) {
      lua_getfield(state, 1, "color");
      if (!lua_isnil(state, -1))
        color = check_color(state, -1);
      lua_pop(state, 1);
    }
  }
  if (!job->display_open || job->frame_open)
    return luaL_error(state, "invalid begin_frame");
  job->frame_open = 1;
  if (clear) display_clear_pixels(job, color);
  return 0;
}

static void display_option(lua_State *state, const char *key) {
  if (lua_isnoneornil(state, 1)) lua_pushnil(state);
  else { lua_pushstring(state, key); lua_rawget(state, 1); }
}

static int display_boolean_option(lua_State *state, const char *key,
                                   int fallback) {
  display_option(state, key);
  int value = fallback;
  if (!lua_isnil(state, -1)) {
    luaL_checktype(state, -1, LUA_TBOOLEAN);
    value = lua_toboolean(state, -1);
  }
  lua_pop(state, 1);
  return value;
}

static void display_release_presented(lua_State *state, h2_lua_job_t *job) {
  int ref = job->display_presented_ref;
  job->display_presented = NULL;
  job->display_presented_ref = 0;
  job->display_presented_valid = 0;
  if (ref > 0) luaL_unref(state, LUA_REGISTRYINDEX, ref);
}

static void display_enable_retained(lua_State *state, h2_lua_job_t *job) {
  int width = job->display_info.width, height = job->display_info.height;
  if (width > 4096 || height > 4096)
    luaL_error(state, "retained display dimensions exceed 4096");
  size_t count = (size_t)width * height;
  size_t tiles = display_tile_count(width, height);
  size_t bytes = sizeof(display_presented_t) + tiles;
  if (count > (SIZE_MAX - bytes) / sizeof(uint16_t))
    luaL_error(state, "retained display size overflow");
  display_presented_t *frame = lua_newuserdatauv(state,
      bytes + count * sizeof(uint16_t), 0);
  frame->pixel_count = count;
  int ref = luaL_ref(state, LUA_REGISTRYINDEX);
  if (!job->display_open || width != job->display_info.width ||
      height != job->display_info.height) {
    luaL_unref(state, LUA_REGISTRYINDEX, ref);
    luaL_error(state, "display changed while enabling retained mode");
  }
  /* GC may have installed another baseline; detach it before publishing ours. */
  display_release_presented(state, job);
  job->display_presented = frame;
  job->display_presented_ref = ref;
}

static h2_pal_result_t display_submit_rect(h2_lua_job_t *job, int x, int y,
                                           int width, int height,
                                           size_t *pixels, size_t *rects) {
  h2_display_rect_t rect = {x, y, width, height};
  if (job->display_presented != NULL) job->display_presented_valid = 0;
  h2_pal_result_t result = (h2_pal_result_t)h2_pal_display_draw_bitmap(
      job->host->config.runtime->display, &rect,
      job->framebuffer + (size_t)y * job->display_info.width + x,
      (size_t)job->display_info.width * sizeof(uint16_t), H2_DISPLAY_PIXEL_RGB565);
  if (result == H2_PAL_OK) {
    *pixels += (size_t)width * height;
    ++*rects;
    display_presented_t *frame = job->display_presented;
    if (frame != NULL) {
      /* Tentative until present succeeds. Failure forces a complete retry. */
      for (int row = y; row < y + height; ++row) {
        size_t at = (size_t)row * job->display_info.width + x;
        memcpy(frame->pixels + at, job->framebuffer + at,
               (size_t)width * sizeof(uint16_t));
      }
    }
  }
  return result;
}

static int display_tile_changed(h2_lua_job_t *job, display_presented_t *frame,
                                 int tx, int ty) {
  int width = job->display_info.width, height = job->display_info.height;
  int x = tx * 16, y = ty * 16;
  int right = x + 16 < width ? x + 16 : width;
  int bottom = y + 16 < height ? y + 16 : height;
  for (; y < bottom; ++y) {
    size_t at = (size_t)y * width + x;
    if (memcmp(job->framebuffer + at, frame->pixels + at,
               (size_t)(right - x) * sizeof(uint16_t)) != 0) return 1;
  }
  return 0;
}

static h2_pal_result_t display_submit_retained(h2_lua_job_t *job, int bounds,
                                               int gap, size_t *pixels,
                                               size_t *rects) {
  display_presented_t *frame = job->display_presented;
  int width = job->display_info.width, height = job->display_info.height;
  int columns = (width + 15) / 16, rows = (height + 15) / 16;
  uint8_t *changed = (uint8_t *)(frame->pixels + frame->pixel_count);
  memset(changed, 0, (size_t)columns * rows);
  int left = columns, top = rows, right = 0, bottom = 0;
  /* Finish every comparison before touching the backend or baseline. */
  if (job->dirty_valid) {
    for (int ty = job->dirty_min_y / 16; ty <= job->dirty_max_y / 16; ++ty) {
      for (int tx = job->dirty_min_x / 16; tx <= job->dirty_max_x / 16; ++tx) {
        if (!display_tile_changed(job, frame, tx, ty)) continue;
        changed[(size_t)ty * columns + tx] = 1;
        if (tx < left) left = tx;
        if (ty < top) top = ty;
        if (tx + 1 > right) right = tx + 1;
        if (ty + 1 > bottom) bottom = ty + 1;
      }
    }
  }
  if (right == 0) return H2_PAL_OK;
  if (bounds) {
    int r = right * 16 < width ? right * 16 : width;
    int b = bottom * 16 < height ? bottom * 16 : height;
    return display_submit_rect(job, left * 16, top * 16,
                               r - left * 16, b - top * 16, pixels, rects);
  }
  for (int ty = top; ty < bottom; ++ty) {
    for (int tx = left; tx < right; ++tx) {
      if (!changed[(size_t)ty * columns + tx]) continue;
      int end_x = tx + 1;
      for (int x = end_x; x < right && x - end_x <= gap; ++x)
        if (changed[(size_t)ty * columns + x]) end_x = x + 1;
      int end_y = ty + 1;
      for (int y = end_y; y < bottom && y - end_y <= gap; ++y) {
        int any = 0;
        for (int x = tx; x < end_x; ++x) any |= changed[(size_t)y * columns + x];
        if (any) end_y = y + 1;
      }
      for (int y = ty; y < end_y; ++y)
        memset(changed + (size_t)y * columns + tx, 0, (size_t)(end_x - tx));
      int r = end_x * 16 < width ? end_x * 16 : width;
      int b = end_y * 16 < height ? end_y * 16 : height;
      h2_pal_result_t result = display_submit_rect(job, tx * 16, ty * 16,
          r - tx * 16, b - ty * 16, pixels, rects);
      if (result != H2_PAL_OK) return result;
    }
  }
  return H2_PAL_OK;
}

static int display_present(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  if (!lua_isnoneornil(state, 1)) luaL_checktype(state, 1, LUA_TTABLE);
  int retained = display_boolean_option(state, "retained", job->display_presented != NULL);
  int bounds = display_boolean_option(state, "bounds", 0);
  display_option(state, "merge_gap");
  int gap = display_integer(state, -1, 0, 0, 8);
  lua_pop(state, 1);
  if (!job->display_open) return luaL_error(state, "display is not open");
  if (retained && job->display_presented == NULL) display_enable_retained(state, job);
  else if (!retained && job->display_presented != NULL) {
    display_release_presented(state, job);
    display_dirty_full(job);
  }
  size_t pixels = 0, rects = 0;
  h2_pal_result_t result = H2_PAL_OK;
  if (retained && !job->display_presented_valid)
    result = display_submit_rect(job, 0, 0, job->display_info.width,
                                 job->display_info.height, &pixels, &rects);
  else if (retained)
    result = display_submit_retained(job, bounds, gap, &pixels, &rects);
  else if (job->dirty_valid)
    result = display_submit_rect(job, job->dirty_min_x, job->dirty_min_y,
        job->dirty_max_x - job->dirty_min_x + 1,
        job->dirty_max_y - job->dirty_min_y + 1, &pixels, &rects);
  if (result == H2_PAL_OK) {
    result = (h2_pal_result_t)h2_pal_display_present(
        job->host->config.runtime->display);
  }
  if (result != H2_PAL_OK) {
    job->display_presented_valid = 0;
    job->display_background_valid = 0;
    display_dirty_full(job);
    return luaL_error(state, "display present failed: %d", result);
  }
  if (retained) {
    /* Only the whole successful present commits the tentative baseline. */
    job->display_presented_valid = 1;
  }
  job->dirty_valid = 0;
  lua_pushinteger(state, (lua_Integer)pixels);
  lua_pushinteger(state, (lua_Integer)rects);
  return 2;
}

static int display_end_frame(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  if (!job->frame_open) {
    return luaL_error(state, "invalid end_frame");
  }
  job->frame_open = 0;
  return display_present(state);
}

static void display_release(lua_State *state, h2_lua_job_t *job) {
  uint16_t *pixels = job->framebuffer;
  int was_open = job->display_open;
  job->framebuffer = NULL;
  job->display_open = job->frame_open = job->dirty_valid = 0;
  if (state != NULL) {
    lua_rawgetp(state, LUA_REGISTRYINDEX, &s_mesh_stage_key);
    int has_stage_key = !lua_isnil(state, -1);
    lua_pop(state, 1);
    if (has_stage_key) {
      lua_pushboolean(state, 0);
      lua_rawsetp(state, LUA_REGISTRYINDEX, &s_mesh_stage_key);
    }
    display_release_background(state, job);
    display_release_presented(state, job);
    int smooth_ref = job->display_smooth_ref;
    job->display_smooth = NULL;
    job->display_smooth_ref = 0;
    if (smooth_ref > 0) luaL_unref(state, LUA_REGISTRYINDEX, smooth_ref);
  }
  if (was_open && !job->host->config.borrow_display)
    (void)h2_pal_display_close(job->host->config.runtime->display);
  h2_pal_mem_free(job->host->config.runtime->mem, pixels);
}

void h2_lua_job_close_display(h2_lua_job_t *job) {
  job->display_shutting_down = 1;
  display_release(job->vm != NULL ? job->vm->state : NULL, job);
}

static int display_close(lua_State *state) {
  display_release(state, lua_touserdata(state, lua_upvalueindex(1)));
  return 0;
}

int h2_lua_push_display_proxy(lua_State *state, h2_lua_job_t *job) {
  h2_pal_result_t result;
  /* Reserve the registry key before drawing. Replacing its value later does
   * not grow the table, including after release/reacquisition. */
  lua_rawgetp(state, LUA_REGISTRYINDEX, &s_mesh_stage_key);
  int has_stage_key = !lua_isnil(state, -1);
  lua_pop(state, 1);
  if (!has_stage_key) {
    lua_pushboolean(state, 0);
    lua_rawsetp(state, LUA_REGISTRYINDEX, &s_mesh_stage_key);
  }
  result = display_open(job);
  if (result != H2_PAL_OK) {
    lua_pushnil(state);
    lua_pushfstring(state, "display open failed: %d", result);
    return 2;
  }
  lua_createtable(state, 0, 31);
  set_function(state, "stroke_path", display_stroke_path, job);
  set_function(state, "region_from_string", display_region_from_string, job);
  set_function(state, "capture_region", display_capture_region, job);
  set_function(state, "draw_region", display_draw_region, job);
  set_function(state, "restore_background", display_restore_background, job);
  set_function(state, "release_background", display_release_background_lua, job);
  set_function(state, "compile_mesh", display_compile_mesh, job);
  set_function(state, "update_mesh", display_update_mesh, job);
  set_function(state, "draw_mesh", display_draw_mesh, job);
  set_function(state, "draw_pose", display_draw_pose, job);
  set_function(state, "polyline", display_polyline_new, job);
  set_function(state, "compile_line_style", display_line_style, job);
  set_function(state, "draw_polyline", display_draw_polyline, job);
  set_function(state, "fill_polygon", display_fill_polygon, job);
  set_function(state, "fill_ellipse", display_fill_ellipse, job);
  set_function(state, "compile_rects", display_compile_rects, job);
  set_function(state, "compile_palette", display_compile_palette, job);
  set_function(state, "blend_palette", display_blend_palette, job);
  set_function(state, "draw_rects", display_draw_rects, job);
  set_function(state, "compile_commands", display_compile_commands, job);
  set_function(state, "draw_commands", display_draw_commands, job);
  set_function(state, "clear", display_clear, job);
  set_function(state, "fill_rect", display_fill_rect, job);
  set_function(state, "draw_line", display_draw_line, job);
  set_function(state, "fill_circle", display_fill_circle, job);
  set_function(state, "draw_circle", display_draw_circle, job);
  set_function(state, "fill_circle_aa", display_fill_circle_aa, job);
  set_function(state, "fade_to_black", display_fade_to_black, job);
  set_function(state, "fade_rect_to_black", display_fade_rect_to_black, job);
  set_function(state, "fill_round_rect", display_fill_round_rect, job);
  set_function(state, "draw_round_rect", display_draw_round_rect, job);
  set_function(state, "fill_triangle", display_fill_triangle, job);
  set_function(state, "draw_text", display_draw_text, job);
  set_function(state, "draw_text_aligned", display_draw_text_aligned, job);
  set_function(state, "begin_frame", display_begin_frame, job);
  set_function(state, "end_frame", display_end_frame, job);
  set_function(state, "present", display_present, job);
  set_function(state, "deinit", display_close, job);
  lua_pushinteger(state, job->display_info.width);
  lua_setfield(state, -2, "width");
  lua_pushinteger(state, job->display_info.height);
  lua_setfield(state, -2, "height");
  return 1;
}

