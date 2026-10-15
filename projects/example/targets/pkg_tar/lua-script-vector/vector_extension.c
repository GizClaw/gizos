#include "h2_lua_display.h"
#include "h2_lua_module.h"
#include "h2_web_lua_app.h"
#include "lua.h"
#include "lauxlib.h"

/* Synthetic procedural art: no scene data or private Display headers. */
static int vector_new(lua_State *state) {
  const h2_lua_display_vertex_t vertices[] = {{150,20},{180,20},{180,50},{150,50}};
  const h2_lua_display_primitive_t polygon = {H2_LUA_DISPLAY_POLYGON,0,4,0xf800};
  const h2_lua_display_mesh_config_t config = {6,2,{vertices,4,&polygon,1}};
  h2_pal_result_t result = h2_lua_display_mesh_push(state, &config);
  if (result != H2_PAL_OK)
    return luaL_error(state, "native mesh create failed: %d", result);
  return 1;
}

static int vector_move(lua_State *state) {
  double dx = luaL_checknumber(state, 2);
  double dy = luaL_checknumber(state, 3);
  const h2_lua_display_vertex_t vertices[] = {
      {150+dx,20+dy},{180+dx,20+dy},{180+dx,50+dy},{150+dx,50+dy}};
  const h2_lua_display_primitive_t polygon = {H2_LUA_DISPLAY_POLYGON,0,4,0x001f};
  const h2_lua_display_mesh_data_t data = {vertices,4,&polygon,1};
  if (!lua_checkstack(state, 2))
    return luaL_error(state, "native mesh stack exhausted");
  h2_pal_result_t result = h2_lua_display_mesh_update(state, 1, &data);
  if (result != H2_PAL_OK)
    return luaL_error(state, "native mesh update failed: %d", result);
  return 0;
}

static int vector_open(void *lua_state, void *user) {
  lua_State *state = lua_state;
  (void)user;
  lua_newtable(state);
  lua_pushcfunction(state, vector_new); lua_setfield(state, -2, "new");
  lua_pushcfunction(state, vector_move); lua_setfield(state, -2, "move");
  return 1;
}

static h2_pal_result_t register_host(h2_lua_host_t *host) {
  return h2_lua_register_module(host, "vector_native", vector_open, NULL);
}

const h2_web_lua_app_extension_t h2_web_lua_app_extension = {
    .register_host = register_host,
};
