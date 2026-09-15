#include "h2_lua_numeric_internal.h"
#include "h2_lua_texture_internal.h"
#define TEXTURE "h2.display.texture"
#define BATCH "h2.display.texture_batch"
static lua_Integer integer(lua_State *s, int i, lua_Integer lo,
                           lua_Integer hi) {
  if (!lua_isinteger(s, i))
    luaL_error(s, "texture: expected integer");
  lua_Integer v = lua_tointeger(s, i);
  if (v < lo || v > hi)
    luaL_error(s, "texture: integer out of range");
  return v;
}
static void field(lua_State *s, int i, const char *key) {
  i = lua_absindex(s, i);
  lua_pushstring(s, key);
  lua_rawget(s, i);
}
static size_t intfield(lua_State *s, int i, const char *key) {
  field(s, i, key);
  size_t n = (size_t)integer(s, -1, 0, 4096);
  lua_pop(s, 1);
  return n;
}
static double numfield(lua_State *s, int i, const char *key) {
  field(s, i, key);
  if (lua_type(s, -1) != LUA_TNUMBER)
    luaL_error(s, "texture: expected number");
  double v = h2_numeric_number(s, -1);
  lua_pop(s, 1);
  return v;
}
static void meta(lua_State *s, const char *name) {
  luaL_newmetatable(s, name);
  lua_pushstring(s, name);
  lua_setfield(s, -2, "__metatable");
  lua_setmetatable(s, -2);
}
int h2_lua_texture_new(lua_State *s) {
  size_t w = (size_t)integer(s, 1, 1, 4096), h = (size_t)integer(s, 2, 1, 4096),
         n;
  luaL_checktype(s, 3, LUA_TSTRING);
  const char *p = lua_tolstring(s, 3, &n);
  if (n != w * h * 4)
    return luaL_error(s, "texture: RGBA byte length mismatch");
  h2_raster2d_texture_t *t = lua_newuserdatauv(s, sizeof(*t) + n, 0);
  *t = (h2_raster2d_texture_t){(uint8_t *)(t + 1), n, w, h, w * 4};
  memcpy((void *)t->rgba, p, n);
  meta(s, TEXTURE);
  return 1;
}
h2_lua_texture_batch_t *h2_lua_texture_batch_check(lua_State *s, int i) {
  return luaL_checkudata(s, i, BATCH);
}
h2_lua_texture_batch_t *h2_lua_texture_batch_push(lua_State *s, int at,
                                                  size_t cap) {
  at = lua_absindex(s, at);
  luaL_checktype(s, at, LUA_TTABLE);
  size_t nr = lua_rawlen(s, at);
  if (nr > 256 || cap > H2_RASTER2D_SPRITE_LIMIT)
    luaL_error(s, "texture: capacity exceeded");
  lua_pushnil(s);
  while (lua_next(s, at)) {
    if (!lua_isinteger(s, -2) || lua_tointeger(s, -2) < 1 ||
        (lua_Unsigned)lua_tointeger(s, -2) > nr)
      luaL_error(s, "texture: expected dense attachments");
    lua_pop(s, 1);
  }

  size_t bytes = sizeof(h2_lua_texture_batch_t) +
                 (nr + cap * 2) * sizeof(h2_raster2d_sprite_t);
  h2_lua_texture_batch_t *b = lua_newuserdatauv(s, bytes, 1);
  memset(b, 0, bytes);
  b->capacity = cap;
  b->resources = nr;
  b->source = b->storage;
  b->items = b->source + nr;
  b->scratch = b->items + cap;
  int batch = lua_gettop(s);
  meta(s, BATCH);
  lua_createtable(s, (int)nr, 0);
  int refs = lua_gettop(s);
  for (size_t i = 0; i < nr; i++) {
    lua_rawgeti(s, at, (lua_Integer)i + 1);
    int row = lua_gettop(s);
    luaL_checktype(s, row, LUA_TTABLE);
    field(s, row, "texture");
    h2_raster2d_texture_t *t = luaL_checkudata(s, -1, TEXTURE);
    b->source[i].texture = *t;
    lua_rawseti(s, refs, (lua_Integer)i + 1);
    h2_raster2d_sprite_t *r = &b->source[i];
    r->x = intfield(s, row, "x");
    r->y = intfield(s, row, "y");
    r->width = intfield(s, row, "width");
    r->height = intfield(s, row, "height");
    r->anchor_x = numfield(s, row, "anchor_x");
    r->anchor_y = numfield(s, row, "anchor_y");
    r->matrix[0] = r->matrix[3] = 1;
    if (h2_raster2d_sprite_validate(r))
      luaL_error(s, "texture: invalid attachment");
    lua_pop(s, 1);
  }
  lua_setiuservalue(s, batch, 1);
  return b;
}
int h2_lua_texture_batch_new(lua_State *s) {
  size_t cap = (size_t)integer(s, 2, 0, H2_RASTER2D_SPRITE_LIMIT);
  h2_lua_texture_batch_push(s, 1, cap);
  return 1;
}
int h2_lua_texture_update(lua_State *s) {
  h2_lua_texture_batch_t *b = h2_lua_texture_batch_check(s, 1);
  h2_numeric_buffer_t *v = h2_numeric_check(s, 2);
  if (b->producer_owned)
    return luaL_error(s, "texture: batch owned by producer");
  if (v->count % 7 || v->count / 7 > b->capacity)
    return luaL_error(s, "texture: invalid rows/capacity");
  size_t n = v->count / 7;
  for (size_t i = 0; i < n; i++) {
    double id = v->is_f32 ? (double)v->data.f32[i * 7] : v->data.f64[i * 7];
    if (!isfinite(id) || id < 1 || id > (double)b->resources || floor(id) != id)
      return luaL_error(s, "texture: invalid resource ID");
    b->scratch[i] = b->source[(size_t)id - 1];
    for (size_t j = 0; j < 6; ++j)
      b->scratch[i].matrix[j] = v->is_f32
          ? (double)v->data.f32[i * 7 + 1 + j]
          : v->data.f64[i * 7 + 1 + j];
    if (h2_raster2d_sprite_validate(&b->scratch[i]))
      return luaL_error(s, "texture: invalid matrix");
  }
  memcpy(b->items, b->scratch, n * sizeof(*b->items));
  b->count = n;
  return 0;
}
