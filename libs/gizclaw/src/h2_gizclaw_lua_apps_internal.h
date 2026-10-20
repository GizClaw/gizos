#ifndef H2_GIZCLAW_LUA_APPS_INTERNAL_H
#define H2_GIZCLAW_LUA_APPS_INTERNAL_H
#include "h2_gizclaw_config.h"
typedef struct h2_gizclaw_lua_apps h2_gizclaw_lua_apps_t;
h2_pal_result_t h2_gizclaw_lua_apps_create_internal(
    const h2_gizclaw_config_t* config, h2_gizclaw_lua_apps_t** out);
void h2_gizclaw_lua_apps_cancel_internal(h2_gizclaw_lua_apps_t* apps);
void h2_gizclaw_lua_apps_destroy_internal(h2_gizclaw_lua_apps_t* apps);
int h2_gizclaw_lua_apps_rpc_internal(h2_gizclaw_lua_apps_t* apps,
                                     h2_gizclaw_tool_t tool,
                                     h2_gizclaw_rpc_bytes_t bytes,
                                     h2_gizclaw_rpc_provider_response_t* out);
#endif
