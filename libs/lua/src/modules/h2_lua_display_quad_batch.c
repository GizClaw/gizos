#include "h2_lua_display_quad_batch.h"
#include "h2_lua_display_internal.h"

#include <math.h>
#include <string.h>

#define H2_LUA_QUAD_BATCH_META "h2.display.quad_batch"
#define H2_LUA_QUAD_BATCH_LIMIT 256u

typedef struct display_quad_strip {
  double left, right, top, bottom;
  unsigned color_index;
  int patch;
} display_quad_strip_t;

typedef struct display_quad_batch {
  size_t count, color_count;
  display_quad_strip_t strips[];
} display_quad_batch_t;

static double display_quad_fraction(lua_State *state, int index) {
  lua_rawgeti(state, -1, index);
  double value = luaL_checknumber(state, -1);
  lua_pop(state, 1);
  if (!isfinite(value) || value < 0 || value > 1)
    luaL_error(state, "quad batch fraction must be in [0, 1]");
  return value;
}

static int display_compile_quad_batch(lua_State *state) {
  size_t count = h2_lua_display_dense_count(state, H2_LUA_QUAD_BATCH_LIMIT);
  display_quad_batch_t *batch = lua_newuserdatauv(
      state, sizeof(*batch) + count * sizeof(*batch->strips), 0);
  batch->count = count;
  batch->color_count = 0;
  for (size_t i = 0; i < count; ++i) {
    lua_rawgeti(state, 1, (lua_Integer)i + 1);
    luaL_checktype(state, -1, LUA_TTABLE);
    size_t fields = lua_rawlen(state, -1);
    if (fields != 3 && fields != 5)
      return luaL_error(state, "quad batch record needs 3 or 5 fields");
    display_quad_strip_t *strip = &batch->strips[i];
    strip->left = display_quad_fraction(state, 1);
    strip->right = display_quad_fraction(state, 2);
    lua_rawgeti(state, -1, 3);
    lua_Integer color = luaL_checkinteger(state, -1);
    lua_pop(state, 1);
    if (color < 1 || color > H2_LUA_QUAD_BATCH_LIMIT)
      return luaL_error(state, "quad batch color index out of range");
    strip->color_index = (unsigned)color - 1u;
    if ((size_t)color > batch->color_count) batch->color_count = (size_t)color;
    strip->patch = fields == 5;
    strip->top = strip->patch ? display_quad_fraction(state, 4) : 0;
    strip->bottom = strip->patch ? display_quad_fraction(state, 5) : 1;
    if (strip->left > strip->right || strip->top > strip->bottom)
      return luaL_error(state, "reversed quad batch interval");
    lua_pop(state, 1);
  }
  luaL_newmetatable(state, H2_LUA_QUAD_BATCH_META);
  lua_setmetatable(state, -2);
  return 1;
}

/* Lua performs separate multiply/add instructions. Force that rounding even
 * on hosts whose compiler otherwise contracts across C statements to FMA. */
static double display_quad_lerp(double origin, double delta, double t) {
  volatile double step = delta * t;
  return origin + step;
}

static int display_draw_quad_batch(lua_State *state) {
  h2_lua_job_t *job = lua_touserdata(state, lua_upvalueindex(1));
  const display_quad_batch_t *batch =
      luaL_checkudata(state, 1, H2_LUA_QUAD_BATCH_META);
  if (lua_gettop(state) < 10 || lua_gettop(state) > 12)
    return luaL_error(state,
        "quad batch needs colors, eight coordinates and optional row clip");
  double corners[8];
  for (int i = 0; i < 8; ++i)
    corners[i] = h2_lua_display_check_geometry_number(state, i + 3);
  uint16_t decoded[H2_LUA_QUAD_BATCH_LIMIT];
  const uint16_t *colors;
  const display_palette_t *palette =
      luaL_testudata(state, 2, H2_LUA_PALETTE_META);
  if (palette != NULL) {
    if (palette->count < batch->color_count)
      return luaL_error(state, "quad batch palette too short");
    colors = palette->colors;
  } else {
    luaL_checktype(state, 2, LUA_TTABLE);
    size_t count = lua_rawlen(state, 2);
    if (count < batch->color_count || count > H2_LUA_QUAD_BATCH_LIMIT)
      return luaL_error(state, "quad batch color count out of range");
    for (size_t i = 0; i < count; ++i) {
      lua_rawgeti(state, 2, (lua_Integer)i + 1);
      decoded[i] = h2_lua_display_check_color(state, -1);
      lua_pop(state, 1);
    }
    colors = decoded;
  }
  /* Color getters can close/reopen Display or recursively draw. All scratch
   * is call-local; no Lua callback or allocation follows this acquisition check. */
  int top, bottom;
  h2_lua_display_check_clip(state, job, 11, 12, &top, &bottom);
  if (top == bottom)
    return 0;
  double edges[8] = {0};
  const display_quad_strip_t *previous = NULL;
  for (size_t i = 0; i < batch->count; ++i) {
    const display_quad_strip_t *strip = &batch->strips[i];
    if (previous == NULL || strip->patch != previous->patch ||
        strip->top != previous->top || strip->bottom != previous->bottom) {
      double patch[8];
      memcpy(patch, corners, sizeof(patch));
      if (strip->patch) {
        for (int axis = 0; axis < 2; ++axis) {
          double ad = corners[6 + axis] - corners[axis];
          double bc = corners[4 + axis] - corners[2 + axis];
          patch[axis] = display_quad_lerp(corners[axis], ad, strip->top);
          patch[2 + axis] = display_quad_lerp(corners[2 + axis], bc, strip->top);
          patch[4 + axis] = display_quad_lerp(corners[2 + axis], bc, strip->bottom);
          patch[6 + axis] = display_quad_lerp(corners[axis], ad, strip->bottom);
        }
      }
      for (int axis = 0; axis < 2; ++axis) {
        edges[axis] = patch[axis];
        edges[2 + axis] = patch[2 + axis] - patch[axis];
        edges[4 + axis] = patch[6 + axis];
        edges[6 + axis] = patch[4 + axis] - patch[6 + axis];
      }
    }
    double xy[2][4];
    for (int axis = 0; axis < 2; ++axis) {
      xy[axis][0] = display_quad_lerp(edges[axis], edges[2 + axis], strip->left);
      xy[axis][1] = display_quad_lerp(edges[axis], edges[2 + axis], strip->right);
      xy[axis][2] = display_quad_lerp(edges[4 + axis], edges[6 + axis], strip->right);
      xy[axis][3] = display_quad_lerp(edges[4 + axis], edges[6 + axis], strip->left);
    }
    h2_lua_display_raster_quad(job, xy[0], xy[1], colors[strip->color_index],
                               top, bottom);
    previous = strip;
  }
  return 0;
}

static void display_quad_batch_open(lua_State *state, h2_lua_job_t *job) {
  lua_pushlightuserdata(state, job);
  lua_pushcclosure(state, display_compile_quad_batch, 1);
  lua_setfield(state, -2, "compile_quad_batch");
  lua_pushlightuserdata(state, job);
  lua_pushcclosure(state, display_draw_quad_batch, 1);
  lua_setfield(state, -2, "draw_quad_batch");
}

h2_pal_result_t h2_lua_display_quad_batch_enable(h2_lua_host_t *host) {
  if (host == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (h2_atomic_load(&host->started) != 0 ||
      h2_atomic_load(&host->stopping) != 0 ||
      host->display_quad_batch_open != NULL)
    return H2_PAL_ERR_INVALID_STATE;
  host->display_quad_batch_open = display_quad_batch_open;
  return H2_PAL_OK;
}
