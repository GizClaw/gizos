#ifndef H2_LUA_CANVAS_H
#define H2_LUA_CANVAS_H
#include "../runtime/h2_lua_internal.h"
/* Registers a scoped RGB888 composition surface on the display proxy. */
void h2_lua_canvas_register(lua_State *state, h2_lua_job_t *job);
void h2_lua_canvas_reset(lua_State *state);
#endif
