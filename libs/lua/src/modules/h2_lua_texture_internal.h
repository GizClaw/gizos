#ifndef H2_LUA_TEXTURE_INTERNAL_H
#define H2_LUA_TEXTURE_INTERNAL_H
/* Display-owned implementation detail; borrowed only during owning VM calls. */
#include "h2_raster2d.h"
#include "lua.h"
typedef struct h2_lua_texture_batch {
  size_t resources, capacity, count;
  int producer_owned;
  h2_raster2d_sprite_t *source, *items, *scratch;
  /* Flexible storage forces correct double alignment on 32-bit/WASM too. */
  h2_raster2d_sprite_t storage[];
} h2_lua_texture_batch_t;
_Static_assert(offsetof(h2_lua_texture_batch_t, storage) %
                   _Alignof(h2_raster2d_sprite_t) == 0,
               "texture batch storage alignment");
int h2_lua_texture_new(lua_State *s);
int h2_lua_texture_batch_new(lua_State *s);
int h2_lua_texture_update(lua_State *s);
h2_lua_texture_batch_t *h2_lua_texture_batch_check(lua_State *s, int index);
/* Pushes a batch, copies attachments and retains their textures; can raise. */
h2_lua_texture_batch_t *h2_lua_texture_batch_push(lua_State *s, int attachments,
                                                  size_t capacity);
#endif
