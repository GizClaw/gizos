#ifndef H2_LUA_PLATFORM_SMOKE_H
#define H2_LUA_PLATFORM_SMOKE_H
#include "h2_runtime.h"
#ifdef __cplusplus
extern "C" {
#endif
/** Run a bounded Lua scheduler smoke against borrowed Runtime. Returns zero on
 * success. Ordinary task-context calls only. A persistent shutdown failure
 * keeps this call blocked with the Host and Runtime alive for platform
 * recovery. */
int h2_lua_platform_smoke_run(h2_runtime_t* runtime);
#ifdef __cplusplus
}
#endif
#endif
