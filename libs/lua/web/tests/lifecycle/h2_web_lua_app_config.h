#ifndef H2_WEB_LUA_APP_CONFIG_H
#define H2_WEB_LUA_APP_CONFIG_H
#include "h2_lua_job.h"
extern const uint8_t *test_source;
extern size_t test_source_size;
#define H2_WEB_LUA_APP_NAME "lifecycle"
#define H2_WEB_LUA_APP_SOURCE test_source
#define H2_WEB_LUA_APP_SOURCE_SIZE test_source_size
#define H2_WEB_LUA_APP_EXIT_BUTTON ""
#define H2_WEB_LUA_APP_EXTENSION 0
#define H2_WEB_LUA_APP_RUN_MS 0u
#define H2_WEB_LUA_APP_VM_BYTES (2u * 1024u * 1024u)
#define H2_WEB_LUA_APP_SOURCE_LIMIT_BYTES 131072u
#define H2_WEB_LUA_APP_ARG_COUNT 0u
static const h2_lua_arg_t h2_web_lua_script_args[] = {{NULL, NULL}};
#endif
