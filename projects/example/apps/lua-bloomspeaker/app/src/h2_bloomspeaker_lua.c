#include "h2_bloomspeaker_lua.h"

#include "lauxlib.h"
#include "lua.h"

static h2_bloomspeaker_lua_context_t *module_context(lua_State *state) {
  return lua_touserdata(state, lua_upvalueindex(1));
}

static uint64_t module_now_ms(h2_bloomspeaker_lua_context_t *context) {
  uint64_t now_ms = 0u;
  if (context != NULL && context->runtime != NULL &&
      context->runtime->time != NULL) {
    (void)h2_pal_time_get_monotonic_ms(context->runtime->time, &now_ms);
  }
  return now_ms;
}

static int module_long_press(lua_State *state) {
  h2_bloomspeaker_lua_context_t *context = module_context(state);
  if (context == NULL || context->controller == NULL) {
    return luaL_error(state, "intercom controller unavailable");
  }
  h2_bloomspeaker_controller_long_press(context->controller,
                                        module_now_ms(context));
  h2_bloomspeaker_snapshot_t snapshot;
  h2_bloomspeaker_controller_snapshot(context->controller, &snapshot);
  lua_pushstring(state, h2_bloomspeaker_state_name(snapshot.state));
  return 1;
}

static int module_hold_release(lua_State *state) {
  h2_bloomspeaker_lua_context_t *context = module_context(state);
  if (context == NULL || context->controller == NULL) {
    return luaL_error(state, "intercom controller unavailable");
  }
  h2_bloomspeaker_controller_hold_release(context->controller,
                                          module_now_ms(context));
  h2_bloomspeaker_snapshot_t snapshot;
  h2_bloomspeaker_controller_snapshot(context->controller, &snapshot);
  lua_pushstring(state, h2_bloomspeaker_state_name(snapshot.state));
  return 1;
}

static int module_snapshot(lua_State *state) {
  h2_bloomspeaker_lua_context_t *context = module_context(state);
  if (context == NULL || context->controller == NULL) {
    return luaL_error(state, "intercom controller unavailable");
  }
  h2_bloomspeaker_snapshot_t snapshot;
  h2_bloomspeaker_controller_snapshot(context->controller, &snapshot);
  lua_createtable(state, 0, 10);
  lua_pushstring(state, h2_bloomspeaker_state_name(snapshot.state));
  lua_setfield(state, -2, "state");
  lua_pushinteger(state, (lua_Integer)snapshot.state_entered_ms);
  lua_setfield(state, -2, "state_entered_ms");
  lua_pushinteger(state, (lua_Integer)snapshot.peer_tag);
  lua_setfield(state, -2, "peer_tag");
  lua_pushnumber(state, snapshot.local_level);
  lua_setfield(state, -2, "local_level");
  lua_pushnumber(state, snapshot.local_peak);
  lua_setfield(state, -2, "local_peak");
  lua_pushnumber(state, snapshot.remote_level);
  lua_setfield(state, -2, "remote_level");
  lua_pushnumber(state, snapshot.remote_peak);
  lua_setfield(state, -2, "remote_peak");
  lua_pushboolean(state, snapshot.native_audio);
  lua_setfield(state, -2, "native_audio");
  lua_pushinteger(state, snapshot.last_error);
  lua_setfield(state, -2, "last_error");
  lua_pushinteger(state, (lua_Integer)module_now_ms(context));
  lua_setfield(state, -2, "now_ms");
  return 1;
}

/* Allocation-free hot-path projection used by the render loop. */
static int module_sample(lua_State *state) {
  h2_bloomspeaker_lua_context_t *context = module_context(state);
  if (context == NULL || context->controller == NULL) {
    return luaL_error(state, "intercom controller unavailable");
  }
  h2_bloomspeaker_snapshot_t snapshot;
  h2_bloomspeaker_controller_snapshot(context->controller, &snapshot);
  lua_pushstring(state, h2_bloomspeaker_state_name(snapshot.state));
  lua_pushinteger(state, (lua_Integer)snapshot.state_entered_ms);
  lua_pushnumber(state, snapshot.local_level);
  lua_pushnumber(state, snapshot.local_peak);
  lua_pushnumber(state, snapshot.remote_level);
  lua_pushnumber(state, snapshot.remote_peak);
  lua_pushboolean(state, snapshot.native_audio);
  lua_pushinteger(state, snapshot.last_error);
  lua_pushinteger(state, (lua_Integer)module_now_ms(context));
  return 9;
}

static void set_module_function(lua_State *state, const char *name,
                                lua_CFunction function,
                                h2_bloomspeaker_lua_context_t *context) {
  lua_pushlightuserdata(state, context);
  lua_pushcclosure(state, function, 1);
  lua_setfield(state, -2, name);
}

int h2_bloomspeaker_lua_open(void *lua_state, void *user) {
  lua_State *state = lua_state;
  h2_bloomspeaker_lua_context_t *context = user;
  lua_createtable(state, 0, 4);
  set_module_function(state, "long_press", module_long_press, context);
  set_module_function(state, "hold_release", module_hold_release, context);
  set_module_function(state, "sample", module_sample, context);
  set_module_function(state, "snapshot", module_snapshot, context);
  return 1;
}
