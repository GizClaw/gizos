#ifndef H2_MEM_ARENA_H
#define H2_MEM_ARENA_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/os/h2_pal_mem.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_mem_arena h2_mem_arena_t;

typedef struct h2_mem_arena_config {
    /** Optional borrowed diagnostic name, valid until destroy. */
    const char *name;
    /** Caller-owned writable region, aligned as malloc, borrowed until destroy.
     * Includes arena/TLSF metadata; create never allocates outside this block. */
    void *block;
    size_t block_bytes;
    /** Payload requests <= this value prefer the small pool in split mode. */
    size_t small_request_max;
    /** First region's size, a multiple of TLSF alignment (8 bytes is portable).
     * Zero disables the small pool: large serves every size. In split mode
     * both regions must fit their metadata plus a TLSF minimum block. */
    size_t small_pool_bytes;
    /** Borrowed until destroy. NULL means pool exhaustion returns NULL.
     * Requires alloc/free; realloc is optional. Must not reenter this arena. */
    const h2_pal_mem_api_t *fallback;
    /** Required paired callbacks. Return only with the lock held/released.
     * Called around allocation, stats and destroy, including fallback calls.
     * No reentry; RTOS locks must inherit priority. Single-threaded callers
     * may provide no-op callbacks. The core never spins. */
    void (*lock)(void *user);
    void (*unlock)(void *user);
    void *lock_user;
} h2_mem_arena_config_t;

typedef struct h2_mem_arena_pool_stats {
    /** Full region size, including metadata/alignment overhead. */
    size_t reserved_bytes;
    /** Requested payload in this pool, excluding fallback and metadata. */
    size_t live_bytes;
    size_t peak_bytes;
    /** Largest payload requested from this class or successfully borrowed. */
    size_t largest_request;
    /** Cumulative misses after trying both pools, charged to the requested
     * class, including failed or absent fallback. Unrepresentable sizes are
     * rejected before counting a request.
     * Counters saturate at UINT64_MAX. */
    uint64_t fallback_count;
    uint64_t fallback_bytes;
    /** Live fallback payload belonging to this request class. */
    size_t fallback_live_bytes;
    /** Cumulative successful allocations/reallocations this pool served for
     * its sibling request class. Saturates at UINT64_MAX; free never decrements
     * it. Live/peak bytes belong to the pool actually serving the block. */
    uint64_t borrowed_count;
} h2_mem_arena_pool_stats_t;

typedef struct h2_mem_arena_stats {
    size_t reserved_bytes;
    h2_mem_arena_pool_stats_t small;
    h2_mem_arena_pool_stats_t large;
    size_t fallback_live_bytes;
} h2_mem_arena_stats_t;

/** @brief Initialize an arena inside config->block; *out_arena is NULL on error.
 * Returns INVALID_ARG for invalid config, alignment or pool sizes. All config
 * pointers are borrowed; the config struct itself is copied. The caller must
 * exclusively own the region during create and never create over a live arena.
 */
h2_pal_result_t h2_mem_arena_create(const h2_mem_arena_config_t *config,
                                     h2_mem_arena_t **out_arena);

/** @brief Borrow Memory PAL until destroy (NULL arena returns NULL).
 * Allocations are aligned for fundamental C types. Only this API may free or
 * realloc its blocks. Zero size frees and returns NULL; free(NULL) is a no-op.
 * Requests try their preferred size class, then the sibling pool, then the
 * optional fallback. Realloc can resize in its actual owning pool or migrate
 * across pools/fallback, preserving the old allocation/data on failure.
 */
const h2_pal_mem_api_t *h2_mem_arena_mem(h2_mem_arena_t *arena);

/** @brief Copy a snapshot under the caller's lock. No logging under the lock.
 * NULL output returns INVALID_ARG; NULL arena zeros output and returns
 * INVALID_STATE. */
h2_pal_result_t h2_mem_arena_stats(h2_mem_arena_t *arena,
                                    h2_mem_arena_stats_t *out_stats);

/** On-demand diagnostics; no additional state or allocation-path work. */
typedef struct h2_mem_arena_pool_inspection {
    size_t free_bytes;
    size_t largest_free_block;
    /** TLSF block sizes plus per-allocation TLSF headers, including arena
     * headers/alignment. Fixed arena/control/pool metadata is not included. */
    size_t consumed_bytes;
} h2_mem_arena_pool_inspection_t;

typedef struct h2_mem_arena_block_info {
    size_t requested_bytes;
    size_t consumed_bytes;
    unsigned pool; /* 0: small, 1: large */
    int fallback;
} h2_mem_arena_block_info_t;

/** @brief Walk both pools under the arena lock, only at diagnostic boundaries.
 * out_pools points to two entries (small, large). NULL arguments are invalid.
 * Free sizes are raw TLSF capacity, before arena header/alignment overhead.
 */
h2_pal_result_t h2_mem_arena_inspect(h2_mem_arena_t *arena,
                                    h2_mem_arena_pool_inspection_t out_pools[2]);

/** @brief Inspect a live block belonging to this arena in constant time.
 * Caller must exclude concurrent free/realloc of ptr. NULL arguments fail.
 * Fallback consumed_bytes is a lower bound: bytes requested from the fallback
 * allocator, whose own usable size/header is not part of Memory PAL.
 */
h2_pal_result_t h2_mem_arena_block_info(h2_mem_arena_t *arena, const void *ptr,
                                       h2_mem_arena_block_info_t *out_info);

/** @brief Refuse live pool/fallback blocks with INVALID_STATE, keeping arena.
 * Caller must stop/join all borrowers before destroy. NULL succeeds. Successful
 * destroy invalidates the handle/API but never frees the caller's block/lock.
 */
h2_pal_result_t h2_mem_arena_destroy(h2_mem_arena_t *arena);

#ifdef __cplusplus
}
#endif
#endif
