#include "h2_lua_numeric_internal.h"
#include <float.h>

#define ROTATION_META "h2.geometry.rotations"
typedef struct rotation_data {
  size_t n;
  double axis[3], parallel[3], moment[18][3], weight_max, magnitude;
  double *segments, *weights, *dots;
} rotation_data_t;
static double rotation_checked(lua_State *s, double x) {
  if (!isfinite(x) || fabs(x) > H2_LUA_NUMERIC_VALUE_LIMIT)
    luaL_error(s, "rotation value out of bounds");
  return x;
}
static h2_numeric_buffer_t *rotation_buffer(lua_State *s, int at, size_t n) {
  h2_numeric_buffer_t *b = h2_numeric_check(s, at);
  if (b->is_f32)
    luaL_error(s, "prepared rotations require f64 buffers");
  h2_numeric_capacity(s, b, n);
  return b;
}
static void rotation_cross(const double *a, const double *b, double *out) {
  out[0] = a[1] * b[2] - a[2] * b[1];
  out[1] = a[2] * b[0] - a[0] * b[2];
  out[2] = a[0] * b[1] - a[1] * b[0];
}
static int rotations_new(lua_State *s) {
  size_t n = h2_numeric_size(s, 4, 256);
  h2_numeric_buffer_t *segments = rotation_buffer(s, 1, 3 * n),
                      *weights = rotation_buffer(s, 2, n),
                      *axis = rotation_buffer(s, 3, 3);
  if (segments == weights || segments == axis || weights == axis)
    return luaL_error(s, "aliased rotation inputs");
  double magnitude = 0;
  for (size_t j = 0; j < 3; ++j)
    magnitude += fabs(rotation_checked(s, axis->data.f64[j]));
  if (magnitude == 0)
    return luaL_error(s, "zero rotation axis");
  rotation_data_t *r =
      lua_newuserdatauv(s, sizeof(*r) + 5 * n * sizeof(double), 0);
  memset(r, 0, sizeof(*r));
  r->n = n;
  luaL_setmetatable(s, ROTATION_META);
  r->segments = (double *)(r + 1);
  r->weights = r->segments + 3 * n;
  r->dots = r->weights + n;
  memcpy(r->axis, axis->data.f64, sizeof(r->axis));
  for (size_t i = 0; i < n; ++i) {
    r->weights[i] = rotation_checked(s, weights->data.f64[i]);
    r->weight_max = fmax(r->weight_max, fabs(r->weights[i]));
    double *v = r->segments + 3 * i;
    for (size_t j = 0; j < 3; ++j)
      v[j] = rotation_checked(s, segments->data.f64[3 * i + j]);
    r->dots[i] = r->axis[0] * v[0] + r->axis[1] * v[1] + r->axis[2] * v[2];
    for (size_t j = 0; j < 3; ++j) {
      double along = r->axis[j] * r->dots[i], perpendicular = v[j] - along,
             power = 1;
      r->parallel[j] += along;
      r->magnitude += fabs(perpendicular);
      /* Only build moments in the bounded weight domain. Other inputs use
       * the full reference loop without overflowing unnecessary powers. */
      if (fabs(r->weights[i]) <= 1)
        for (size_t k = 0; k < 18; ++k) {
          r->moment[k][j] += perpendicular * power;
          power *= r->weights[i];
        }
    }
  }
  return 1;
}
static int rotations_evaluate(lua_State *s) {
  rotation_data_t *r = luaL_checkudata(s, 1, ROTATION_META);
  double angle = h2_numeric_number(s, 3), bend = h2_numeric_number(s, 4),
         yaw = h2_numeric_number(s, 5);
  double position[3];
  for (int j = 0; j < 3; ++j)
    position[j] = h2_numeric_number(s, 6 + j);
  luaL_checktype(s, 9, LUA_TBOOLEAN);
  luaL_checktype(s, 10, LUA_TBOOLEAN);
  int full = lua_toboolean(s, 9), shared = lua_toboolean(s, 10);
  if (shared && bend != 0)
    return luaL_error(s, "shared rotation requires zero bend");
  size_t count = full ? 3 * (r->n + 1) : 3;
  h2_numeric_buffer_t *out = rotation_buffer(s, 2, count);
  double *scratch = out->data.f64 + out->count;
  double ca = cos(angle), sa = sin(angle), cy = cos(yaw), sy = sin(yaw);
  double axis_sum = fabs(r->axis[0]) + fabs(r->axis[1]) + fabs(r->axis[2]);
  /* Degree-17 remainder <= x^18/18!, plus a conservative accumulated
   * floating error bound. Arbitrary weights/axes/lengths are not a fast domain.
   * This is an absolute 1e-9 endpoint bound before the supplied translation. */
  double error = r->magnitude * (1 + axis_sum) *
                 (7.38e-13 + (double)(r->n + 18) * DBL_EPSILON * 32);
  int fast = !full && !shared && r->weight_max <= 1 && fabs(bend) <= 1.6 &&
             fabs(angle) <= 16 && axis_sum <= 2 && error <= 1e-9;
  if (full)
    memcpy(scratch, position, sizeof(position));
  if (fast) {
    double cv[3] = {0}, sv[3] = {0}, power = 1, cc[3], cs[3], delta[3];
    for (size_t k = 0; k < 18; ++k) {
      for (size_t j = 0; j < 3; ++j)
        if (k % 2 == 0)
          cv[j] += power * r->moment[k][j];
        else
          sv[j] += power * r->moment[k][j];
      power *= bend / (double)(k + 1);
      if (k % 2 == 1)
        power = -power;
    }
    rotation_cross(r->axis, cv, cc);
    rotation_cross(r->axis, sv, cs);
    for (size_t j = 0; j < 3; ++j)
      delta[j] =
          r->parallel[j] + ca * cv[j] + sa * sv[j] + sa * cc[j] - ca * cs[j];
    if (yaw != 0) {
      double x = delta[0] * cy + delta[2] * sy;
      delta[2] = -delta[0] * sy + delta[2] * cy;
      delta[0] = x;
    }
    for (size_t j = 0; j < 3; ++j)
      position[j] += delta[j];
  } else {
    double matrix[3][3] = {{0}};
    if (shared)
      for (size_t j = 0; j < 3; ++j) {
        double v[3] = {0}, cross[3], rotated[3];
        v[j] = 1;
        rotation_cross(r->axis, v, cross);
        for (size_t k = 0; k < 3; ++k)
          rotated[k] =
              v[k] * ca + cross[k] * sa + r->axis[k] * r->axis[j] * (1 - ca);
        matrix[0][j] = rotated[0] * cy + rotated[2] * sy;
        matrix[1][j] = rotated[1];
        matrix[2][j] = -rotated[0] * sy + rotated[2] * cy;
      }
    for (size_t i = 0; i < r->n; ++i) {
      const double *v = r->segments + 3 * i;
      double delta[3], cross[3];
      if (shared)
        for (size_t j = 0; j < 3; ++j)
          delta[j] =
              matrix[j][0] * v[0] + matrix[j][1] * v[1] + matrix[j][2] * v[2];
      else {
        double a = angle - bend * r->weights[i];
        double c = r->weights[i] == 0 ? ca : cos(a),
               sn = r->weights[i] == 0 ? sa : sin(a);
        rotation_cross(r->axis, v, cross);
        for (size_t j = 0; j < 3; ++j)
          delta[j] =
              v[j] * c + cross[j] * sn + r->axis[j] * r->dots[i] * (1 - c);
        if (yaw != 0) {
          double x = delta[0] * cy + delta[2] * sy;
          delta[2] = -delta[0] * sy + delta[2] * cy;
          delta[0] = x;
        }
      }
      for (size_t j = 0; j < 3; ++j) {
        position[j] += delta[j];
        if (full)
          scratch[3 * (i + 1) + j] = position[j];
      }
    }
  }
  if (!full)
    memcpy(scratch, position, sizeof(position));
  for (size_t i = 0; i < count; ++i)
    rotation_checked(s, scratch[i]);
  memcpy(out->data.f64, scratch, count * sizeof(double));
  lua_pushboolean(s, fast);
  return 1;
}
void h2_geometry_prepared_register(lua_State *s) {
  if (luaL_newmetatable(s, ROTATION_META)) {
    lua_pushcfunction(s, rotations_evaluate);
    lua_setfield(s, -2, "evaluate");
    lua_pushvalue(s, -1);
    lua_setfield(s, -2, "__index");
    lua_pushliteral(s, "prepared rotations");
    lua_setfield(s, -2, "__metatable");
  }
  lua_pop(s, 1);
  lua_pushcfunction(s, rotations_new);
  lua_setfield(s, -2, "rotations");
}
