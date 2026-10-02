#ifndef H2_LUA_DISPLAY_QUAD_BATCH_H
#define H2_LUA_DISPLAY_QUAD_BATCH_H

/** @file h2_lua_display_quad_batch.h
 * @brief Optional bounded quad batch component for Lua Display.
 */

#include "h2_lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Enable compile_quad_batch and draw_quad_batch for one Host.
 *
 * Link //libs/lua:lua_display_quad_batch and call once after successful
 * h2_lua_host_create(), before h2_lua_host_start(). The caller must serialize
 * this configuration with other Host lifecycle/registration operations.
 * This nonblocking call borrows host, allocates nothing and does not acquire
 * Display. No context or cleanup is required; batch userdata remains owned
 * and memory-accounted by each VM until GC or VM teardown.
 *
 * Without this call, both Lua fields are nil, even if the component is linked
 * or another Host has enabled it. Core-only consumers do not link the batch
 * implementation. The Lua API contract is documented in h2_lua_display.h.
 *
 * @param host Borrowed live Host to configure; must not be NULL.
 * @return H2_PAL_OK on success; H2_PAL_ERR_INVALID_ARG for NULL;
 * H2_PAL_ERR_INVALID_STATE if already enabled, started, or stopping/stopped.
 * Failure leaves the Host unchanged. There is no disable operation.
 */
h2_pal_result_t h2_lua_display_quad_batch_enable(h2_lua_host_t *host);

#ifdef __cplusplus
}
#endif

#endif
