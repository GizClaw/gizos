#include "h2_lua_display.h"
#include "h2_lua_numeric_internal.h"
#include "h2_lua_geometry_batches_internal.h"

static void distinct(lua_State *s, h2_numeric_buffer_t *a,
                     h2_numeric_buffer_t *b) {
  if (a == b)
    luaL_error(s, "output buffers must be distinct");
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

#define H2_NUMERIC_F32 1
#include "h2_lua_geometry_kernels.h"
#undef H2_NUMERIC_F32
#define H2_NUMERIC_F32 0
#include "h2_lua_geometry_kernels.h"
#undef H2_NUMERIC_F32

H2_NUMERIC_DISPATCH(affine, 1)
H2_NUMERIC_DISPATCH(displace, 1)
H2_NUMERIC_DISPATCH(rotate, 1)
H2_NUMERIC_DISPATCH(prefix, 1)
H2_NUMERIC_DISPATCH(project, 1)
H2_NUMERIC_DISPATCH(project_segments, 1)
H2_NUMERIC_DISPATCH(segments, 1)
H2_NUMERIC_DISPATCH(split, 1)
H2_NUMERIC_DISPATCH(mesh_update, 2)

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
  h2_geometry_prepared_register(s);
  h2_geometry_batches_register(s);
  lua_pushinteger(s, 2);
  lua_pushcclosure(s, affine, 1);
  lua_setfield(s, -2, "affine2");
  lua_pushinteger(s, 3);
  lua_pushcclosure(s, affine, 1);
  lua_setfield(s, -2, "affine3");
  return 1;
}
