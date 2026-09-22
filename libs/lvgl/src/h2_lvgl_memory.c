#include "h2_lvgl_memory.h"

#include "h2_tlsf.h"

#include <stdint.h>
#include <string.h>

typedef union h2_lvgl_chunk h2_lvgl_chunk_t;
union h2_lvgl_chunk {
    struct {
        h2_lvgl_chunk_t *next;
        size_t bytes;
        pool_t pool;
    } info;
    max_align_t alignment;
};

typedef union h2_lvgl_direct h2_lvgl_direct_t;
union h2_lvgl_direct {
    struct {
        h2_lvgl_direct_t *next;
        size_t bytes;
    } info;
    max_align_t alignment;
};

typedef struct h2_lvgl_memory_state {
    const h2_pal_mem_api_t *allocator;
    const h2_pal_sync_api_t *sync;
    h2_pal_mutex_t *mutex;
    tlsf_t tlsf;
    h2_lvgl_chunk_t *chunks;
    h2_lvgl_direct_t *direct;
    size_t initial_bytes;
    size_t grow_bytes;
    size_t chunk_bytes;
    int threaded;
} h2_lvgl_memory_state_t;

static h2_lvgl_memory_state_t s_memory;

static int memory_lock(void) {
    return !s_memory.threaded ||
        h2_pal_mutex_lock(s_memory.sync, s_memory.mutex) == H2_PAL_OK;
}

static void memory_unlock(void) {
    if (s_memory.threaded)
        (void)h2_pal_mutex_unlock(s_memory.sync, s_memory.mutex);
}

static size_t pool_alignment(void) {
    const size_t alignment = _Alignof(max_align_t);
    return alignment > tlsf_align_size() ? alignment : tlsf_align_size();
}

static size_t pooled_size(size_t size) {
    const size_t alignment = pool_alignment();
    const size_t overhead = tlsf_alloc_overhead();
    if (size < tlsf_block_size_min()) size = tlsf_block_size_min();
    /* Pooled requests are bounded by half a validated chunk size. Keep every
     * block's physical stride aligned, including the TLSF size word. */
    return ((size + overhead + alignment - 1u) & ~(alignment - 1u)) - overhead;
}

static int add_chunk(size_t bytes) {
    const size_t alignment = pool_alignment();
    const size_t padding = (alignment - tlsf_alloc_overhead() % alignment) % alignment;
    const size_t overhead = sizeof(h2_lvgl_chunk_t) + padding;
    if (bytes > SIZE_MAX - overhead ||
        s_memory.chunk_bytes > SIZE_MAX - bytes - overhead)
        return H2_PAL_ERR_NO_MEMORY;
    h2_lvgl_chunk_t *chunk = h2_pal_mem_alloc(
        s_memory.allocator, overhead + bytes);
    if (chunk == NULL) return H2_PAL_ERR_NO_MEMORY;
    /* The first user pointer is one TLSF size word after the pool start.
     * Combined with pooled_size(), this keeps split/coalesced blocks aligned
     * without tlsf_memalign's conservative extra-space search on every call. */
    chunk->info.pool = tlsf_add_pool(
        s_memory.tlsf, (unsigned char *)(chunk + 1) + padding, bytes);
    if (chunk->info.pool == NULL) {
        h2_pal_mem_free(s_memory.allocator, chunk);
        return H2_PAL_ERR_INVALID_ARG;
    }
    chunk->info.bytes = bytes;
    chunk->info.next = s_memory.chunks;
    s_memory.chunks = chunk;
    s_memory.chunk_bytes += overhead + bytes;
    return H2_PAL_OK;
}

void h2_lvgl_memory_release(void) {
    /* Consumers and workers are stopped. Do not remove individual TLSF pools:
     * teardown invalidates even outstanding blocks and releases all backing. */
    if (s_memory.tlsf != NULL) {
        tlsf_destroy(s_memory.tlsf);
        h2_pal_mem_free(s_memory.allocator, s_memory.tlsf);
        s_memory.tlsf = NULL;
    }
    while (s_memory.chunks != NULL) {
        h2_lvgl_chunk_t *chunk = s_memory.chunks;
        s_memory.chunks = chunk->info.next;
        h2_pal_mem_free(s_memory.allocator, chunk);
    }
    while (s_memory.direct != NULL) {
        h2_lvgl_direct_t *direct = s_memory.direct;
        s_memory.direct = direct->info.next;
        h2_pal_mem_free(s_memory.allocator, direct);
    }
    s_memory.chunk_bytes = 0u;
    if (s_memory.mutex != NULL) {
        (void)h2_pal_mutex_destroy(s_memory.sync, s_memory.mutex);
        s_memory.mutex = NULL;
    }
}

void h2_lvgl_memory_deinit(void) {
    h2_lvgl_memory_release();
    memset(&s_memory, 0, sizeof(s_memory));
}

int h2_lvgl_memory_prepare(void) {
    if (s_memory.allocator == NULL) return H2_PAL_ERR_INVALID_STATE;
    if (s_memory.initial_bytes == 0u || s_memory.tlsf != NULL)
        return H2_PAL_OK;
    if (s_memory.threaded) {
        const h2_pal_mutex_config_t config = {
            .name = "lvgl/memory",
            .allocator = s_memory.allocator,
        };
        int rc = h2_pal_mutex_create(s_memory.sync, &config, &s_memory.mutex);
        if (rc != H2_PAL_OK) return rc;
    }
    void *control = h2_pal_mem_alloc(s_memory.allocator, tlsf_size());
    if (control == NULL) {
        h2_lvgl_memory_release();
        return H2_PAL_ERR_NO_MEMORY;
    }
    s_memory.tlsf = tlsf_create(control);
    if (s_memory.tlsf == NULL) {
        h2_pal_mem_free(s_memory.allocator, control);
        h2_lvgl_memory_release();
        return H2_PAL_ERR_INVALID_ARG;
    }
    int rc = add_chunk(s_memory.initial_bytes);
    if (rc != H2_PAL_OK) h2_lvgl_memory_release();
    return rc;
}

int h2_lvgl_memory_init(const h2_lvgl_platform_config_t *config, int threaded) {
    if (s_memory.allocator != NULL) return H2_PAL_ERR_INVALID_STATE;
    if (config->pool_initial_bytes != 0u) {
        size_t grow = config->pool_grow_bytes != 0u ?
            config->pool_grow_bytes : config->pool_initial_bytes;
        if (config->pool_initial_bytes < 256u || grow < 256u ||
            config->pool_initial_bytes > tlsf_block_size_max() / 2u ||
            grow > tlsf_block_size_max() / 2u ||
            config->allocator->vtable == NULL ||
            config->allocator->vtable->alloc == NULL ||
            config->allocator->vtable->realloc == NULL ||
            config->allocator->vtable->free == NULL)
            return H2_PAL_ERR_INVALID_ARG;
        if (threaded && (config->sync_api->vtable == NULL ||
            config->sync_api->vtable->create_mutex == NULL ||
            config->sync_api->vtable->destroy_mutex == NULL ||
            config->sync_api->vtable->lock_mutex == NULL ||
            config->sync_api->vtable->unlock_mutex == NULL))
            return H2_PAL_ERR_UNSUPPORTED;
        s_memory.initial_bytes = config->pool_initial_bytes;
        s_memory.grow_bytes = grow;
        s_memory.threaded = threaded;
    }
    s_memory.allocator = config->allocator;
    s_memory.sync = config->sync_api;
    int rc = h2_lvgl_memory_prepare();
    if (rc != H2_PAL_OK) h2_lvgl_memory_deinit();
    return rc;
}

static int is_pooled(const void *ptr) {
    const uintptr_t address = (uintptr_t)ptr;
    for (const h2_lvgl_chunk_t *chunk = s_memory.chunks;
         chunk != NULL; chunk = chunk->info.next) {
        const uintptr_t start = (uintptr_t)chunk->info.pool;
        if (address >= start && address - start < chunk->info.bytes) return 1;
    }
    return 0;
}

static void *alloc_locked(size_t size) {
    if (size == 0u) return NULL;
    if (size <= s_memory.grow_bytes / 2u) {
        void *ptr = tlsf_malloc(s_memory.tlsf, pooled_size(size));
        if (ptr == NULL && add_chunk(s_memory.grow_bytes) == H2_PAL_OK)
            ptr = tlsf_malloc(s_memory.tlsf, pooled_size(size));
        return ptr;
    }
    if (size > SIZE_MAX - sizeof(h2_lvgl_direct_t)) return NULL;
    h2_lvgl_direct_t *direct = h2_pal_mem_alloc(
        s_memory.allocator, sizeof(*direct) + size);
    if (direct == NULL) return NULL;
    direct->info.bytes = size;
    direct->info.next = s_memory.direct;
    s_memory.direct = direct;
    return direct + 1;
}

static h2_lvgl_direct_t **find_direct(void *ptr) {
    h2_lvgl_direct_t **link = &s_memory.direct;
    while (*link != NULL && (void *)(*link + 1) != ptr)
        link = &(*link)->info.next;
    return link;
}

static void free_locked(void *ptr) {
    if (ptr == NULL) return;
    if (is_pooled(ptr)) {
        tlsf_free(s_memory.tlsf, ptr);
    } else {
        h2_lvgl_direct_t **link = find_direct(ptr);
        if (*link != NULL) {
            h2_lvgl_direct_t *direct = *link;
            *link = direct->info.next;
            h2_pal_mem_free(s_memory.allocator, direct);
        }
    }
}

void *h2_lvgl_memory_alloc(size_t size) {
    if (s_memory.allocator == NULL) return NULL;
    if (s_memory.initial_bytes == 0u)
        return h2_pal_mem_alloc(s_memory.allocator, size);
    if (s_memory.tlsf == NULL || !memory_lock()) return NULL;
    void *ptr = alloc_locked(size);
    memory_unlock();
    return ptr;
}

void h2_lvgl_memory_free(void *ptr) {
    if (ptr == NULL || s_memory.allocator == NULL) return;
    if (s_memory.initial_bytes == 0u) {
        h2_pal_mem_free(s_memory.allocator, ptr);
        return;
    }
    if (s_memory.tlsf == NULL || !memory_lock()) return;
    free_locked(ptr);
    memory_unlock();
}

void *h2_lvgl_memory_realloc(void *ptr, size_t size) {
    if (s_memory.allocator == NULL) return NULL;
    if (ptr == NULL) return h2_lvgl_memory_alloc(size);
    if (s_memory.initial_bytes == 0u)
        return h2_pal_mem_realloc(s_memory.allocator, ptr, size);
    if (s_memory.tlsf == NULL || !memory_lock()) return NULL;
    void *resized = NULL;
    if (size == 0u) {
        free_locked(ptr);
    } else if (is_pooled(ptr)) {
        const size_t old_size = tlsf_block_size(ptr);
        if (size <= old_size) {
            /* Shrinking cannot move the pointer, preserving max alignment. */
            const size_t adjusted = pooled_size(size);
            resized = adjusted < old_size ?
                tlsf_realloc(s_memory.tlsf, ptr, adjusted) : ptr;
        } else {
            resized = alloc_locked(size);
            if (resized != NULL) {
                memcpy(resized, ptr, old_size);
                tlsf_free(s_memory.tlsf, ptr);
            }
        }
    } else if (size <= SIZE_MAX - sizeof(h2_lvgl_direct_t)) {
        h2_lvgl_direct_t **link = find_direct(ptr);
        if (*link != NULL) {
            h2_lvgl_direct_t *direct = h2_pal_mem_realloc(
                s_memory.allocator, *link, sizeof(*direct) + size);
            if (direct != NULL) {
                direct->info.bytes = size;
                *link = direct;
                resized = direct + 1;
            }
        }
    }
    memory_unlock();
    return resized;
}

static void collect_block(void *ptr, size_t size, int used, void *user) {
    (void)ptr;
    h2_lvgl_memory_stats_t *stats = user;
    if (used) stats->used_bytes += size;
    else if (size > stats->largest_free_bytes) stats->largest_free_bytes = size;
}

int h2_lvgl_platform_get_memory_stats(h2_lvgl_memory_stats_t *out_stats) {
    if (out_stats == NULL) return H2_PAL_ERR_INVALID_ARG;
    memset(out_stats, 0, sizeof(*out_stats));
    if (s_memory.tlsf == NULL) return H2_PAL_OK;
    if (!memory_lock()) return H2_PAL_ERR_INVALID_STATE;
    out_stats->chunk_bytes = s_memory.chunk_bytes;
    out_stats->control_bytes = tlsf_size();
    for (const h2_lvgl_chunk_t *chunk = s_memory.chunks;
         chunk != NULL; chunk = chunk->info.next) {
        ++out_stats->chunks;
        tlsf_walk_pool(chunk->info.pool, collect_block, out_stats);
    }
    for (const h2_lvgl_direct_t *direct = s_memory.direct;
         direct != NULL; direct = direct->info.next) {
        ++out_stats->direct_blocks;
        out_stats->direct_bytes += direct->info.bytes;
    }
    memory_unlock();
    return H2_PAL_OK;
}
