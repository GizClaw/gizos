#include "h2_lua_numeric_internal.h"

static double checked(lua_State *s, double v) {
  if (!isfinite(v) || fabs(v) > H2_LUA_NUMERIC_VALUE_LIMIT)
    luaL_error(s, "numeric value outside finite bounds");
  return v;
}
double h2_numeric_number(lua_State *s, int at) {
  luaL_checktype(s, at, LUA_TNUMBER);
  return checked(s, lua_tonumber(s, at));
}
size_t h2_numeric_size(lua_State *s, int at, size_t max) {
  luaL_checktype(s, at, LUA_TNUMBER);
  lua_Integer n = luaL_checkinteger(s, at);
  if (n < 0 || (lua_Unsigned)n > max)
    luaL_error(s, "numeric size out of bounds");
  return (size_t)n;
}
h2_numeric_buffer_t *h2_numeric_check(lua_State *s, int at) {
  return luaL_checkudata(s, at, H2_NUMERIC_META);
}
void h2_numeric_capacity(lua_State *s, h2_numeric_buffer_t *b, size_t n) {
  if (n > b->count)
    luaL_error(s, "numeric buffer too small");
}
static int buffer_new(lua_State *s) {
  size_t n = h2_numeric_size(s, 1, H2_LUA_NUMERIC_COUNT_LIMIT);
  static const char *const kinds[] = {"f64", "f32", NULL};
  int is_f32 = luaL_checkoption(s, 2, "f64", kinds);
  size_t bytes = 2 * n * (is_f32 ? sizeof(float) : sizeof(double));
  h2_numeric_buffer_t *b = lua_newuserdatauv(s, sizeof(*b) + bytes, 0);
  b->count = n;
  b->is_f32 = is_f32;
  if (is_f32) {
    b->data.f32 = (float *)(void *)(b + 1);
    for (size_t i = 0; i < 2 * n; ++i)
      b->data.f32[i] = 0.0f;
  } else {
    b->data.f64 = (double *)(void *)(b + 1);
    for (size_t i = 0; i < 2 * n; ++i)
      b->data.f64[i] = 0.0;
  }
  luaL_setmetatable(s, H2_NUMERIC_META);
  return 1;
}
static int buffer_len(lua_State *s) {
  lua_pushinteger(s, (lua_Integer)h2_numeric_check(s, 1)->count);
  return 1;
}
static size_t index_of(lua_State *s, int at, size_t n) {
  size_t i = h2_numeric_size(s, at, n);
  if (!i)
    luaL_error(s, "indices are one-based");
  return i - 1;
}
static int scalar_clamp(lua_State *s) {
  double x = h2_numeric_number(s, 1), a = h2_numeric_number(s, 2),
         b = h2_numeric_number(s, 3);
  if (a > b)
    return luaL_error(s, "reversed interval");
  lua_pushnumber(s, fmax(a, fmin(b, x)));
  return 1;
}
static int scalar_lerp(lua_State *s) {
  double a = h2_numeric_number(s, 1), b = h2_numeric_number(s, 2),
         t = h2_numeric_number(s, 3);
  lua_pushnumber(s, checked(s, a + (b - a) * t));
  return 1;
}
static int scalar_smoothstep(lua_State *s) {
  double a = h2_numeric_number(s, 1), b = h2_numeric_number(s, 2),
         x = h2_numeric_number(s, 3);
  if (a >= b)
    return luaL_error(s, "empty or reversed interval");
  double t = fmax(0, fmin(1, (x - a) / (b - a)));
  lua_pushnumber(s, t * t * (3 - 2 * t));
  return 1;
}
static int spring(lua_State *s) {
  double x = h2_numeric_number(s, 1), v = h2_numeric_number(s, 2),
         target = h2_numeric_number(s, 3);
  double k = h2_numeric_number(s, 4), c = h2_numeric_number(s, 5),
         a = h2_numeric_number(s, 6), dt = h2_numeric_number(s, 7);
  if (k < 0 || c < 0 || dt < 1e-6 || dt > .1)
    return luaL_error(s, "invalid spring parameters");
  v = checked(s, v + (k * (target - x) - c * v + a) * dt);
  x = checked(s, x + v * dt);
  lua_pushnumber(s, x);
  lua_pushnumber(s, v);
  return 2;
}

#define H2_NUMERIC_F32 1
#include "h2_lua_vmath_kernels.h"
#undef H2_NUMERIC_F32
#define H2_NUMERIC_F32 0
#include "h2_lua_vmath_kernels.h"
#undef H2_NUMERIC_F32

H2_NUMERIC_DISPATCH(buffer_get, 1)
H2_NUMERIC_DISPATCH(buffer_set, 1)
H2_NUMERIC_DISPATCH(buffer_fill, 1)
H2_NUMERIC_DISPATCH(buffer_load, 1)
H2_NUMERIC_DISPATCH(buffer_copy, 1)
H2_NUMERIC_DISPATCH(channel, 1)
H2_NUMERIC_DISPATCH(combine, 1)
H2_NUMERIC_DISPATCH(polynomial, 1)
H2_NUMERIC_DISPATCH(clamp_bulk, 1)
H2_NUMERIC_DISPATCH(product, 1)
H2_NUMERIC_DISPATCH(vector_length, 1)
H2_NUMERIC_DISPATCH(dot, 1)
H2_NUMERIC_DISPATCH(verlet, 1)
H2_NUMERIC_DISPATCH(relax, 1)
H2_NUMERIC_DISPATCH(relax_sweep, 1)
H2_NUMERIC_DISPATCH(damp_edges, 1)
H2_NUMERIC_DISPATCH(map, 1)
H2_NUMERIC_DISPATCH(select_le, 1)
H2_NUMERIC_DISPATCH(take, 1)
H2_NUMERIC_DISPATCH(damp, 1)

int h2_lua_open_vmath(lua_State *s) {
  if (luaL_newmetatable(s, H2_NUMERIC_META)) {
    static const luaL_Reg methods[] = {
        {"get", buffer_get},   {"set", buffer_set},   {"fill", buffer_fill},
        {"load", buffer_load}, {"copy", buffer_copy}, {"__len", buffer_len},
        {NULL, NULL}};
    luaL_setfuncs(s, methods, 0);
    lua_pushvalue(s, -1);
    lua_setfield(s, -2, "__index");
    lua_pushliteral(s, "numeric buffer");
    lua_setfield(s, -2, "__metatable");
  }
  lua_pop(s, 1);
  static const luaL_Reg functions[] = {{"buffer", buffer_new},
                                       {"clamp", scalar_clamp},
                                       {"lerp", scalar_lerp},
                                       {"smoothstep", scalar_smoothstep},
                                       {"spring", spring},
                                       {"combine", combine},
                                       {"polynomial", polynomial},
                                       {"clamp_bulk", clamp_bulk},
                                       {"dot", dot},
                                       {"verlet", verlet},
                                       {"relax", relax},
                                       {"damp", damp},
                                       {"relax_sweep", relax_sweep},
                                       {"damp_edges", damp_edges},
                                       {"map", map},
                                       {"select_le", select_le},
                                       {"take", take},
                                       {NULL, NULL}};
  luaL_newlib(s, functions);
  lua_pushinteger(s, 0);
  lua_pushcclosure(s, product, 1);
  lua_setfield(s, -2, "multiply");
  lua_pushinteger(s, 1);
  lua_pushcclosure(s, product, 1);
  lua_setfield(s, -2, "divide");
  lua_pushinteger(s, 0);
  lua_pushcclosure(s, vector_length, 1);
  lua_setfield(s, -2, "length3");
  lua_pushinteger(s, 1);
  lua_pushcclosure(s, vector_length, 1);
  lua_setfield(s, -2, "normalize3");
  lua_pushinteger(s, 0);
  lua_pushcclosure(s, channel, 1);
  lua_setfield(s, -2, "gather");
  lua_pushinteger(s, 1);
  lua_pushcclosure(s, channel, 1);
  lua_setfield(s, -2, "scatter");
  return 1;
}
