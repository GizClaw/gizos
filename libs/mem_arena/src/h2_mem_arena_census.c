#include "h2_mem_arena_census.h"

#include <stdlib.h>
#include <string.h>

typedef struct census_block {
    void *ptr;
    size_t tag;
    size_t site;
} census_block_t;

typedef struct census_view {
    h2_pal_mem_api_t api;
    struct h2_mem_arena_census *census;
    size_t tag;
} census_view_t;

struct h2_mem_arena_census {
    h2_mem_arena_t *arena;
    const h2_pal_mem_api_t *inner;
    const h2_pal_mem_api_t *metadata;
    const h2_pal_sync_api_t *sync;
    const h2_mem_arena_census_tag_config_t *tag_config;
    h2_mem_arena_census_capture_site_fn capture_site;
    void *capture_site_user;
    h2_pal_mutex_t *lock;
    h2_pal_mutex_t *snapshot_lock;
    census_view_t *views;
    census_block_t *blocks;
    h2_mem_arena_census_tag_snapshot_t *live_tags;
    h2_mem_arena_census_tag_snapshot_t *copied_tags;
    h2_mem_arena_census_site_snapshot_t *live_sites;
    h2_mem_arena_census_site_snapshot_t *copied_sites;
    size_t tag_count;
    size_t block_capacity;
    size_t site_capacity;
    size_t probe_limit;
    size_t site_overflow;
    size_t block_overflow;
    h2_mem_arena_census_counts_t unattributed_overflow;
    size_t allocation_failures;
    size_t metadata_bytes;
};

static void census_lock(h2_mem_arena_census_t *c, h2_pal_mutex_t *mutex) {
    if (h2_pal_mutex_lock(c->sync, mutex) != H2_PAL_OK)
        abort();
}

static void census_unlock(h2_mem_arena_census_t *c, h2_pal_mutex_t *mutex) {
    if (h2_pal_mutex_unlock(c->sync, mutex) != H2_PAL_OK)
        abort();
}

static size_t hash_address(uintptr_t key) {
    key ^= key >> 16u;
    key *= (uintptr_t)0x45d9f3bu;
    return (size_t)(key ^ (key >> 16u));
}

/* The entire fixed probe window is searched, even across deleted slots. */
static census_block_t *find_block(h2_mem_arena_census_t *c, const void *ptr,
                                   bool empty) {
    const size_t start = hash_address((uintptr_t)ptr);
    for (size_t i = 0u; i < c->probe_limit; ++i) {
        census_block_t *b = &c->blocks[(start + i) % c->block_capacity];
        if (empty ? b->ptr == NULL : b->ptr == ptr)
            return b;
    }
    return NULL;
}

static size_t find_site(h2_mem_arena_census_t *c, size_t tag,
                        uintptr_t caller, uintptr_t outer) {
    if (caller == 0u && outer == 0u)
        return 0u;
    const size_t start = hash_address(caller ^ (outer << 1u) ^ (uintptr_t)tag);
    for (size_t i = 0u; i < c->probe_limit; ++i) {
        const size_t index = 1u + (start + i) % (c->site_capacity - 1u);
        h2_mem_arena_census_site_snapshot_t *site = &c->live_sites[index];
        if (!site->used) {
            site->used = true;
            site->tag_index = tag;
            site->caller = caller;
            site->outer = outer;
        }
        if (site->tag_index == tag && site->caller == caller &&
            site->outer == outer)
            return index;
    }
    ++c->site_overflow;
    return 0u;
}

static void add_counts(h2_mem_arena_census_counts_t *counts,
                       const h2_mem_arena_block_info_t *info) {
    counts->live_bytes += info->requested_bytes;
    ++counts->blocks;
    if (counts->live_bytes > counts->peak_bytes)
        counts->peak_bytes = counts->live_bytes;
    if (info->pool == 0u && !info->fallback) {
        counts->small_live_bytes += info->requested_bytes;
        ++counts->small_blocks;
        if (counts->small_live_bytes > counts->small_peak_bytes)
            counts->small_peak_bytes = counts->small_live_bytes;
    }
}

static void remove_counts(h2_mem_arena_census_counts_t *counts,
                          const h2_mem_arena_block_info_t *info) {
    counts->live_bytes -= info->requested_bytes;
    --counts->blocks;
    if (info->pool == 0u && !info->fallback) {
        counts->small_live_bytes -= info->requested_bytes;
        --counts->small_blocks;
    }
}

static h2_mem_arena_block_info_t block_info(h2_mem_arena_census_t *c,
                                             const void *ptr) {
    h2_mem_arena_block_info_t info = {0};
    /* The configured inner PAL must be backed by this arena. A violation here
     * would corrupt accounting and cannot be recovered from a void free. */
    if (h2_mem_arena_block_info(c->arena, ptr, &info) != H2_PAL_OK)
        abort();
    return info;
}

static void record(h2_mem_arena_census_t *c, size_t tag, void *ptr,
                    const h2_mem_arena_block_info_t *info,
                    uintptr_t caller, uintptr_t outer) {
    census_block_t *b = find_block(c, ptr, true);
    size_t site = 0u;
    if (c->tag_config[tag].track_sites && b != NULL)
        site = find_site(c, tag, caller, outer);
    if (b != NULL) {
        *b = (census_block_t){.ptr = ptr, .tag = tag, .site = site};
        add_counts(&c->live_tags[tag].counts, info);
        if (c->tag_config[tag].track_sites)
            add_counts(&c->live_sites[site].counts, info);
    } else {
        ++c->block_overflow;
        add_counts(&c->unattributed_overflow, info);
        /* The view that later frees this block need not be its original tag.
         * Charge site zero unconditionally so both counters remain balanced. */
        add_counts(&c->live_sites[0].counts, info);
    }
}

static void *alloc_at(census_view_t *view, size_t bytes, uintptr_t caller,
                       uintptr_t outer) {
    h2_mem_arena_census_t *c = view->census;
    void *ptr = h2_pal_mem_alloc(c->inner, bytes);
    h2_mem_arena_block_info_t info = {0};
    if (ptr != NULL)
        info = block_info(c, ptr);
    census_lock(c, c->lock);
    if (ptr != NULL)
        record(c, view->tag, ptr, &info, caller, outer);
    else if (bytes != 0u)
        ++c->allocation_failures;
    census_unlock(c, c->lock);
    return ptr;
}

static void census_free(void *user, void *ptr) {
    census_view_t *view = user;
    h2_mem_arena_census_t *c = view->census;
    if (ptr == NULL)
        return;
    census_lock(c, c->lock);
    const h2_mem_arena_block_info_t info = block_info(c, ptr);
    census_block_t *b = find_block(c, ptr, false);
    if (b != NULL) {
        const size_t tag = b->tag;
        const size_t site = b->site;
        b->ptr = NULL;
        remove_counts(&c->live_tags[tag].counts, &info);
        if (c->tag_config[tag].track_sites)
            remove_counts(&c->live_sites[site].counts, &info);
    } else {
        remove_counts(&c->unattributed_overflow, &info);
        remove_counts(&c->live_sites[0].counts, &info);
    }
    /* Keep pointer retirement and the underlying free in one transaction so
     * a snapshot cannot see an unowned block that is still reusable. */
    h2_pal_mem_free(c->inner, ptr);
    census_unlock(c, c->lock);
}

static void *realloc_at(census_view_t *view, void *ptr, size_t bytes,
                         uintptr_t caller, uintptr_t outer) {
    h2_mem_arena_census_t *c = view->census;
    if (ptr == NULL)
        return alloc_at(view, bytes, caller, outer);
    if (bytes == 0u) {
        census_free(view, ptr);
        return NULL;
    }
    census_lock(c, c->lock);
    const h2_mem_arena_block_info_t old_info = block_info(c, ptr);
    census_block_t *b = find_block(c, ptr, false);
    const size_t tag = b != NULL ? b->tag : view->tag;
    const size_t site = b != NULL ? b->site : 0u;
    void *next = h2_pal_mem_realloc(c->inner, ptr, bytes);
    h2_mem_arena_block_info_t new_info = {0};
    if (next != NULL)
        new_info = block_info(c, next);
    if (next == NULL) {
        ++c->allocation_failures;
    } else {
        if (b != NULL)
            b->ptr = NULL;
        if (b != NULL) {
            remove_counts(&c->live_tags[tag].counts, &old_info);
            if (c->tag_config[tag].track_sites)
                remove_counts(&c->live_sites[site].counts, &old_info);
        } else {
            remove_counts(&c->unattributed_overflow, &old_info);
            remove_counts(&c->live_sites[0].counts, &old_info);
        }
        record(c, tag, next, &new_info, caller, outer);
    }
    census_unlock(c, c->lock);
    return next;
}

static void capture_site(h2_mem_arena_census_t *c, size_t tag,
                         uintptr_t *caller, uintptr_t *outer) {
    *caller = 0u;
    *outer = 0u;
    if (!c->tag_config[tag].track_sites || c->capture_site == NULL)
        return;
    if (c->capture_site(c->capture_site_user, caller, outer) != H2_PAL_OK) {
        *caller = 0u;
        *outer = 0u;
    }
}

static void *census_alloc(void *user, size_t bytes) {
    census_view_t *view = user;
    uintptr_t caller = 0u, outer = 0u;
    capture_site(view->census, view->tag, &caller, &outer);
    return alloc_at(view, bytes, caller, outer);
}

static void *census_realloc(void *user, void *ptr,
                                                size_t bytes) {
    census_view_t *view = user;
    uintptr_t caller = 0u, outer = 0u;
    capture_site(view->census, view->tag, &caller, &outer);
    return realloc_at(view, ptr, bytes, caller, outer);
}

static const h2_pal_mem_vtable_t k_vtable = {
    .alloc = census_alloc, .realloc = census_realloc, .free = census_free};

static void release_metadata(h2_mem_arena_census_t *c) {
    if (c->snapshot_lock != NULL)
        (void)h2_pal_mutex_destroy(c->sync, c->snapshot_lock);
    if (c->lock != NULL)
        (void)h2_pal_mutex_destroy(c->sync, c->lock);
    h2_pal_mem_free(c->metadata, c->copied_sites);
    h2_pal_mem_free(c->metadata, c->live_sites);
    h2_pal_mem_free(c->metadata, c->copied_tags);
    h2_pal_mem_free(c->metadata, c->live_tags);
    h2_pal_mem_free(c->metadata, c->blocks);
    h2_pal_mem_free(c->metadata, c->views);
    h2_pal_mem_free(c->metadata, c);
}

static void *metadata_array(h2_mem_arena_census_t *c, size_t count,
                             size_t element_size) {
    if (count > SIZE_MAX / element_size)
        return NULL;
    const size_t bytes = count * element_size;
    if (bytes > SIZE_MAX - c->metadata_bytes)
        return NULL;
    void *ptr = h2_pal_mem_alloc(c->metadata, bytes);
    if (ptr != NULL) {
        memset(ptr, 0, bytes);
        c->metadata_bytes += bytes;
    }
    return ptr;
}

h2_pal_result_t h2_mem_arena_census_create(
    const h2_mem_arena_census_config_t *config,
    h2_mem_arena_census_t **out_census) {
    if (out_census == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    *out_census = NULL;
    if (config == NULL || config->arena == NULL || config->metadata == NULL ||
        config->metadata->vtable == NULL ||
        config->metadata->vtable->alloc == NULL ||
        config->metadata->vtable->free == NULL || config->tags == NULL ||
        config->tag_count == 0u || config->tag_count > UINT16_MAX ||
        config->block_capacity == 0u || config->site_capacity < 2u ||
        config->probe_limit == 0u ||
        config->probe_limit > config->block_capacity ||
        config->probe_limit > config->site_capacity - 1u)
        return H2_PAL_ERR_INVALID_ARG;
    if (config->sync == NULL || config->sync->vtable == NULL ||
        config->sync->vtable->create_mutex == NULL ||
        config->sync->vtable->destroy_mutex == NULL ||
        config->sync->vtable->lock_mutex == NULL ||
        config->sync->vtable->unlock_mutex == NULL)
        return H2_PAL_ERR_UNSUPPORTED;
    const h2_pal_mem_api_t *inner = config->inner != NULL
                                        ? config->inner
                                        : h2_mem_arena_mem(config->arena);
    if (inner == NULL || inner->vtable == NULL ||
        inner->vtable->alloc == NULL || inner->vtable->realloc == NULL ||
        inner->vtable->free == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    for (size_t i = 0u; i < config->tag_count; ++i) {
        if (config->tags[i].name == NULL || config->tags[i].name[0] == '\0')
            return H2_PAL_ERR_INVALID_ARG;
        for (size_t j = 0u; j < i; ++j)
            if (strcmp(config->tags[i].name, config->tags[j].name) == 0)
                return H2_PAL_ERR_INVALID_ARG;
    }
    h2_mem_arena_census_t *c = h2_pal_mem_alloc(config->metadata, sizeof(*c));
    if (c == NULL)
        return H2_PAL_ERR_NO_MEMORY;
    memset(c, 0, sizeof(*c));
    c->arena = config->arena;
    c->inner = inner;
    c->metadata = config->metadata;
    c->sync = config->sync;
    c->tag_config = config->tags;
    c->capture_site = config->capture_site;
    c->capture_site_user = config->capture_site_user;
    c->tag_count = config->tag_count;
    c->block_capacity = config->block_capacity;
    c->site_capacity = config->site_capacity;
    c->probe_limit = config->probe_limit;
    c->metadata_bytes = sizeof(*c);
    c->views = metadata_array(c, c->tag_count, sizeof(*c->views));
    c->blocks = metadata_array(c, c->block_capacity, sizeof(*c->blocks));
    c->live_tags = metadata_array(c, c->tag_count, sizeof(*c->live_tags));
    c->copied_tags = metadata_array(c, c->tag_count, sizeof(*c->copied_tags));
    c->live_sites = metadata_array(c, c->site_capacity, sizeof(*c->live_sites));
    c->copied_sites = metadata_array(c, c->site_capacity, sizeof(*c->copied_sites));
    if (c->inner == NULL || c->views == NULL || c->blocks == NULL ||
        c->live_tags == NULL || c->copied_tags == NULL ||
        c->live_sites == NULL || c->copied_sites == NULL) {
        release_metadata(c);
        return H2_PAL_ERR_NO_MEMORY;
    }
    h2_pal_mutex_config_t mutex_config = {
        .name = "arena/census", .allocator = c->metadata};
    h2_pal_result_t rc = h2_pal_mutex_create(c->sync, &mutex_config, &c->lock);
    if (rc == H2_PAL_OK) {
        mutex_config.name = "arena/census-snapshot";
        rc = h2_pal_mutex_create(c->sync, &mutex_config, &c->snapshot_lock);
    }
    if (rc != H2_PAL_OK) {
        release_metadata(c);
        return rc;
    }
    c->live_sites[0].used = true;
    c->live_sites[0].tag_index = SIZE_MAX;
    for (size_t i = 0u; i < c->tag_count; ++i) {
        c->views[i] = (census_view_t){.census = c, .tag = i};
        c->views[i].api = (h2_pal_mem_api_t){
            .user = &c->views[i], .vtable = &k_vtable};
        c->live_tags[i].name = c->tag_config[i].name;
    }
    *out_census = c;
    return H2_PAL_OK;
}

const h2_pal_mem_api_t *h2_mem_arena_census_tag_mem(
    h2_mem_arena_census_t *census, size_t tag_index) {
    return census != NULL && tag_index < census->tag_count
               ? &census->views[tag_index].api : NULL;
}

h2_pal_result_t h2_mem_arena_census_snapshot(
    h2_mem_arena_census_t *census, h2_mem_arena_census_visit_fn visit,
    void *user) {
    if (census == NULL || visit == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    census_lock(census, census->snapshot_lock);
    census_lock(census, census->lock);
    memcpy(census->copied_tags, census->live_tags,
           census->tag_count * sizeof(*census->copied_tags));
    memcpy(census->copied_sites, census->live_sites,
           census->site_capacity * sizeof(*census->copied_sites));
    const h2_mem_arena_census_snapshot_t snapshot = {
        .tags = census->copied_tags,
        .tag_count = census->tag_count,
        .sites = census->copied_sites,
        .site_capacity = census->site_capacity,
        .site_overflow = census->site_overflow,
        .block_overflow = census->block_overflow,
        .unattributed_overflow = census->unattributed_overflow,
        .allocation_failures = census->allocation_failures,
        .metadata_bytes = census->metadata_bytes,
    };
    census_unlock(census, census->lock);
    visit(user, &snapshot);
    census_unlock(census, census->snapshot_lock);
    return H2_PAL_OK;
}

h2_pal_result_t h2_mem_arena_census_destroy(h2_mem_arena_census_t *census) {
    if (census == NULL)
        return H2_PAL_OK;
    /* Callers have already stopped and joined users. Keep the instance and
     * its borrowed APIs valid if even one measured block is still live. */
    for (size_t i = 0u; i < census->tag_count; ++i)
        if (census->live_tags[i].counts.blocks != 0u)
            return H2_PAL_ERR_INVALID_STATE;
    if (census->unattributed_overflow.blocks != 0u)
        return H2_PAL_ERR_INVALID_STATE;
    release_metadata(census);
    return H2_PAL_OK;
}

#if defined(H2_MEM_ARENA_CENSUS_TESTING)
bool h2_mem_arena_census_test_is_tracked(h2_mem_arena_census_t *c,
                                          const void *ptr) {
    if (c == NULL || ptr == NULL)
        return false;
    census_lock(c, c->lock);
    const bool tracked = find_block(c, ptr, false) != NULL;
    census_unlock(c, c->lock);
    return tracked;
}

void *h2_mem_arena_census_test_alloc(h2_mem_arena_census_t *c, size_t tag,
                                      size_t bytes, uintptr_t caller,
                                      uintptr_t outer) {
    if (c == NULL || tag >= c->tag_count)
        return NULL;
    return alloc_at(&c->views[tag], bytes, caller, outer);
}
#endif
