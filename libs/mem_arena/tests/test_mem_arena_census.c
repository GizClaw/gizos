#include "h2_mem_arena_census.h"

#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

extern void *h2_mem_arena_census_test_alloc(h2_mem_arena_census_t *, size_t,
                                              size_t, uintptr_t, uintptr_t);
extern bool h2_mem_arena_census_test_is_tracked(h2_mem_arena_census_t *,
                                                 const void *);

struct h2_pal_mutex { pthread_mutex_t native; };

static size_t metadata_live;
static size_t allocation_calls, fail_allocation;
static size_t mutex_calls, fail_mutex;
static pthread_mutex_t arena_mutex = PTHREAD_MUTEX_INITIALIZER;

static void *meta_alloc(void *user, size_t bytes) {
    (void)user;
    if (++allocation_calls == fail_allocation)
        return NULL;
    void *ptr = malloc(bytes);
    if (ptr != NULL)
        ++metadata_live;
    return ptr;
}
static void meta_free(void *user, void *ptr) {
    (void)user;
    --metadata_live;
    free(ptr);
}
static const h2_pal_mem_vtable_t k_meta_vtable = {
    .alloc = meta_alloc, .free = meta_free};
static const h2_pal_mem_api_t k_metadata = {.vtable = &k_meta_vtable};

static h2_pal_result_t mutex_create(void *user,
                                     const h2_pal_mutex_config_t *config,
                                     h2_pal_mutex_t **out) {
    (void)user;
    *out = NULL;
    if (++mutex_calls == fail_mutex)
        return H2_PAL_ERR_NO_MEMORY;
    *out = h2_pal_mem_alloc(config->allocator, sizeof(**out));
    if (*out == NULL)
        return H2_PAL_ERR_NO_MEMORY;
    assert(pthread_mutex_init(&(*out)->native, NULL) == 0);
    return H2_PAL_OK;
}
static h2_pal_result_t mutex_destroy(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    assert(pthread_mutex_destroy(&mutex->native) == 0);
    h2_pal_mem_free(&k_metadata, mutex);
    return H2_PAL_OK;
}
static h2_pal_result_t mutex_lock(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    assert(pthread_mutex_lock(&mutex->native) == 0);
    return H2_PAL_OK;
}
static h2_pal_result_t mutex_unlock(void *user, h2_pal_mutex_t *mutex) {
    (void)user;
    assert(pthread_mutex_unlock(&mutex->native) == 0);
    return H2_PAL_OK;
}
static const h2_pal_sync_vtable_t k_sync_vtable = {
    .create_mutex = mutex_create, .destroy_mutex = mutex_destroy,
    .lock_mutex = mutex_lock, .unlock_mutex = mutex_unlock};
static const h2_pal_sync_api_t k_sync = {.vtable = &k_sync_vtable};

static void arena_lock(void *user) {
    assert(user == &arena_mutex);
    assert(pthread_mutex_lock(&arena_mutex) == 0);
}
static void arena_unlock(void *user) {
    assert(user == &arena_mutex);
    assert(pthread_mutex_unlock(&arena_mutex) == 0);
}

static const h2_mem_arena_census_tag_config_t k_tags[] = {
    {.name = "other", .track_sites = false},
    {.name = "service", .track_sites = true},
    {.name = "audio", .track_sites = true},
};

typedef struct fixture {
    void *region;
    h2_mem_arena_t *arena;
    h2_mem_arena_census_t *census;
    h2_mem_arena_census_config_t config;
    h2_mem_arena_census_counts_t service;
    h2_mem_arena_census_counts_t unattributed;
    h2_mem_arena_census_counts_t overflow;
    size_t failures, block_overflow, site_overflow, metadata_bytes;
    size_t snapshots;
    bool reenter;
    bool concurrent;
} fixture_t;

static void observe(void *user, const h2_mem_arena_census_snapshot_t *s) {
    fixture_t *f = user;
    if (!f->concurrent) {
        assert(pthread_mutex_trylock(&arena_mutex) == 0);
        assert(pthread_mutex_unlock(&arena_mutex) == 0);
    }
    assert(s->tag_count == 3u && s->site_capacity == 4u);
    assert(strcmp(s->tags[1].name, "service") == 0);
    f->service = s->tags[1].counts;
    f->unattributed = s->sites[0].counts;
    f->overflow = s->unattributed_overflow;
    f->failures = s->allocation_failures;
    f->block_overflow = s->block_overflow;
    f->site_overflow = s->site_overflow;
    f->metadata_bytes = s->metadata_bytes;
    ++f->snapshots;
    if (f->reenter) {
        const h2_pal_mem_api_t *audio =
            h2_mem_arena_census_tag_mem(f->census, 2u);
        void *ptr = h2_pal_mem_alloc(audio, 1u);
        assert(ptr != NULL);
        h2_pal_mem_free(audio, ptr);
    }
}

static void snapshot(fixture_t *f) {
    assert(h2_mem_arena_census_snapshot(f->census, observe, f) == H2_PAL_OK);
}

static void create(fixture_t *f) {
    memset(f, 0, sizeof(*f));
    f->region = malloc(1024u * 1024u);
    assert(f->region != NULL);
    const h2_mem_arena_config_t arena_config = {
        .name = "census-test", .block = f->region,
        .block_bytes = 1024u * 1024u, .small_request_max = 32768u,
        .small_pool_bytes = 256u * 1024u,
        .lock = arena_lock, .unlock = arena_unlock,
        .lock_user = &arena_mutex,
    };
    assert(h2_mem_arena_create(&arena_config, &f->arena) == H2_PAL_OK);
    f->config = (h2_mem_arena_census_config_t){
        .arena = f->arena, .metadata = &k_metadata, .sync = &k_sync,
        .tags = k_tags, .tag_count = 3u, .block_capacity = 8u,
        .site_capacity = 4u, .probe_limit = 2u,
    };
    h2_mem_arena_census_t *out = (void *)1;
    h2_mem_arena_census_config_t invalid = f->config;
    invalid.probe_limit = 5u;
    assert(h2_mem_arena_census_create(&invalid, &out) == H2_PAL_ERR_INVALID_ARG);
    assert(out == NULL);
    invalid = f->config;
    invalid.sync = NULL;
    assert(h2_mem_arena_census_create(&invalid, &out) == H2_PAL_ERR_UNSUPPORTED);
    assert(out == NULL);
    const h2_mem_arena_census_tag_config_t duplicate_tags[] = {
        {.name = "same"}, {.name = "same"}};
    invalid = f->config;
    invalid.tags = duplicate_tags;
    invalid.tag_count = 2u;
    assert(h2_mem_arena_census_create(&invalid, &out) == H2_PAL_ERR_INVALID_ARG);
    assert(out == NULL);
    fail_allocation = allocation_calls + 3u;
    assert(h2_mem_arena_census_create(&f->config, &out) ==
           H2_PAL_ERR_NO_MEMORY);
    assert(out == NULL && metadata_live == 0u);
    fail_allocation = 0u;
    fail_mutex = mutex_calls + 2u;
    assert(h2_mem_arena_census_create(&f->config, &out) ==
           H2_PAL_ERR_NO_MEMORY);
    assert(out == NULL && metadata_live == 0u);
    fail_mutex = 0u;
    assert(h2_mem_arena_census_create(&f->config, &f->census) == H2_PAL_OK);
    assert(f->census != NULL && metadata_live == 9u);
    assert(h2_mem_arena_census_tag_mem(f->census, 3u) == NULL);
}

static void finish(fixture_t *f) {
    snapshot(f);
    assert(f->service.blocks == 0u && f->unattributed.blocks == 0u);
    assert(h2_mem_arena_census_destroy(f->census) == H2_PAL_OK);
    assert(metadata_live == 0u);
    assert(h2_mem_arena_destroy(f->arena) == H2_PAL_OK);
    free(f->region);
}

static void test_accounting(void) {
    fixture_t f;
    create(&f);
    const h2_pal_mem_api_t *service =
        h2_mem_arena_census_tag_mem(f.census, 1u);
    unsigned char *ptr = h2_pal_mem_alloc(service, 32768u);
    assert(ptr != NULL);
    assert(metadata_live == 9u);
    memset(ptr, 0xa5, 32768u);
    assert(h2_mem_arena_census_destroy(f.census) == H2_PAL_ERR_INVALID_STATE);
    f.reenter = true;
    snapshot(&f);
    assert(f.service.live_bytes == 32768u && f.service.small_live_bytes == 32768u);
    assert(metadata_live == 9u);
    assert(f.service.blocks == 1u && f.metadata_bytes != 0u);
    assert(h2_pal_mem_realloc(service, ptr, SIZE_MAX) == NULL);
    snapshot(&f);
    assert(f.service.live_bytes == 32768u && f.failures == 1u);
    ptr = h2_pal_mem_realloc(service, ptr, 32769u);
    assert(ptr != NULL);
    for (size_t i = 0u; i < 32768u; ++i)
        assert(ptr[i] == 0xa5);
    snapshot(&f);
    assert(f.service.live_bytes == 32769u && f.service.small_live_bytes == 0u);
    h2_pal_mem_free(service, ptr);
    assert(h2_pal_mem_alloc(service, 0u) == NULL);
    finish(&f);
}

static void test_overflow(void) {
    fixture_t f;
    create(&f);
    const h2_pal_mem_api_t *service =
        h2_mem_arena_census_tag_mem(f.census, 1u);
    const h2_pal_mem_api_t *audio =
        h2_mem_arena_census_tag_mem(f.census, 2u);
    void *blocks[32];
    for (size_t i = 0u; i < 32u; ++i) {
        blocks[i] = h2_mem_arena_census_test_alloc(f.census, 1u, 64u,
                                                      (uintptr_t)(i + 1u), 0x200u);
        assert(blocks[i] != NULL);
    }
    snapshot(&f);
    assert(f.service.live_bytes + f.overflow.live_bytes == 32u * 64u);
    assert(f.service.blocks + f.overflow.blocks == 32u);
    assert(f.site_overflow > 0u && f.block_overflow > 0u);
    /* Site zero also includes tracked blocks whose caller site overflowed. */
    assert(f.unattributed.live_bytes >= f.overflow.live_bytes);
    assert(f.overflow.blocks > 0u);
    assert(h2_mem_arena_census_destroy(f.census) == H2_PAL_ERR_INVALID_STATE);
    size_t overflow_index = 0u;
    while (overflow_index < 32u &&
           h2_mem_arena_census_test_is_tracked(f.census, blocks[overflow_index]))
        ++overflow_index;
    assert(overflow_index < 32u);
    memset(blocks[overflow_index], 0x6d, 64u);
    assert(h2_pal_mem_realloc(audio, blocks[overflow_index], SIZE_MAX) == NULL);
    snapshot(&f);
    assert(f.service.live_bytes + f.overflow.live_bytes == 32u * 64u);
    assert(f.overflow.blocks > 0u);
    for (size_t j = 0u; j < 64u; ++j)
        assert(((unsigned char *)blocks[overflow_index])[j] == 0x6d);
    for (size_t i = 0u; i < 32u; ++i) {
        if (i % 2u == 0u) {
            memset(blocks[i], 0xa5, 64u);
            /* Deliberately use another view, including for overflowed blocks. */
            void *next = h2_pal_mem_realloc(audio, blocks[i], 128u);
            assert(next != NULL);
            for (size_t j = 0u; j < 64u; ++j)
                assert(((unsigned char *)next)[j] == 0xa5);
            blocks[i] = next;
        }
        h2_pal_mem_free(i % 2u == 0u ? service : audio, blocks[i]);
    }
    snapshot(&f);
    assert(f.service.live_bytes == 0u && f.overflow.live_bytes == 0u &&
           f.unattributed.live_bytes == 0u);
    finish(&f);
}

typedef struct worker {
    const h2_pal_mem_api_t *mem;
} worker_t;

static void *allocation_worker(void *user) {
    const worker_t *worker = user;
    for (size_t i = 0u; i < 500u; ++i) {
        unsigned char *ptr = h2_pal_mem_alloc(worker->mem, 64u);
        assert(ptr != NULL);
        memset(ptr, (int)(i & 0xffu), 64u);
        if (i % 2u == 0u) {
            ptr = h2_pal_mem_realloc(worker->mem, ptr, 96u);
            assert(ptr != NULL);
        }
        h2_pal_mem_free(worker->mem, ptr);
    }
    return NULL;
}

static void test_concurrent_snapshot(void) {
    fixture_t f;
    create(&f);
    f.concurrent = true;
    pthread_t threads[4];
    const worker_t workers[4] = {
        {.mem = h2_mem_arena_census_tag_mem(f.census, 1u)},
        {.mem = h2_mem_arena_census_tag_mem(f.census, 2u)},
        {.mem = h2_mem_arena_census_tag_mem(f.census, 1u)},
        {.mem = h2_mem_arena_census_tag_mem(f.census, 2u)},
    };
    for (size_t i = 0u; i < 4u; ++i)
        assert(pthread_create(&threads[i], NULL, allocation_worker,
                              (void *)&workers[i]) == 0);
    for (size_t i = 0u; i < 100u; ++i)
        snapshot(&f);
    for (size_t i = 0u; i < 4u; ++i)
        assert(pthread_join(threads[i], NULL) == 0);
    snapshot(&f);
    assert(f.service.live_bytes == 0u);
    finish(&f);
}

int main(void) {
    test_accounting();
    test_overflow();
    test_concurrent_snapshot();
    return 0;
}
