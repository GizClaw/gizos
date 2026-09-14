/* Intentionally included twice: one algorithm, two element types.
 * NUM_* is configured by h2_lua_numeric_typed.h. */
#include "h2_lua_numeric_typed.h"

static int NUM_NAME(affine)(lua_State *s) {
  h2_numeric_buffer_t *d = NUM_NAME(buffer_check)(s, 1),
                      *p = NUM_NAME(buffer_check)(s, 2),
                      *m = NUM_NAME(buffer_check)(s, 3);
  size_t n = h2_numeric_size(s, 4, H2_LUA_NUMERIC_COUNT_LIMIT / 3);
  size_t dim = (size_t)lua_tointeger(s, lua_upvalueindex(1));
  h2_numeric_capacity(s, d, n * dim);
  h2_numeric_capacity(s, p, n * dim);
  h2_numeric_capacity(s, m, dim * (dim + 1));
  for (size_t i = 0; i < n; ++i)
    for (size_t j = 0; j < dim; ++j) {
      NUM_REAL v = NUM_DATA(m)[j * (dim + 1) + dim];
      for (size_t k = 0; k < dim; ++k)
        v += NUM_DATA(m)[j * (dim + 1) + k] * NUM_DATA(p)[i * dim + k];
      NUM_DATA(d)[d->count + i * dim + j] = v;
    }
  NUM_NAME(commit)(s, d, n * dim);
  return 0;
}
static int NUM_NAME(displace)(lua_State *s) {
  h2_numeric_buffer_t *d = NUM_NAME(buffer_check)(s, 1),
                      *p = NUM_NAME(buffer_check)(s, 2),
                      *w = NUM_NAME(buffer_check)(s, 3);
  NUM_REAL x = NUM_NAME(number)(s, 4), y = NUM_NAME(number)(s, 5),
           z = NUM_NAME(number)(s, 6);
  size_t n = h2_numeric_size(s, 7, H2_LUA_NUMERIC_COUNT_LIMIT / 3);
  h2_numeric_capacity(s, d, 3 * n);
  h2_numeric_capacity(s, p, 3 * n);
  h2_numeric_capacity(s, w, n);
  const NUM_REAL delta[] = {x, y, z};
  for (size_t i = 0; i < n; ++i)
    for (size_t j = 0; j < 3; ++j)
      NUM_DATA(d)
      [d->count + 3 * i + j] =
          NUM_DATA(p)[3 * i + j] + NUM_DATA(w)[i] * delta[j];
  NUM_NAME(commit)(s, d, 3 * n);
  return 0;
}
static int NUM_NAME(rotate)(lua_State *s) {
  h2_numeric_buffer_t *d = NUM_NAME(buffer_check)(s, 1),
                      *p = NUM_NAME(buffer_check)(s, 2),
                      *w = NUM_NAME(buffer_check)(s, 3);
  NUM_REAL axis[] = {NUM_NAME(number)(s, 4), NUM_NAME(number)(s, 5),
                     NUM_NAME(number)(s, 6)};
  NUM_REAL angle = NUM_NAME(number)(s, 7);
  size_t n = h2_numeric_size(s, 8, H2_LUA_NUMERIC_COUNT_LIMIT / 3);
  h2_numeric_capacity(s, d, 3 * n);
  h2_numeric_capacity(s, p, 3 * n);
  h2_numeric_capacity(s, w, n);
  NUM_REAL len =
      NUM_MATH(sqrt)(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
  if (len < NUM_C(1e-12))
    return luaL_error(s, "zero rotation axis");
  for (size_t j = 0; j < 3; ++j)
    axis[j] /= len;
  for (size_t i = 0; i < n; ++i) {
    const NUM_REAL *v = NUM_DATA(p) + 3 * i;
    NUM_REAL a = angle * NUM_DATA(w)[i];
    if (NUM_MATH(fabs)(a) > NUM_C(1000000.0))
      return luaL_error(s, "rotation angle out of bounds");
    NUM_REAL c = NUM_MATH(cos)(a), sn = NUM_MATH(sin)(a),
             dot = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2];
    for (size_t j = 0; j < 3; ++j)
      NUM_DATA(d)
      [d->count + 3 * i + j] = v[j] * c +
                               (axis[(j + 1) % 3] * v[(j + 2) % 3] -
                                axis[(j + 2) % 3] * v[(j + 1) % 3]) *
                                   sn +
                               axis[j] * dot * (1 - c);
  }
  NUM_NAME(commit)(s, d, 3 * n);
  return 0;
}
static int NUM_NAME(prefix)(lua_State *s) {
  h2_numeric_buffer_t *d = NUM_NAME(buffer_check)(s, 1),
                      *p = NUM_NAME(buffer_check)(s, 2);
  NUM_REAL v[] = {NUM_NAME(number)(s, 3), NUM_NAME(number)(s, 4),
                  NUM_NAME(number)(s, 5)};
  size_t n = h2_numeric_size(s, 6, H2_LUA_NUMERIC_COUNT_LIMIT / 3 - 1);
  h2_numeric_capacity(s, d, 3 * (n + 1));
  h2_numeric_capacity(s, p, 3 * n);
  memcpy(NUM_DATA(d) + d->count, v, sizeof(v));
  for (size_t i = 0; i < n; ++i)
    for (size_t j = 0; j < 3; ++j) {
      v[j] += NUM_DATA(p)[3 * i + j];
      NUM_DATA(d)[d->count + 3 * (i + 1) + j] = v[j];
    }
  NUM_NAME(commit)(s, d, 3 * (n + 1));
  return 0;
}
static const NUM_REAL *NUM_NAME(camera)(lua_State *s, int at) {
  h2_numeric_buffer_t *c = NUM_NAME(buffer_check)(s, at);
  h2_numeric_capacity(s, c, 5);
  if (NUM_DATA(c)[4] < NUM_C(.001))
    return (luaL_error(s, "near plane must be >= .001"),
            (const NUM_REAL *)NULL);
  return NUM_DATA(c); /* fx,fy,cx,cy,near; camera-space +Z forward. */
}
static int NUM_NAME(project)(lua_State *s) {
  h2_numeric_buffer_t *d = NUM_NAME(buffer_check)(s, 1),
                      *mask = NUM_NAME(buffer_check)(s, 2),
                      *p = NUM_NAME(buffer_check)(s, 3);
  const NUM_REAL *c = NUM_NAME(camera)(s, 4);
  size_t n = h2_numeric_size(s, 5, H2_LUA_NUMERIC_COUNT_LIMIT / 3);
  distinct(s, d, mask);
  h2_numeric_capacity(s, d, 2 * n);
  h2_numeric_capacity(s, mask, n);
  h2_numeric_capacity(s, p, 3 * n);
  for (size_t i = 0; i < n; ++i) {
    const NUM_REAL *v = NUM_DATA(p) + 3 * i;
    int visible = v[2] >= c[4];
    NUM_DATA(mask)[mask->count + i] = visible ? NUM_C(1.0) : NUM_C(0.0);
    NUM_DATA(d)[d->count + 2 * i] = visible ? c[2] + c[0] * v[0] / v[2] : 0;
    NUM_DATA(d)[d->count + 2 * i + 1] = visible ? c[3] + c[1] * v[1] / v[2] : 0;
  }
  NUM_NAME(commit)(s, d, 2 * n);
  NUM_NAME(commit)(s, mask, n);
  return 0;
}
static int NUM_NAME(project_segments)(lua_State *s) {
  h2_numeric_buffer_t *d = NUM_NAME(buffer_check)(s, 1),
                      *ids = NUM_NAME(buffer_check)(s, 2),
                      *p = NUM_NAME(buffer_check)(s, 3);
  const NUM_REAL *c = NUM_NAME(camera)(s, 4);
  size_t n = h2_numeric_size(s, 5, 4096);
  distinct(s, d, ids);
  h2_numeric_capacity(s, d, 4 * n);
  h2_numeric_capacity(s, ids, n);
  h2_numeric_capacity(s, p, 6 * n);
  size_t used = 0;
  for (size_t i = 0; i < n; ++i) {
    NUM_REAL v[6];
    memcpy(v, NUM_DATA(p) + 6 * i, sizeof(v));
    if (v[2] < c[4] && v[5] < c[4])
      continue;
    if (v[2] < c[4] || v[5] < c[4]) {
      NUM_REAL t = (c[4] - v[2]) / (v[5] - v[2]);
      size_t k = v[2] < c[4] ? 0 : 3;
      NUM_REAL x = v[0] + t * (v[3] - v[0]), y = v[1] + t * (v[4] - v[1]);
      v[k] = x;
      v[k + 1] = y;
      v[k + 2] = c[4];
    }
    for (size_t j = 0; j < 2; ++j) {
      NUM_DATA(d)
      [d->count + 4 * used + 2 * j] = c[2] + c[0] * v[3 * j] / v[3 * j + 2];
      NUM_DATA(d)
      [d->count + 4 * used + 2 * j + 1] =
          c[3] + c[1] * v[3 * j + 1] / v[3 * j + 2];
    }
    NUM_DATA(ids)[ids->count + used] = (NUM_REAL)i + 1;
    ++used;
  }
  NUM_NAME(commit)(s, d, 4 * used);
  NUM_NAME(commit)(s, ids, used);
  lua_pushinteger(s, (lua_Integer)used);
  return 1;
}
static int NUM_NAME(segments)(lua_State *s) {
  h2_numeric_buffer_t *d = NUM_NAME(buffer_check)(s, 1),
                      *p = NUM_NAME(buffer_check)(s, 2);
  size_t n = h2_numeric_size(s, 3, 4097);
  if (!n)
    return luaL_error(s, "polyline requires a point");
  h2_numeric_capacity(s, p, 3 * n);
  h2_numeric_capacity(s, d, 6 * (n - 1));
  for (size_t i = 0; i + 1 < n; ++i)
    memcpy(NUM_DATA(d) + d->count + 6 * i, NUM_DATA(p) + 3 * i,
           6 * sizeof(NUM_REAL));
  NUM_NAME(commit)(s, d, 6 * (n - 1));
  lua_pushinteger(s, (lua_Integer)n - 1);
  return 1;
}
static int NUM_NAME(split)(lua_State *s) {
  h2_numeric_buffer_t *d = NUM_NAME(buffer_check)(s, 1),
                      *tags = NUM_NAME(buffer_check)(s, 2),
                      *p = NUM_NAME(buffer_check)(s, 3);
  size_t axis = h2_numeric_size(s, 4, 3);
  NUM_REAL offset = NUM_NAME(number)(s, 5);
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
    const NUM_REAL *v = NUM_DATA(p) + 6 * i;
    NUM_REAL a = v[axis] - offset, b = v[axis + 3] - offset;
    int crossing = (a < 0 && b > 0) || (a > 0 && b < 0);
    NUM_REAL cut[3];
    if (crossing) {
      NUM_REAL t = -a / (b - a);
      for (size_t j = 0; j < 3; ++j)
        cut[j] = v[j] + t * (v[j + 3] - v[j]);
      cut[axis] = offset;
    }
    for (int part = 0; part < (crossing ? 2 : 1); ++part) {
      NUM_REAL *out = NUM_DATA(d) + d->count + 6 * used;
      memcpy(out, part ? cut : v, 3 * sizeof(NUM_REAL));
      memcpy(out + 3, crossing && !part ? cut : v + 3, 3 * sizeof(NUM_REAL));
      NUM_REAL side = crossing ? (part ? b : a) : (a != 0 ? a : b);
      NUM_DATA(tags)[tags->count + 2 * used] =
          side < 0 ? -NUM_C(1.0) : side > 0 ? NUM_C(1.0) : NUM_C(0.0);
      NUM_DATA(tags)[tags->count + 2 * used + 1] = (NUM_REAL)i + 1;
      ++used;
    }
  }
  NUM_NAME(commit)(s, d, 6 * used);
  NUM_NAME(commit)(s, tags, 2 * used);
  lua_pushinteger(s, (lua_Integer)used);
  return 1;
}
static int NUM_NAME(mesh_update)(lua_State *s) {
  mesh_writer_t *w = luaL_checkudata(s, 1, MESH_META);
  h2_numeric_buffer_t *xy = NUM_NAME(buffer_check)(s, 2),
                      *top = NUM_NAME(buffer_check)(s, 3);
  size_t nv = h2_numeric_size(s, 4, w->vertices),
         np = h2_numeric_size(s, 5, w->primitives);
  h2_numeric_capacity(s, xy, nv * 2);
  h2_numeric_capacity(s, top, np * 4);
  lua_getiuservalue(s, 1, 1);
  h2_lua_display_vertex_t *vertices = lua_touserdata(s, -1);
  lua_getiuservalue(s, 1, 2);
  h2_lua_display_primitive_t *primitives = lua_touserdata(s, -1);
  for (size_t i = 0; i < nv; ++i) {
    vertices[i].x = (double)NUM_DATA(xy)[2 * i];
    vertices[i].y = (double)NUM_DATA(xy)[2 * i + 1];
  }
  for (size_t i = 0; i < np; ++i) {
    const NUM_REAL *t = NUM_DATA(top) + 4 * i;
    if ((t[0] != 0 && t[0] != 1) || t[1] < 1 || t[1] > nv ||
        NUM_MATH(floor)(t[1]) != t[1] || t[2] < 2 || t[2] > 128 ||
        NUM_MATH(floor)(t[2]) != t[2] || t[1] - 1 + t[2] > nv || t[3] < 0 ||
        t[3] > 65535 || NUM_MATH(floor)(t[3]) != t[3])
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
