#ifndef H2_LUA_MODULE_H
#define H2_LUA_MODULE_H

/** @file h2_lua_module.h @brief Static native module registration contract. */

#include "h2_lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Module open callback receives the current lua_State as an opaque pointer. */
typedef int (*h2_lua_module_open_fn)(void *lua_state, void *user);

h2_pal_result_t h2_lua_register_module(h2_lua_host_t *host, const char *name,
                                       h2_lua_module_open_fn open_fn,
                                       void *user);

#ifdef __cplusplus
}
#endif

#endif
