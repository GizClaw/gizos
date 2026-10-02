#ifndef H2_LUA_TASK_NAMES_H
#define H2_LUA_TASK_NAMES_H

#define H2_LUA_WORKER_TASK_NAME_VALUE "$lua/worker"
#define H2_LUA_DISPLAY_TASK_NAME_VALUE "$lua/display"

#ifdef __cplusplus
extern "C" {
#endif

extern const char
    h2_lua_worker_task_name[sizeof(H2_LUA_WORKER_TASK_NAME_VALUE)];
extern const char
    h2_lua_display_task_name[sizeof(H2_LUA_DISPLAY_TASK_NAME_VALUE)];

#ifdef __cplusplus
}
#endif

#endif
