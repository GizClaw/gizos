#ifndef H2_LUA_VECTOR_SW_H
#define H2_LUA_VECTOR_SW_H
#include <stddef.h>
#include <stdint.h>
/* Bounded, synchronous H2VG software renderer. Borrowed inputs; caller owns the
 * width * height * 4 output bytes (premultiplied RGBA). Zero means invalid
 * input or exhausted workspace. No retained state or allocations between calls.
 */
int h2_lua_vector_sw_render(const uint8_t *data, size_t length, uint8_t *rgba,
                            unsigned width, unsigned height,
                            const double matrix[6]);
/* Optional cooperative poll. Return zero to abort with normal cleanup; the
 * callback must not longjmp/yield the Lua VM across this native call. */
typedef int (*h2_lua_vector_sw_poll_fn)(void *user);
typedef enum {
  H2_LUA_VECTOR_SW_INVALID = 0,
  H2_LUA_VECTOR_SW_OK = 1,
  H2_LUA_VECTOR_SW_NO_MEMORY,
  H2_LUA_VECTOR_SW_INTERRUPTED,
  H2_LUA_VECTOR_SW_WORKSPACE_LIMIT,
} h2_lua_vector_sw_result_t;
/* Optional caller-owned, malloc-aligned coverage cache. No colors or clip masks
 * are cached; changing opacity/paint retains exactly the uncached pixels.
 * Storage and cache are confined to the renderer's calling thread. */
typedef struct h2_lua_vector_sw_cache h2_lua_vector_sw_cache_t;
size_t h2_lua_vector_sw_cache_bytes(size_t payload_bytes);
h2_lua_vector_sw_cache_t *h2_lua_vector_sw_cache_init(void *storage, size_t bytes);
h2_lua_vector_sw_result_t h2_lua_vector_sw_render_cached_result(
    const uint8_t *data,size_t length,uint8_t *rgba,uint8_t *const *rows,
    unsigned width,unsigned height,const double matrix[6],
    h2_lua_vector_sw_poll_fn poll,void *user,h2_lua_vector_sw_cache_t *cache);
/* Detailed result after all native workspaces have been freed. Exactly one
 * output layout is supplied; callers may reclaim memory and retry NO_MEMORY. */
h2_lua_vector_sw_result_t h2_lua_vector_sw_render_result(
    const uint8_t *data,size_t length,uint8_t *rgba,uint8_t *const *rows,
    unsigned width,unsigned height,const double matrix[6],
    h2_lua_vector_sw_poll_fn poll,void *user);
int h2_lua_vector_sw_render_with_poll(const uint8_t *data, size_t length,
    uint8_t *rgba, unsigned width, unsigned height, const double matrix[6],
    h2_lua_vector_sw_poll_fn poll, void *user);
/* Scattered output: height caller-owned, non-overlapping rows, each containing
 * at least width*4 bytes. Identical pixels; no contiguous frame allocation. */
int h2_lua_vector_sw_render_rows_with_poll(const uint8_t *data, size_t length,
    uint8_t *const *rows, unsigned width, unsigned height, const double matrix[6],
    h2_lua_vector_sw_poll_fn poll, void *user);
#endif
