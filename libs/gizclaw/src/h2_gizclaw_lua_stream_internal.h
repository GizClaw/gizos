#ifndef H2_GIZCLAW_LUA_STREAM_INTERNAL_H
#define H2_GIZCLAW_LUA_STREAM_INTERNAL_H

#include "h2_gizclaw_config.h"
#include "h2/pal/os/h2_pal_sync.h"
#include "h2/pal/os/h2_pal_task.h"
#include "gzc_client.h"

typedef struct h2_gizclaw_lua_stream h2_gizclaw_lua_stream_t;
int h2_gizclaw_lua_stream_create_internal(const h2_gizclaw_config_t *config,
                                          const h2_pal_task_api_t *task,
                                          const h2_pal_sync_api_t *sync,
                                          h2_gizclaw_lua_stream_t **out);
int h2_gizclaw_lua_stream_start_internal(h2_gizclaw_lua_stream_t *stream);
void h2_gizclaw_lua_stream_cancel_internal(h2_gizclaw_lua_stream_t *stream);
int h2_gizclaw_lua_stream_stop_internal(h2_gizclaw_lua_stream_t *stream);
void h2_gizclaw_lua_stream_destroy_internal(h2_gizclaw_lua_stream_t *stream);
const gzc_rpc_stream_provider_t *
h2_gizclaw_lua_stream_provider_internal(h2_gizclaw_lua_stream_t *stream);
#endif
