#include "h2_lua_skeleton2d.h"
#include "h2_lua_display.h"
#include "h2_lua_numeric_internal.h"
#include "h2_skeleton2d.h"
#include <limits.h>
#include <stddef.h>
#define DEF "h2.skeleton2d.definition"
#define ACTOR "h2.skeleton2d.actor"
#define WRITER "h2.skeleton2d.writer"
typedef union handle {
  struct {
    void *ptr;
    size_t bytes;
    const void *definition;
    h2_skeleton2d_local_t *locals;
    h2_skeleton2d_part_state_t *parts;
    size_t nb, np;
  } v;
  max_align_t align;
} handle_t;
typedef struct resource {
  size_t nv, np;
  h2_lua_display_vertex_t *vertices;
  h2_lua_display_primitive_t *primitives;
} resource_t;
typedef struct writer {
  const void *definition;
  size_t nr, nparts, vc, pc;
  resource_t *resources;
  h2_lua_display_vertex_t *vertices;
  h2_lua_display_primitive_t *primitives;
  double *bounds, *scratch_bounds;
  size_t bytes;
  h2_skeleton2d_draw_item_t *cached_items;
  size_t cached_count;
} writer_t;
static void check(lua_State *s, h2_pal_result_t rc, const char *operation) {
  if (rc)
    luaL_error(s, "skeleton2d.%s: PAL error %d", operation, rc);
}
static size_t count(lua_State *s, int at, size_t max) {
  luaL_checktype(s, at, LUA_TTABLE);
  size_t n = lua_rawlen(s, at);
  if (n > max)
    luaL_error(s, "skeleton2d: count exceeds limit");
  return n;
}
static void field(lua_State *s, int at, const char *key) {
  at = lua_absindex(s, at);
  lua_pushstring(s, key);
  lua_rawget(s, at);
}
static lua_Integer integer(lua_State *s, int at, lua_Integer low,
                           lua_Integer high) {
  if (!lua_isinteger(s, at))
    luaL_error(s, "skeleton2d: expected integer");
  lua_Integer v = lua_tointeger(s, at);
  if (v < low || v > high)
    luaL_error(s, "skeleton2d: integer out of range");
  return v;
}
static double value(lua_State *s, int at) {
  if (lua_type(s, at) != LUA_TNUMBER)
    luaL_error(s, "skeleton2d: expected number");
  return h2_numeric_number(s, at);
}
static double rownum(lua_State *s, int at, int j) {
  lua_rawgeti(s, at, j);
  double v = value(s, -1);
  lua_pop(s, 1);
  return v;
}
static lua_Integer rowint(lua_State *s, int at, int j, lua_Integer low,
                          lua_Integer high) {
  lua_rawgeti(s, at, j);
  lua_Integer v = integer(s, -1, low, high);
  lua_pop(s, 1);
  return v;
}
static h2_skeleton2d_transform_t local(lua_State *s, int at, int j) {
  at = lua_absindex(s, at);
  return (h2_skeleton2d_transform_t){rownum(s, at, j), rownum(s, at, j + 1),
                                     rownum(s, at, j + 2), rownum(s, at, j + 3),
                                     rownum(s, at, j + 4)};
}
static int boolean_field(lua_State *s, int at, const char *key) {
  field(s, at, key);
  luaL_checktype(s, -1, LUA_TBOOLEAN);
  int v = lua_toboolean(s, -1);
  lua_pop(s, 1);
  return v;
}
static lua_Integer intfield(lua_State *s, int at, const char *key,
                            lua_Integer lo, lua_Integer hi) {
  field(s, at, key);
  lua_Integer v = integer(s, -1, lo, hi);
  lua_pop(s, 1);
  return v;
}
static void *array(lua_State *s, size_t n, size_t width) {
  void *p = lua_newuserdatauv(s, n * width, 0);
  memset(p, 0, n * width);
  return p;
}
static handle_t *handle(lua_State *s, int at, const char *meta) {
  return luaL_checkudata(s, at, meta);
}
static int compile(lua_State *s) {
  luaL_checktype(s, 1, LUA_TTABLE);
  if (intfield(s, 1, "schema_version", 0, LUA_MAXINTEGER) != 1)
    check(s, H2_PAL_ERR_UNSUPPORTED, "compile");
  field(s, 1, "bones");
  int bt = lua_gettop(s);
  size_t nb = count(s, bt, 128);
  field(s, 1, "parts");
  int pt = lua_gettop(s);
  size_t np = count(s, pt, 256);
  field(s, 1, "clips");
  int ct = lua_gettop(s);
  size_t nc = count(s, ct, 32);
  h2_skeleton2d_bone_t *bones = array(s, nb, sizeof(*bones));
  h2_skeleton2d_part_t *parts = array(s, np, sizeof(*parts));
  h2_skeleton2d_clip_t *clips = array(s, nc, sizeof(*clips));
  for (size_t i = 0; i < nb; i++) {
    lua_rawgeti(s, bt, i + 1);
    int at = lua_gettop(s);
    if (count(s, at, 6) != 6)
      luaL_error(s, "skeleton2d: bone row width");
    size_t p = (size_t)rowint(s, at, 1, 0, 128);
    bones[i] = (h2_skeleton2d_bone_t){p ? p - 1 : SIZE_MAX, local(s, at, 2)};
    lua_pop(s, 1);
  }
  for (size_t i = 0; i < np; i++) {
    lua_rawgeti(s, pt, i + 1);
    int at = lua_gettop(s);
    if (count(s, at, 9) != 9)
      luaL_error(s, "skeleton2d: part row width");
    parts[i] = (h2_skeleton2d_part_t){
        (size_t)rowint(s, at, 1, 1, 128) - 1, local(s, at, 5),
        (uint32_t)rowint(s, at, 2, 1, 256),
        (int32_t)rowint(s, at, 3, -1000000, 1000000),
        (int)rowint(s, at, 4, 0, 1)};
    lua_pop(s, 1);
  }
  size_t totalkeys = 0;
  for (size_t i = 0; i < nc; i++) {
    if (!lua_checkstack(s, 12))
      luaL_error(s, "skeleton2d: stack exhausted");
    lua_rawgeti(s, ct, i + 1);
    int cl = lua_gettop(s);
    luaL_checktype(s, cl, LUA_TTABLE);
    clips[i].duration_us = intfield(s, cl, "duration_us", 1, 3600000000LL);
    field(s, cl, "tracks");
    int tt = lua_gettop(s);
    size_t nt = count(s, tt, 640);
    h2_skeleton2d_track_t *tracks = array(s, nt, sizeof(*tracks));
    clips[i].tracks = tracks;
    clips[i].track_count = nt;
    /* Retain allocation objects on the stack until definition copy completes.
     */
    for (size_t j = 0; j < nt; j++) {
      lua_rawgeti(s, tt, j + 1);
      int at = lua_gettop(s);
      luaL_checktype(s, at, LUA_TTABLE);
      tracks[j].bone = (size_t)intfield(s, at, "bone", 1, 128) - 1;
      tracks[j].channel = (unsigned)intfield(s, at, "channel", 1, 5) - 1;
      tracks[j].linear = boolean_field(s, at, "linear");
      tracks[j].shortest = boolean_field(s, at, "shortest");
      field(s, at, "keys");
      int kt = lua_gettop(s);
      size_t nk = count(s, kt, 256);
      totalkeys += nk;
      if (totalkeys > 16384)
        luaL_error(s, "skeleton2d: key limit");
      h2_skeleton2d_key_t *keys = array(s, nk, sizeof(*keys));
      tracks[j].keys = keys;
      tracks[j].key_count = nk;
      for (size_t k = 0; k < nk; k++) {
        lua_rawgeti(s, kt, k + 1);
        int row = lua_gettop(s);
        if (count(s, row, 2) != 2)
          luaL_error(s, "skeleton2d: key width");
        keys[k] = (h2_skeleton2d_key_t){rowint(s, row, 1, 0, 3600000000LL),
                                        rownum(s, row, 2)};
        lua_pop(s, 1);
      }
      /* Keep the key userdata rooted on the stack, discard source tables. */
      lua_remove(s, kt);
      lua_remove(s, at);
      if (!lua_checkstack(s, 8))
        luaL_error(s, "skeleton2d: stack exhausted");
    }
    lua_remove(s, tt);
    lua_remove(s, cl);
  }
  h2_skeleton2d_config_t cfg = {bones, nb, parts, np, clips, nc};
  size_t bytes;
  check(s, h2_skeleton2d_definition_size(&cfg, &bytes), "compile");
  handle_t *h = lua_newuserdatauv(s, sizeof(*h) + bytes, 0);
  memset(h, 0, sizeof(*h));
  h->v.bytes = sizeof(*h) + bytes;
  h2_skeleton2d_definition_t *d;
  check(s, h2_skeleton2d_definition_init(h + 1, bytes, &cfg, &d), "compile");
  h->v.ptr = d;
  luaL_setmetatable(s, DEF);
  return 1;
}
static int instance_new(lua_State *s) {
  handle_t *d = handle(s, 1, DEF);
  size_t bytes;
  check(s, h2_skeleton2d_instance_size(d->v.ptr, &bytes), "instance");
  size_t nb = h2_skeleton2d_bone_count(d->v.ptr),
         np = h2_skeleton2d_part_count(d->v.ptr);
  size_t total = sizeof(handle_t) + bytes + nb * sizeof(h2_skeleton2d_local_t) +
                 np * sizeof(h2_skeleton2d_part_state_t);
  handle_t *a = lua_newuserdatauv(s, total, 1);
  memset(a, 0, sizeof(*a));
  h2_skeleton2d_t *actor;
  check(s, h2_skeleton2d_instance_init(a + 1, bytes, d->v.ptr, &actor),
        "instance");
  a->v.ptr = actor;
  a->v.definition = d->v.ptr;
  a->v.bytes = total;
  a->v.nb = nb;
  a->v.np = np;
  a->v.locals = (void *)((unsigned char *)(a + 1) + bytes);
  a->v.parts = (void *)(a->v.locals + nb);
  lua_pushvalue(s, 1);
  lua_setiuservalue(s, -2, 1);
  luaL_setmetatable(s, ACTOR);
  return 1;
}
static int bytes(lua_State *s) {
  handle_t *h = luaL_testudata(s, 1, DEF);
  if (!h)
    h = handle(s, 1, ACTOR);
  lua_pushinteger(s, h->v.bytes);
  return 1;
}
static int sample(lua_State *s) {
  handle_t *a = handle(s, 1, ACTOR);
  static const char *const loops[] = {"clamp", "repeat", NULL},
                           *const slots[] = {"current", "a", "b", NULL};
  size_t clip = (size_t)integer(s, 2, 1, 32) - 1;
  int64_t time = integer(s, 3, LUA_MININTEGER, LUA_MAXINTEGER);
  int loop = luaL_checkoption(s, 4, NULL, loops),
      slot = luaL_checkoption(s, 5, "current", slots);
  check(s, h2_skeleton2d_sample(a->v.ptr, clip, time, loop, slot), "sample");
  return 0;
}
static int blend(lua_State *s) {
  check(s, h2_skeleton2d_blend(handle(s, 1, ACTOR)->v.ptr, value(s, 2)),
        "blend");
  return 0;
}
static size_t buffer_id(lua_State *s, double v, size_t max) {
  if (!isfinite(v) || v < 1 || v > max || floor(v) != v)
    luaL_error(s, "skeleton2d: invalid buffer ID");
  return (size_t)v - 1;
}
static int set_local(lua_State *s) {
  handle_t *a = handle(s, 1, ACTOR);
  h2_numeric_buffer_t *b = h2_numeric_check(s, 2);
  if (b->count % 6 || b->count / 6 > a->v.nb)
    luaL_error(s, "skeleton2d: local buffer width");
  h2_skeleton2d_local_t *rows = a->v.locals;
  size_t n = b->count / 6;
  for (size_t i = 0; i < n; i++) {
    const double *v = b->data + 6 * i;
    rows[i] = (h2_skeleton2d_local_t){buffer_id(s, v[0], 128),
                                      {v[1], v[2], v[3], v[4], v[5]}};
  }
  check(s, h2_skeleton2d_set_local(a->v.ptr, rows, n), "set_local");
  return 0;
}
static int set_parts(lua_State *s) {
  handle_t *a = handle(s, 1, ACTOR);
  h2_numeric_buffer_t *b = h2_numeric_check(s, 2);
  if (b->count % 4 || b->count / 4 > a->v.np)
    luaL_error(s, "skeleton2d: part buffer width");
  h2_skeleton2d_part_state_t *rows = a->v.parts;
  size_t n = b->count / 4;
  for (size_t i = 0; i < n; i++) {
    const double *v = b->data + 4 * i;
    if (!isfinite(v[2]) || fabs(v[2]) > 1e6 || floor(v[2]) != v[2] ||
        (v[3] != 0 && v[3] != 1))
      luaL_error(s, "skeleton2d: invalid part state");
    rows[i] = (h2_skeleton2d_part_state_t){
        buffer_id(s, v[0], 256), (uint32_t)buffer_id(s, v[1], 256) + 1,
        (int32_t)v[2], (int)v[3]};
  }
  check(s, h2_skeleton2d_set_parts(a->v.ptr, rows, n), "set_parts");
  return 0;
}
static int evaluate(lua_State *s) {
  handle_t *a = handle(s, 1, ACTOR);
  if (count(s, 2, 6) != 6)
    luaL_error(s, "skeleton2d: matrix width");
  double m[6];
  for (int i = 0; i < 6; i++)
    m[i] = rownum(s, 2, i + 1);
  check(s, h2_skeleton2d_evaluate(a->v.ptr, m), "evaluate");
  return 0;
}
static int copy_matrices(lua_State *s) {
  h2_skeleton2d_view_t v;
  check(s, h2_skeleton2d_view(handle(s, 1, ACTOR)->v.ptr, &v), "copy_matrices");
  h2_numeric_buffer_t *b = h2_numeric_check(s, 2);
  h2_numeric_capacity(s, b, v.bone_count * 6);
  memcpy(b->data + b->count, v.matrices, v.bone_count * 6 * sizeof(double));
  h2_numeric_commit(s, b, v.bone_count * 6);
  lua_pushinteger(s, v.bone_count);
  return 1;
}
static int copy_items(lua_State *s) {
  h2_skeleton2d_view_t v;
  check(s, h2_skeleton2d_view(handle(s, 1, ACTOR)->v.ptr, &v),
        "copy_draw_items");
  h2_numeric_buffer_t *b = h2_numeric_check(s, 2);
  h2_numeric_capacity(s, b, v.item_count * 9);
  for (size_t i = 0; i < v.item_count; i++) {
    double *p = b->data + b->count + i * 9;
    p[0] = v.items[i].part + 1;
    p[1] = v.items[i].resource;
    p[2] = v.items[i].layer;
    memcpy(p + 3, v.items[i].matrix, 6 * sizeof(double));
  }
  h2_numeric_commit(s, b, v.item_count * 9);
  lua_pushinteger(s, v.item_count);
  return 1;
}
static void keep_array(lua_State *s, int roots, size_t i, size_t n,
                       size_t width, void **out) {
  *out = array(s, n, width);
  lua_rawseti(s, roots, i);
}
static int mesh_new(lua_State *s) {
  handle_t *d = handle(s, 1, DEF);
  size_t nr = count(s, 2, 256);
  luaL_checktype(s, 3, LUA_TTABLE);
  size_t vc = intfield(s, 3, "vertices", 0, 65536),
         pc = intfield(s, 3, "primitives", 0, 4096);
  writer_t *w = lua_newuserdatauv(s, sizeof(*w), 3);
  memset(w, 0, sizeof(*w));
  int wi = lua_gettop(s);
  w->definition = d->v.ptr;
  w->nr = nr;
  w->nparts = h2_skeleton2d_part_count(d->v.ptr);
  w->vc = vc;
  w->pc = pc;
  w->bytes = sizeof(*w);
  luaL_setmetatable(s, WRITER);
  lua_pushvalue(s, 1);
  lua_setiuservalue(s, wi, 1);
  lua_newtable(s);
  int roots = lua_gettop(s);
  size_t ri = 1;
  void *p;
  keep_array(s, roots, ri++, nr, sizeof(resource_t), &p);
  w->resources = p;
  w->bytes += nr * sizeof(resource_t);
  keep_array(s, roots, ri++, vc, sizeof(*w->vertices), &p);
  w->vertices = p;
  keep_array(s, roots, ri++, pc, sizeof(*w->primitives), &p);
  w->primitives = p;
  w->bytes += vc * sizeof(*w->vertices) + pc * sizeof(*w->primitives);
  keep_array(s, roots, ri++, w->nparts * 5, sizeof(double), &p);
  w->bounds = p;
  keep_array(s, roots, ri++, w->nparts * 5, sizeof(double), &p);
  w->scratch_bounds = p;
  w->bytes += w->nparts * 10 * sizeof(double);
  keep_array(s, roots, ri++, w->nparts, sizeof(*w->cached_items), &p);
  w->cached_items = p;
  w->cached_count = SIZE_MAX;
  w->bytes += w->nparts * sizeof(*w->cached_items);
  size_t totalv = 0, totalp = 0;
  for (size_t i = 0; i < nr; i++) {
    lua_rawgeti(s, 2, i + 1);
    int rt = lua_gettop(s);
    luaL_checktype(s, rt, LUA_TTABLE);
    field(s, rt, "vertices");
    int vt = lua_gettop(s);
    size_t nv = count(s, vt, 65536);
    field(s, rt, "primitives");
    int pt = lua_gettop(s);
    size_t np = count(s, pt, 4096);
    totalv += nv;
    totalp += np;
    if (totalv > 65536 || totalp > 4096)
      luaL_error(s, "skeleton2d: resource capacity");
    resource_t *r = &w->resources[i];
    r->nv = nv;
    r->np = np;
    keep_array(s, roots, ri++, nv, sizeof(*r->vertices), &p);
    r->vertices = p;
    keep_array(s, roots, ri++, np, sizeof(*r->primitives), &p);
    r->primitives = p;
    w->bytes += nv * sizeof(*r->vertices) + np * sizeof(*r->primitives);
    for (size_t j = 0; j < nv; j++) {
      lua_rawgeti(s, vt, j + 1);
      int at = lua_gettop(s);
      if (count(s, at, 2) != 2)
        luaL_error(s, "skeleton2d: vertex width");
      r->vertices[j] =
          (h2_lua_display_vertex_t){rownum(s, at, 1), rownum(s, at, 2)};
      lua_pop(s, 1);
    }
    for (size_t j = 0; j < np; j++) {
      lua_rawgeti(s, pt, j + 1);
      int at = lua_gettop(s);
      if (count(s, at, 4) != 4)
        luaL_error(s, "skeleton2d: primitive width");
      int kind = (int)rowint(s, at, 1, 0, 1);
      size_t first = (size_t)rowint(s, at, 2, 1, (lua_Integer)nv) - 1,
             n = (size_t)rowint(s, at, 3, kind ? 2 : 3, kind ? 2 : 128);
      if (n > nv - first)
        luaL_error(s, "skeleton2d: primitive range");
      r->primitives[j] = (h2_lua_display_primitive_t){
          kind, first, n, (uint16_t)rowint(s, at, 4, 0, 65535)};
      lua_pop(s, 1);
    }
    lua_pop(s, 3);
  }
  lua_setiuservalue(s, wi, 2);
  h2_lua_display_mesh_config_t cfg = {vc, pc, {NULL, 0, NULL, 0}};
  check(s, h2_lua_display_mesh_push(s, &cfg), "mesh");
  lua_pushvalue(s, -1);
  lua_setiuservalue(s, wi, 3);
  return 2;
}
static int update_mesh(lua_State *s) {
  writer_t *w = luaL_checkudata(s, 1, WRITER);
  handle_t *a = handle(s, 2, ACTOR);
  if (a->v.definition != w->definition)
    luaL_error(s, "skeleton2d: definition mismatch");
  h2_skeleton2d_view_t view;
  check(s, h2_skeleton2d_view(a->v.ptr, &view), "update_mesh");
  int unchanged = w->cached_count == view.item_count;
  for (size_t i = 0; unchanged && i < view.item_count; i++) {
    const h2_skeleton2d_draw_item_t *x = &w->cached_items[i],
                                    *y = &view.items[i];
    unchanged = x->part == y->part && x->resource == y->resource &&
                x->layer == y->layer &&
                !memcmp(x->matrix, y->matrix, sizeof(x->matrix));
  }
  if (unchanged) {
    lua_getiuservalue(s, 1, 3);
    return 1;
  }
  size_t nv = 0, np = 0;
  memset(w->scratch_bounds, 0, w->nparts * 5 * sizeof(double));
  for (size_t i = 0; i < view.item_count; i++) {
    const h2_skeleton2d_draw_item_t *it = &view.items[i];
    if (it->resource < 1 || it->resource > w->nr)
      luaL_error(s, "skeleton2d: missing resource");
    resource_t *r = &w->resources[it->resource - 1];
    if (r->nv > w->vc - nv || r->np > w->pc - np)
      luaL_error(s, "skeleton2d: mesh capacity");
    double *b = w->scratch_bounds + it->part * 5;
    const double *m = it->matrix;
    for (size_t j = 0; j < r->nv; j++) {
      double x = m[0] * r->vertices[j].x + m[2] * r->vertices[j].y + m[4],
             y = m[1] * r->vertices[j].x + m[3] * r->vertices[j].y + m[5];
      if (!isfinite(x) || !isfinite(y) || fabs(x) > 1e6 || fabs(y) > 1e6)
        luaL_error(s, "skeleton2d: transformed coordinate range");
      w->vertices[nv + j] = (h2_lua_display_vertex_t){x, y};
      if (j == 0) {
        b[0] = 1;
        b[1] = b[3] = x;
        b[2] = b[4] = y;
      } else {
        b[1] = fmin(b[1], x);
        b[2] = fmin(b[2], y);
        b[3] = fmax(b[3], x);
        b[4] = fmax(b[4], y);
      }
    }
    if (!r->np)
      memset(b, 0, 5 * sizeof(double));
    for (size_t j = 0; j < r->np; j++) {
      w->primitives[np + j] = r->primitives[j];
      w->primitives[np + j].first += nv;
    }
    nv += r->nv;
    np += r->np;
  }
  if (!lua_checkstack(s, 4))
    luaL_error(s, "skeleton2d: stack capacity");
  lua_getiuservalue(s, 1, 3);
  h2_lua_display_mesh_data_t data = {w->vertices, nv, w->primitives, np};
  check(s, h2_lua_display_mesh_update(s, -1, &data), "update_mesh");
  memcpy(w->bounds, w->scratch_bounds, w->nparts * 5 * sizeof(double));
  memcpy(w->cached_items, view.items, view.item_count * sizeof(*view.items));
  w->cached_count = view.item_count;
  return 1;
}
static int bounds(lua_State *s) {
  writer_t *w = luaL_checkudata(s, 1, WRITER);
  h2_numeric_buffer_t *b = h2_numeric_check(s, 2);
  h2_numeric_capacity(s, b, w->nparts * 5);
  memcpy(b->data + b->count, w->bounds, w->nparts * 5 * sizeof(double));
  h2_numeric_commit(s, b, w->nparts * 5);
  lua_pushinteger(s, w->nparts);
  return 1;
}
static int writer_bytes(lua_State *s) {
  writer_t *w = luaL_checkudata(s, 1, WRITER);
  lua_pushinteger(s, w->bytes);
  return 1;
}
static void meta(lua_State *s, const char *name, const luaL_Reg *methods) {
  luaL_newmetatable(s, name);
  luaL_setfuncs(s, methods, 0);
  lua_pushvalue(s, -1);
  lua_setfield(s, -2, "__index");
  lua_pushstring(s, name);
  lua_setfield(s, -2, "__metatable");
  lua_pop(s, 1);
}
int h2_lua_open_skeleton2d(lua_State *s) {
  static const luaL_Reg defs[] = {{"instance", instance_new},
                                  {"bytes", bytes},
                                  {NULL, NULL}},
                        actors[] = {{"sample", sample},
                                    {"blend", blend},
                                    {"set_local", set_local},
                                    {"set_parts", set_parts},
                                    {"evaluate", evaluate},
                                    {"copy_matrices", copy_matrices},
                                    {"copy_draw_items", copy_items},
                                    {"bytes", bytes},
                                    {NULL, NULL}},
                        writers[] = {{"copy_bounds", bounds},
                                     {"bytes", writer_bytes},
                                     {NULL, NULL}};
  meta(s, DEF, defs);
  meta(s, ACTOR, actors);
  meta(s, WRITER, writers);
  static const luaL_Reg funcs[] = {{"compile", compile},
                                   {"mesh", mesh_new},
                                   {"update_mesh", update_mesh},
                                   {NULL, NULL}};
  luaL_newlib(s, funcs);
  return 1;
}
