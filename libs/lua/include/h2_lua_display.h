#ifndef H2_LUA_DISPLAY_H
#define H2_LUA_DISPLAY_H

/** @file h2_lua_display.h
 * @brief VM-owned procedural geometry shared by native producers and Display.
 */
#include "h2/pal/core/h2_pal_errors.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_LUA_DISPLAY_VERTEX_LIMIT 65536u
#define H2_LUA_DISPLAY_PRIMITIVE_LIMIT 4096u

/** Finite screen-space coordinates in [-1000000, 1000000]. */
typedef struct h2_lua_display_vertex {
  double x;
  double y;
} h2_lua_display_vertex_t;

typedef enum h2_lua_display_primitive_kind {
  H2_LUA_DISPLAY_POLYGON = 0,
  H2_LUA_DISPLAY_LINE = 1,
} h2_lua_display_primitive_kind_t;

/** Ordered primitive referencing a contiguous range of the vertex array.
 * Polygons use 3..128 vertices; lines use exactly two. Ranges may overlap.
 */
typedef struct h2_lua_display_primitive {
  h2_lua_display_primitive_kind_t kind;
  size_t first; /**< Zero-based first vertex (Lua primitive tables are one-based). */
  size_t count;
  uint16_t color; /**< Native RGB565 value, not a serialized byte stream. */
} h2_lua_display_primitive_t;

/** Borrowed input arrays, stable throughout the call, including any GC during
 * push. NULL arrays are allowed only when their corresponding count is zero.
 * Arrays must not point into a retained batch's private storage.
 */
typedef struct h2_lua_display_mesh_data {
  const h2_lua_display_vertex_t *vertices;
  size_t vertex_count;
  const h2_lua_display_primitive_t *primitives;
  size_t primitive_count;
} h2_lua_display_mesh_data_t;

/** Fixed capacities, independently bounded by the two public limits. Initial
 * active lengths must fit. A zero-capacity empty batch is valid.
 */
typedef struct h2_lua_display_mesh_config {
  size_t vertex_capacity;
  size_t primitive_capacity;
  h2_lua_display_mesh_data_t initial;
} h2_lua_display_mesh_config_t;

/**
 * @brief Copy initial data and push one typed, VM-owned mesh userdata.
 * @param lua_state Current Lua state, borrowed within its owning native callback.
 * @param config Borrowed configuration; no pointers are retained.
 * @return OK pushes exactly one userdata. INVALID_ARG rejects malformed inputs;
 * NO_MEMORY reports protected allocation/stack-reservation failure;
 * INVALID_STATE reports other protected Lua failures. Failure preserves stack.
 *
 * Synchronous, owning worker only; not thread-safe or ISR-safe. This function
 * reserves its own scratch stack and protects Lua allocations from longjmp
 * across the caller's resource ownership. Geometry and transformed-coordinate
 * storage count toward VM memory and are reclaimed with the userdata/VM.
 * Neither this operation nor update opens, draws or presents the Display.
 * Keep the userdata referenced in Lua; never retain its internal native address.
 */
h2_pal_result_t h2_lua_display_mesh_push(
    void *lua_state, const h2_lua_display_mesh_config_t *config);

/**
 * @brief Transactionally replace active vertices, topology and colors.
 * @param lua_state Current state in the owning native callback.
 * @param stack_index Positive or relative stack index of a retained mesh;
 * zero, pseudo-indices and other userdata are rejected.
 * @param data Borrowed replacement arrays, copied only after full validation.
 * @return OK replaces data and invalidates derived coordinates/span caches. INVALID_ARG
 * leaves all prior data/cache state unchanged. Stack is unchanged on both paths.
 *
 * No allocation, stack growth, Lua callback or framebuffer access. Caller must
 * provide two unused stack slots (reserve with lua_checkstack before entering
 * a no-allocation section). The same worker/lifetime constraints as push apply.
 * Changes to active lengths, including empty, must fit the original capacities.
 */
h2_pal_result_t h2_lua_display_mesh_update(
    void *lua_state, int stack_index, const h2_lua_display_mesh_data_t *data);

#ifdef __cplusplus
}
#endif
#endif
