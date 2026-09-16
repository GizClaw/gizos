#include "h2_lua_geometry_batches_internal.h"

static double batch_value(lua_State *s, double value) {
  if (!isfinite(value) || fabs(value) > H2_LUA_NUMERIC_VALUE_LIMIT)
    luaL_error(s, "geometry value out of bounds");
  return value;
}
static const double *batch_buffer(lua_State *s, int at, size_t count) {
  h2_numeric_buffer_t *b = h2_numeric_check(s, at);
  if (b->is_f32)
    luaL_error(s, "prepared geometry requires f64 buffers");
  h2_numeric_capacity(s, b, count);
  return b->data.f64;
}
static int batch_new(lua_State *s) {
  size_t n = h2_numeric_size(s, 6, 32768), np = h2_numeric_size(s, 7, 4096);
  const double *xy = batch_buffer(s, 1, 2 * n),
               *top = batch_buffer(s, 2, 3 * np);
  const double *w0 = lua_isnil(s, 3) ? NULL : batch_buffer(s, 3, n);
  const double *w1 = lua_isnil(s, 4) ? NULL : batch_buffer(s, 4, n);
  const double *direction = batch_buffer(s, 5, 4);
  h2_geometry_batch_t *g = lua_newuserdatauv(s, sizeof(*g), 2);
  memset(g, 0, sizeof(*g));
  g->n = n;
  g->parts = np;
  luaL_setmetatable(s, H2_GEOMETRY_BATCH_META);
  int at = lua_gettop(s);
  g->base = lua_newuserdatauv(s, 4 * n * sizeof(double), 0);
  lua_setiuservalue(s, at, 1);
  g->topology = lua_newuserdatauv(s, np * sizeof(h2_geometry_part_t), 0);
  lua_setiuservalue(s, at, 2);
  for (size_t j = 0; j < 4; ++j)
    g->direction[j] = batch_value(s, direction[j]);
  for (size_t i = 0; i < n; ++i) {
    g->base[4 * i] = batch_value(s, xy[2 * i]);
    g->base[4 * i + 1] = batch_value(s, xy[2 * i + 1]);
    g->base[4 * i + 2] = w0 ? batch_value(s, w0[i]) : 0;
    g->base[4 * i + 3] = w1 ? batch_value(s, w1[i]) : 0;
  }
  for (size_t i = 0; i < np; ++i) {
    double kind = top[3 * i], first = top[3 * i + 1], count = top[3 * i + 2];
    if ((kind != 0 && kind != 1) || first < 1 || first > (double)n ||
        floor(first) != first || count < (kind ? 2 : 3) ||
        count > (kind ? 2 : 128) || floor(count) != count ||
        count > (double)n - first + 1)
      return luaL_error(s, "invalid geometry topology");
    g->topology[i] =
        (h2_geometry_part_t){(size_t)first - 1, (size_t)count, (int)kind};
  }
  return 1;
}
static int pose_new(lua_State *s) {
  h2_geometry_batch_t *g = luaL_checkudata(s, 1, H2_GEOMETRY_BATCH_META);
  h2_geometry_pose_t *p =
      lua_newuserdatauv(s, sizeof(*p) + 8 * g->n * sizeof(double), 1);
  memset(p, 0, sizeof(*p));
  p->geometry = g;
  p->positions = (double *)(p + 1);
  p->staged = p->positions + 3 * g->n;
  p->screen = p->staged + 3 * g->n;
  luaL_setmetatable(s, H2_GEOMETRY_POSE_META);
  lua_pushvalue(s, 1);
  lua_setiuservalue(s, -2, 1);
  return 1;
}
static int pose_evaluate(lua_State *s) {
  h2_geometry_pose_t *p = luaL_checkudata(s, 1, H2_GEOMETRY_POSE_META);
  double inputs[14] = {0};
  for (int i = 0; i < 7; ++i)
    inputs[i] = h2_numeric_number(s, i + 2);
  int projected = !lua_isnoneornil(s, 9);
  if (projected) {
    const double *projection = batch_buffer(s, 9, 7);
    for (size_t i = 0; i < 7; ++i)
      inputs[7 + i] = batch_value(s, projection[i]);
    if (inputs[11] == 0)
      return luaL_error(s, "zero perspective divisor");
  }
  if (p->valid && p->projected == projected &&
      memcmp(p->inputs, inputs, sizeof(inputs)) == 0) {
    lua_pushboolean(s, 1);
    return 1;
  }
  const h2_geometry_batch_t *g = p->geometry;
  double a0 = inputs[0], a1 = inputs[1], tx = inputs[2], ty = inputs[3];
  double ca = inputs[4], sa = inputs[5], scale = inputs[6];
  for (size_t i = 0; i < g->n; ++i) {
    const double *v = g->base + 4 * i;
    double x = v[0] + (v[2] * a0) * g->direction[0],
           y = v[1] + (v[2] * a0) * g->direction[1];
    x = x + (v[3] * a1) * g->direction[2];
    y = y + (v[3] * a1) * g->direction[3];
    double u = tx + (x * ca - y * sa) * scale,
           w = ty + (x * sa + y * ca) * scale;
    double xx = u, yy = w, r = 1;
    if (projected) {
      double depth = inputs[7] * u + inputs[8] * w,
             lateral = inputs[9] * u + inputs[10] * w;
      double denominator = 1 + depth / inputs[11];
      if (!isfinite(denominator) || denominator == 0)
        return luaL_error(s, "invalid perspective denominator");
      r = 1 / denominator;
      xx = lateral * r;
      yy = inputs[13] - (depth * inputs[12]) * r;
    }
    p->staged[3 * i] = batch_value(s, xx);
    p->staged[3 * i + 1] = batch_value(s, yy);
    if (!isfinite(r) || fabs(r) > 1000)
      return luaL_error(s, "perspective result out of bounds");
    p->staged[3 * i + 2] = r;
  }
  memcpy(p->positions, p->staged, 3 * g->n * sizeof(double));
  memcpy(p->inputs, inputs, sizeof(inputs));
  p->projected = projected;
  p->valid = 1;
  ++p->generation;
  lua_pushboolean(s, 0);
  return 1;
}
static int pose_copy(lua_State *s) {
  h2_geometry_pose_t *p = luaL_checkudata(s, 1, H2_GEOMETRY_POSE_META);
  if (!p->valid)
    return luaL_error(s, "pose has not been evaluated");
  h2_numeric_buffer_t *b = h2_numeric_check(s, 2);
  if (b->is_f32)
    return luaL_error(s, "pose copy requires f64");
  h2_numeric_capacity(s, b, 3 * p->geometry->n);
  memcpy(b->data.f64, p->positions, 3 * p->geometry->n * sizeof(double));
  lua_pushinteger(s, (lua_Integer)p->generation);
  return 1;
}
void h2_geometry_batches_register(lua_State *s) {
  luaL_newmetatable(s, H2_GEOMETRY_BATCH_META);
  lua_pushliteral(s, "immutable geometry");
  lua_setfield(s, -2, "__metatable");
  lua_pop(s, 1);
  if (luaL_newmetatable(s, H2_GEOMETRY_POSE_META)) {
    lua_pushcfunction(s, pose_evaluate);
    lua_setfield(s, -2, "evaluate");
    lua_pushcfunction(s, pose_copy);
    lua_setfield(s, -2, "copy");
    lua_pushvalue(s, -1);
    lua_setfield(s, -2, "__index");
    lua_pushliteral(s, "evaluated geometry pose");
    lua_setfield(s, -2, "__metatable");
  }
  lua_pop(s, 1);
  lua_pushcfunction(s, batch_new);
  lua_setfield(s, -2, "batch");
  lua_pushcfunction(s, pose_new);
  lua_setfield(s, -2, "pose");
}
