#include "h2_mem_arena.h"
#include "h2_tlsf.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

struct h2_mem_arena {
    h2_mem_arena_config_t config;
    h2_pal_mem_api_t api;
    tlsf_t pools[2];
    h2_mem_arena_stats_t stats;
};

typedef union arena_header {
    struct {
        void *base;
        size_t bytes;
        unsigned pool;
        bool fallback;
    } allocation;
#if defined(_MSC_VER)
    long double alignment;
    void *pointer_alignment;
    uint64_t integer_alignment;
#else
    max_align_t alignment;
#endif
} arena_header_t;

/* Native TLSF realloc can move to a differently aligned base. Spare padding
 * lets us realign the payload without requiring a second large allocation. */
#define BLOCK_OVERHEAD (sizeof(arena_header_t) + _Alignof(arena_header_t) - 1u)

static arena_header_t *header_for(void *base) {
    const uintptr_t start = (uintptr_t)base + sizeof(arena_header_t);
    const size_t align = _Alignof(arena_header_t);
    return (arena_header_t *)(start + (align - start % align) % align) - 1;
}

static h2_mem_arena_pool_stats_t *pool_stats(h2_mem_arena_t *arena,
                                             unsigned pool) {
    return pool == 0u ? &arena->stats.small : &arena->stats.large;
}

static void release(h2_mem_arena_t *arena, void *base, unsigned pool,
                    bool fallback) {
    if (fallback)
        h2_pal_mem_free(arena->config.fallback, base);
    else
        tlsf_free(arena->pools[pool], base);
}

static void account_remove(h2_mem_arena_t *arena, unsigned pool, bool fallback,
                           size_t bytes) {
    h2_mem_arena_pool_stats_t *stats = pool_stats(arena, pool);
    if (fallback) {
        stats->fallback_live_bytes -= bytes;
        arena->stats.fallback_live_bytes -= bytes;
    } else {
        stats->live_bytes -= bytes;
    }
}

static void arena_free(void *user, void *ptr) {
    if (ptr == NULL)
        return;
    h2_mem_arena_t *arena = user;
    arena->config.lock(arena->config.lock_user);
    arena_header_t *old = (arena_header_t *)ptr - 1;
    account_remove(arena, old->allocation.pool, old->allocation.fallback,
                   old->allocation.bytes);
    release(arena, old->allocation.base, old->allocation.pool,
            old->allocation.fallback);
    arena->config.unlock(arena->config.lock_user);
}

static void *arena_realloc(void *user, void *ptr, size_t bytes) {
    if (bytes == 0u) {
        arena_free(user, ptr);
        return NULL;
    }
    if (bytes > (size_t)PTRDIFF_MAX - BLOCK_OVERHEAD)
        return NULL;
    h2_mem_arena_t *arena = user;
    arena->config.lock(arena->config.lock_user);
    unsigned pool = arena->config.small_pool_bytes != 0u &&
                            bytes <= arena->config.small_request_max
                        ? 0u : 1u;
    h2_mem_arena_pool_stats_t *stats = pool_stats(arena, pool);
    arena_header_t *old = ptr != NULL ? (arena_header_t *)ptr - 1 : NULL;
    /* Capture everything before realloc, which can invalidate the header. */
    const bool had_old = old != NULL;
    void *old_base = had_old ? old->allocation.base : NULL;
    const size_t old_bytes = had_old ? old->allocation.bytes : 0u;
    const unsigned old_pool = had_old ? old->allocation.pool : pool;
    const bool old_fallback = had_old && old->allocation.fallback;
    const size_t old_offset =
        had_old ? (size_t)((uint8_t *)ptr - (uint8_t *)old_base) : 0u;
    const size_t copy_bytes = old_bytes < bytes ? old_bytes : bytes;
    const size_t total = BLOCK_OVERHEAD + bytes;
    void *base = NULL;
    bool resized = false;
    if (bytes > stats->largest_request)
        stats->largest_request = bytes;
    /* Bound before TLSF's size-class rounding/alignment arithmetic. */
    if (total < stats->reserved_bytes &&
        total < tlsf_block_size_max() - tlsf_align_size()) {
        if (had_old && !old_fallback && old_pool == pool) {
            base = tlsf_realloc(arena->pools[pool], old_base, total);
            resized = base != NULL;
        } else {
            base = tlsf_malloc(arena->pools[pool], total);
        }
    }
    /* A full pool borrows from its sibling before the request fails: the
     * split exists to keep small blocks away from large ones, not to strand
     * free arena space. The header records where the block really came from. */
    if (base == NULL && arena->config.small_pool_bytes != 0u) {
        const unsigned other = pool == 0u ? 1u : 0u;
        h2_mem_arena_pool_stats_t *other_stats = pool_stats(arena, other);
        if (total < other_stats->reserved_bytes &&
            total < tlsf_block_size_max() - tlsf_align_size()) {
            if (had_old && !old_fallback && old_pool == other) {
                base = tlsf_realloc(arena->pools[other], old_base, total);
                resized = base != NULL;
            } else {
                base = tlsf_malloc(arena->pools[other], total);
            }
            if (base != NULL) {
                pool = other;
                stats = other_stats;
                if (bytes > stats->largest_request)
                    stats->largest_request = bytes;
                if (stats->borrowed_count != UINT64_MAX)
                    ++stats->borrowed_count;
            }
        }
    }
    const bool fallback = base == NULL;
    if (fallback) {
        if (stats->fallback_count != UINT64_MAX)
            ++stats->fallback_count;
        stats->fallback_bytes = UINT64_MAX - stats->fallback_bytes < bytes
                                    ? UINT64_MAX : stats->fallback_bytes + bytes;
        const h2_pal_mem_api_t *mem = arena->config.fallback;
        if (old_fallback && mem != NULL && mem->vtable->realloc != NULL) {
            base = h2_pal_mem_realloc(mem, old_base, total);
            resized = base != NULL;
        } else {
            base = h2_pal_mem_alloc(mem, total);
        }
    }
    arena_header_t *next = base != NULL ? header_for(base) : NULL;
    if (next != NULL) {
        if (had_old) {
            if (resized) {
                memmove(next + 1, (uint8_t *)base + old_offset, copy_bytes);
            } else {
                memcpy(next + 1, ptr, copy_bytes);
                release(arena, old_base, old_pool, old_fallback);
            }
            account_remove(arena, old_pool, old_fallback, old_bytes);
        }
        next->allocation.base = base;
        next->allocation.bytes = bytes;
        next->allocation.pool = pool;
        next->allocation.fallback = fallback;
        if (fallback) {
            stats->fallback_live_bytes += bytes;
            arena->stats.fallback_live_bytes += bytes;
        } else {
            stats->live_bytes += bytes;
            if (stats->live_bytes > stats->peak_bytes)
                stats->peak_bytes = stats->live_bytes;
        }
    }
    arena->config.unlock(arena->config.lock_user);
    return next != NULL ? next + 1 : NULL;
}

static void *arena_alloc(void *user, size_t bytes) {
    return arena_realloc(user, NULL, bytes);
}

static const h2_pal_mem_vtable_t arena_vtable = {
    .alloc = arena_alloc,
    .realloc = arena_realloc,
    .free = arena_free,
};

static bool pool_size_valid(size_t bytes) {
    const size_t overhead = tlsf_size() + tlsf_pool_overhead();
    return bytes >= overhead + tlsf_block_size_min() &&
           bytes - overhead <= tlsf_block_size_max();
}

h2_pal_result_t h2_mem_arena_create(const h2_mem_arena_config_t *config,
                                     h2_mem_arena_t **out_arena) {
    if (out_arena == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    *out_arena = NULL;
    const size_t align = tlsf_align_size();
    const size_t prefix = (sizeof(h2_mem_arena_t) + align - 1u) / align * align;
    if (config == NULL || config->block == NULL || config->lock == NULL ||
        config->unlock == NULL || config->block_bytes > (size_t)PTRDIFF_MAX ||
        (uintptr_t)config->block % _Alignof(h2_mem_arena_t) != 0u ||
        (uintptr_t)config->block % align != 0u ||
        config->small_pool_bytes % align != 0u ||
        config->small_pool_bytes >= config->block_bytes)
        return H2_PAL_ERR_INVALID_ARG;
    const h2_pal_mem_api_t *fallback = config->fallback;
    if (fallback != NULL && (fallback->vtable == NULL ||
        fallback->vtable->alloc == NULL || fallback->vtable->free == NULL))
        return H2_PAL_ERR_INVALID_ARG;
    const bool split = config->small_pool_bytes != 0u;
    const size_t first = split ? config->small_pool_bytes : config->block_bytes;
    if (first < prefix || !pool_size_valid(first - prefix) ||
        (split && !pool_size_valid(config->block_bytes - first)))
        return H2_PAL_ERR_INVALID_ARG;
    h2_mem_arena_t *arena = config->block;
    const h2_mem_arena_config_t saved = *config;
    memset(arena, 0, sizeof(*arena));
    arena->config = saved;
    arena->api = (h2_pal_mem_api_t){.user = arena, .vtable = &arena_vtable};
    arena->stats.reserved_bytes = saved.block_bytes;
    arena->stats.small.reserved_bytes = saved.small_pool_bytes;
    arena->stats.large.reserved_bytes = saved.block_bytes - saved.small_pool_bytes;
    arena->pools[split ? 0 : 1] =
        tlsf_create_with_pool((uint8_t *)saved.block + prefix, first - prefix);
    if (split)
        arena->pools[1] = tlsf_create_with_pool((uint8_t *)saved.block + first,
                                               saved.block_bytes - first);
    *out_arena = arena;
    return H2_PAL_OK;
}

const h2_pal_mem_api_t *h2_mem_arena_mem(h2_mem_arena_t *arena) {
    return arena != NULL ? &arena->api : NULL;
}

h2_pal_result_t h2_mem_arena_stats(h2_mem_arena_t *arena,
                                    h2_mem_arena_stats_t *out_stats) {
    if (out_stats == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    memset(out_stats, 0, sizeof(*out_stats));
    if (arena == NULL)
        return H2_PAL_ERR_INVALID_STATE;
    arena->config.lock(arena->config.lock_user);
    *out_stats = arena->stats;
    arena->config.unlock(arena->config.lock_user);
    return H2_PAL_OK;
}

h2_pal_result_t h2_mem_arena_destroy(h2_mem_arena_t *arena) {
    if (arena == NULL)
        return H2_PAL_OK;
    arena->config.lock(arena->config.lock_user);
    h2_pal_result_t rc = H2_PAL_ERR_INVALID_STATE;
    if (arena->stats.small.live_bytes == 0u && arena->stats.large.live_bytes == 0u &&
        arena->stats.fallback_live_bytes == 0u) {
        for (unsigned i = 0u; i < 2u; ++i)
            if (arena->pools[i] != NULL)
                tlsf_destroy(arena->pools[i]);
        arena->api.vtable = NULL;
        rc = H2_PAL_OK;
    }
    arena->config.unlock(arena->config.lock_user);
    return rc;
}

static void inspect_block(void *ptr, size_t bytes, int used, void *user) {
    (void)ptr;
    h2_mem_arena_pool_inspection_t *out = user;
    if (used) {
        out->consumed_bytes += bytes + tlsf_alloc_overhead();
    } else {
        out->free_bytes += bytes;
        if (bytes > out->largest_free_block)
            out->largest_free_block = bytes;
    }
}

h2_pal_result_t h2_mem_arena_inspect(h2_mem_arena_t *arena,
                                    h2_mem_arena_pool_inspection_t out_pools[2]) {
    if (out_pools == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    memset(out_pools, 0, 2u * sizeof(*out_pools));
    if (arena == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    arena->config.lock(arena->config.lock_user);
    for (unsigned i = 0u; i < 2u; ++i)
        if (arena->pools[i] != NULL)
            tlsf_walk_pool(tlsf_get_pool(arena->pools[i]), inspect_block,
                           &out_pools[i]);
    arena->config.unlock(arena->config.lock_user);
    return H2_PAL_OK;
}

h2_pal_result_t h2_mem_arena_block_info(h2_mem_arena_t *arena, const void *ptr,
                                       h2_mem_arena_block_info_t *out_info) {
    if (out_info == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    memset(out_info, 0, sizeof(*out_info));
    if (arena == NULL || ptr == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    arena->config.lock(arena->config.lock_user);
    const arena_header_t *header = (const arena_header_t *)ptr - 1;
    out_info->requested_bytes = header->allocation.bytes;
    out_info->pool = header->allocation.pool;
    out_info->fallback = header->allocation.fallback;
    out_info->consumed_bytes = header->allocation.fallback
        ? header->allocation.bytes + BLOCK_OVERHEAD
        : tlsf_block_size(header->allocation.base) + tlsf_alloc_overhead();
    arena->config.unlock(arena->config.lock_user);
    return H2_PAL_OK;
}
