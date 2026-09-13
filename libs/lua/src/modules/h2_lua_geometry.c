#include "h2_lua_display.h"
#include "h2_lua_numeric_internal.h"

static void distinct(lua_State *s, h2_numeric_buffer_t *a,
                     h2_numeric_buffer_t *b) {
  if (a == b)
    luaL_error(s, "output buffers must be distinct");
}
static int affine(lua_State *s) {
  h2_numeric_buffer_t *d = h2_numeric_check(s, 1), *p = h2_numeric_check(s, 2),
                      *m = h2_numeric_check(s, 3);
  size_t n = h2_numeric_size(s, 4, H2_LUA_NUMERIC_COUNT_LIMIT / 3);
  size_t dim = (size_t)lua_tointeger(s, lua_upvalueindex(1));
  h2_numeric_capacity(s, d, n * dim);
  h2_numeric_capacity(s, p, n * dim);
  h2_numeric_capacity(s, m, dim * (dim + 1));
  for (size_t i = 0; i < n; ++i)
    for (size_t j = 0; j < dim; ++j) {
      double v = m->data[j * (dim + 1) + dim];
      for (size_t k = 0; k < dim; ++k)
        v += m->data[j * (dim + 1) + k] * p->data[i * dim + k];
      d->data[d->count + i * dim + j] = v;
    }
  h2_numeric_commit(s, d, n * dim);
  return 0;
}
static int displace(lua_State *s) {
  h2_numeric_buffer_t *d = h2_numeric_check(s, 1), *p = h2_numeric_check(s, 2),
                      *w = h2_numeric_check(s, 3);
  double x = h2_numeric_number(s, 4), y = h2_numeric_number(s, 5),
         z = h2_numeric_number(s, 6);
  size_t n = h2_numeric_size(s, 7, H2_LUA_NUMERIC_COUNT_LIMIT / 3);
  h2_numeric_capacity(s, d, 3 * n);
  h2_numeric_capacity(s, p, 3 * n);
  h2_numeric_capacity(s, w, n);
  const double delta[] = {x, y, z};
  for (size_t i = 0; i < n; ++i)
    for (size_t j = 0; j < 3; ++j)
      d->data[d->count + 3 * i + j] =
          p->data[3 * i + j] + w->data[i] * delta[j];
  h2_numeric_commit(s, d, 3 * n);
  return 0;
}
static int rotate(lua_State *s) {
  h2_numeric_buffer_t *d = h2_numeric_check(s, 1), *p = h2_numeric_check(s, 2),
                      *w = h2_numeric_check(s, 3);
  double axis[] = {h2_numeric_number(s, 4), h2_numeric_number(s, 5),
                   h2_numeric_number(s, 6)};
  double angle = h2_numeric_number(s, 7);
  size_t n = h2_numeric_size(s, 8, H2_LUA_NUMERIC_COUNT_LIMIT / 3);
  h2_numeric_capacity(s, d, 3 * n);
  h2_numeric_capacity(s, p, 3 * n);
  h2_numeric_capacity(s, w, n);
  double len = sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
  if (len < 1e-12)
    return luaL_error(s, "zero rotation axis");
  for (size_t j = 0; j < 3; ++j)
    axis[j] /= len;
  for (size_t i = 0; i < n; ++i) {
    const double *v = p->data + 3 * i;
    double a = angle * w->data[i];
    if (fabs(a) > H2_LUA_NUMERIC_VALUE_LIMIT)
      return luaL_error(s, "rotation angle out of bounds");
    double c = cos(a), sn = sin(a),
           dot = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2];
    for (size_t j = 0; j < 3; ++j)
      d->data[d->count + 3 * i + j] = v[j] * c +
                                      (axis[(j + 1) % 3] * v[(j + 2) % 3] -
                                       axis[(j + 2) % 3] * v[(j + 1) % 3]) *
                                          sn +
                                      axis[j] * dot * (1 - c);
  }
  h2_numeric_commit(s, d, 3 * n);
  return 0;
}
static int prefix(lua_State *s) {
  h2_numeric_buffer_t *d = h2_numeric_check(s, 1), *p = h2_numeric_check(s, 2);
  double v[] = {h2_numeric_number(s, 3), h2_numeric_number(s, 4),
                h2_numeric_number(s, 5)};
  size_t n = h2_numeric_size(s, 6, H2_LUA_NUMERIC_COUNT_LIMIT / 3 - 1);
  h2_numeric_capacity(s, d, 3 * (n + 1));
  h2_numeric_capacity(s, p, 3 * n);
  memcpy(d->data + d->count, v, sizeof(v));
  for (size_t i = 0; i < n; ++i)
    for (size_t j = 0; j < 3; ++j) {
      v[j] += p->data[3 * i + j];
      d->data[d->count + 3 * (i + 1) + j] = v[j];
    }
  h2_numeric_commit(s, d, 3 * (n + 1));
  return 0;
}
static const double *camera(lua_State *s, int at) {
  h2_numeric_buffer_t *c = h2_numeric_check(s, at);
  h2_numeric_capacity(s, c, 5);
  if (c->data[4] < .001)
    return (luaL_error(s, "near plane must be >= .001"), (const double *)NULL);
  return c->data; /* fx,fy,cx,cy,near; camera-space +Z forward. */
}
static int project(lua_State *s) {
  h2_numeric_buffer_t *d = h2_numeric_check(s, 1),
                      *mask = h2_numeric_check(s, 2),
                      *p = h2_numeric_check(s, 3);
  const double *c = camera(s, 4);
  size_t n = h2_numeric_size(s, 5, H2_LUA_NUMERIC_COUNT_LIMIT / 3);
  distinct(s, d, mask);
  h2_numeric_capacity(s, d, 2 * n);
  h2_numeric_capacity(s, mask, n);
  h2_numeric_capacity(s, p, 3 * n);
  for (size_t i = 0; i < n; ++i) {
    const double *v = p->data + 3 * i;
    int visible = v[2] >= c[4];
    mask->data[mask->count + i] = visible;
    d->data[d->count + 2 * i] = visible ? c[2] + c[0] * v[0] / v[2] : 0;
    d->data[d->count + 2 * i + 1] = visible ? c[3] + c[1] * v[1] / v[2] : 0;
  }
  h2_numeric_commit(s, d, 2 * n);
  h2_numeric_commit(s, mask, n);
  return 0;
}
static int project_segments(lua_State *s) {
  h2_numeric_buffer_t *d = h2_numeric_check(s, 1),
                      *ids = h2_numeric_check(s, 2),
                      *p = h2_numeric_check(s, 3);
  const double *c = camera(s, 4);
  size_t n = h2_numeric_size(s, 5, 4096);
  distinct(s, d, ids);
  h2_numeric_capacity(s, d, 4 * n);
  h2_numeric_capacity(s, ids, n);
  h2_numeric_capacity(s, p, 6 * n);
  size_t used = 0;
  for (size_t i = 0; i < n; ++i) {
    double v[6];
    memcpy(v, p->data + 6 * i, sizeof(v));
    if (v[2] < c[4] && v[5] < c[4])
      continue;
    if (v[2] < c[4] || v[5] < c[4]) {
      double t = (c[4] - v[2]) / (v[5] - v[2]);
      size_t k = v[2] < c[4] ? 0 : 3;
      double x = v[0] + t * (v[3] - v[0]), y = v[1] + t * (v[4] - v[1]);
      v[k] = x;
      v[k + 1] = y;
      v[k + 2] = c[4];
    }
    for (size_t j = 0; j < 2; ++j) {
      d->data[d->count + 4 * used + 2 * j] =
          c[2] + c[0] * v[3 * j] / v[3 * j + 2];
      d->data[d->count + 4 * used + 2 * j + 1] =
          c[3] + c[1] * v[3 * j + 1] / v[3 * j + 2];
    }
    ids->data[ids->count + used] = (double)i + 1;
    ++used;
  }
  h2_numeric_commit(s, d, 4 * used);
  h2_numeric_commit(s, ids, used);
  lua_pushinteger(s, (lua_Integer)used);
  return 1;
}
static int segments(lua_State *s) {
  h2_numeric_buffer_t *d = h2_numeric_check(s, 1), *p = h2_numeric_check(s, 2);
  size_t n = h2_numeric_size(s, 3, 4097);
  if (!n)
    return luaL_error(s, "polyline requires a point");
  h2_numeric_capacity(s, p, 3 * n);
  h2_numeric_capacity(s, d, 6 * (n - 1));
  for (size_t i = 0; i + 1 < n; ++i)
    memcpy(d->data + d->count + 6 * i, p->data + 3 * i, 6 * sizeof(double));
  h2_numeric_commit(s, d, 6 * (n - 1));
  lua_pushinteger(s, (lua_Integer)n - 1);
  return 1;
}
static int split(lua_State *s) {
  h2_numeric_buffer_t *d = h2_numeric_check(s, 1),
                      *tags = h2_numeric_check(s, 2),
                      *p = h2_numeric_check(s, 3);
  size_t axis = h2_numeric_size(s, 4, 3);
  double offset = h2_numeric_number(s, 5);
  size_t n = h2_numeric_size(s, 6, 2048);
  if (!axis)
    return luaL_error(s, "axis must be 1..3");
  --axis;
  distinct(s, d, tags);
  h2_numeric_capacity(s, d, 12 * n);
  h2_numeric_capacity(s, tags, 4 * n);
  h2_numeric_capacity(s, p, 6 * n);
  size_t used = 0;
  for (size_t i = 0; i < n; ++i) {
    const double *v = p->data + 6 * i;
    double a = v[axis] - offset, b = v[axis + 3] - offset;
    int crossing = (a < 0 && b > 0) || (a > 0 && b < 0);
    double cut[3];
    if (crossing) {
      double t = -a / (b - a);
      for (size_t j = 0; j < 3; ++j)
        cut[j] = v[j] + t * (v[j + 3] - v[j]);
      cut[axis] = offset;
    }
    for (int part = 0; part < (crossing ? 2 : 1); ++part) {
      double *out = d->data + d->count + 6 * used;
      memcpy(out, part ? cut : v, 3 * sizeof(double));
      memcpy(out + 3, crossing && !part ? cut : v + 3, 3 * sizeof(double));
      double side = crossing ? (part ? b : a) : (a != 0 ? a : b);
      tags->data[tags->count + 2 * used] = side < 0 ? -1 : side > 0 ? 1 : 0;
      tags->data[tags->count + 2 * used + 1] = (double)i + 1;
      ++used;
    }
  }
  h2_numeric_commit(s, d, 6 * used);
  h2_numeric_commit(s, tags, 2 * used);
  lua_pushinteger(s, (lua_Integer)used);
  return 1;
}
#define MESH_META "h2.geometry.mesh_writer"
typedef struct mesh_writer {
  size_t vertices, primitives;
} mesh_writer_t;
static int mesh_new(lua_State *s) {
  size_t nv = h2_numeric_size(s, 1, H2_LUA_NUMERIC_COUNT_LIMIT / 2),
         np = h2_numeric_size(s, 2, H2_LUA_DISPLAY_PRIMITIVE_LIMIT);
  mesh_writer_t *w = lua_newuserdatauv(s, sizeof(*w), 3);
  w->vertices = nv;
  w->primitives = np;
  luaL_setmetatable(s, MESH_META);
  int handle = lua_gettop(s);
  lua_newuserdatauv(s, nv * sizeof(h2_lua_display_vertex_t), 0);
  lua_setiuservalue(s, handle, 1);
  lua_newuserdatauv(s, np * sizeof(h2_lua_display_primitive_t), 0);
  lua_setiuservalue(s, handle, 2);
  h2_lua_display_mesh_config_t config = {.vertex_capacity = nv,
                                         .primitive_capacity = np};
  h2_pal_result_t rc = h2_lua_display_mesh_push(s, &config);
  if (rc != H2_PAL_OK)
    return luaL_error(s, "mesh allocation failed: %d", rc);
  lua_pushvalue(s, -1);
  lua_setiuservalue(s, handle, 3);
  return 2;
}
static int mesh_update(lua_State *s) {
  mesh_writer_t *w = luaL_checkudata(s, 1, MESH_META);
  h2_numeric_buffer_t *xy = h2_numeric_check(s, 2),
                      *top = h2_numeric_check(s, 3);
  size_t nv = h2_numeric_size(s, 4, w->vertices),
         np = h2_numeric_size(s, 5, w->primitives);
  h2_numeric_capacity(s, xy, nv * 2);
  h2_numeric_capacity(s, top, np * 4);
  lua_getiuservalue(s, 1, 1);
  h2_lua_display_vertex_t *vertices = lua_touserdata(s, -1);
  lua_getiuservalue(s, 1, 2);
  h2_lua_display_primitive_t *primitives = lua_touserdata(s, -1);
  for (size_t i = 0; i < nv; ++i) {
    vertices[i].x = xy->data[2 * i];
    vertices[i].y = xy->data[2 * i + 1];
  }
  for (size_t i = 0; i < np; ++i) {
    const double *t = top->data + 4 * i;
    if ((t[0] != 0 && t[0] != 1) || t[1] < 1 || t[1] > nv ||
        floor(t[1]) != t[1] || t[2] < 2 || t[2] > 128 || floor(t[2]) != t[2] ||
        t[1] - 1 + t[2] > nv || t[3] < 0 || t[3] > 65535 || floor(t[3]) != t[3])
      return luaL_error(s, "invalid mesh topology");
    primitives[i] = (h2_lua_display_primitive_t){
        .kind = (h2_lua_display_primitive_kind_t)t[0],
        .first = (size_t)t[1] - 1,
        .count = (size_t)t[2],
        .color = (uint16_t)t[3]};
  }
  lua_getiuservalue(s, 1, 3);
  h2_lua_display_mesh_data_t data = {.vertices = vertices,
                                     .vertex_count = nv,
                                     .primitives = primitives,
                                     .primitive_count = np};
  h2_pal_result_t rc = h2_lua_display_mesh_update(s, -1, &data);
  if (rc != H2_PAL_OK)
    return luaL_error(s, "mesh update failed: %d", rc);
  return 1;
}
int h2_lua_open_geometry(lua_State *s) {
  luaL_newmetatable(s, MESH_META);
  lua_pushliteral(s, "mesh writer");
  lua_setfield(s, -2, "__metatable");
  lua_pop(s, 1);
  static const luaL_Reg functions[] = {{"displace3", displace},
                                       {"rotate3", rotate},
                                       {"prefix3", prefix},
                                       {"project_points", project},
                                       {"project_segments", project_segments},
                                       {"segments", segments},
                                       {"split_segments", split},
                                       {"mesh", mesh_new},
                                       {"update_mesh", mesh_update},
                                       {NULL, NULL}};
  luaL_newlib(s, functions);
  lua_pushinteger(s, 2);
  lua_pushcclosure(s, affine, 1);
  lua_setfield(s, -2, "affine2");
  lua_pushinteger(s, 3);
  lua_pushcclosure(s, affine, 1);
  lua_setfield(s, -2, "affine3");
  return 1;
}
