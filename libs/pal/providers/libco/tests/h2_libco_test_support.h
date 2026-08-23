#ifndef H2_LIBCO_TEST_SUPPORT_H
#define H2_LIBCO_TEST_SUPPORT_H

#include "h2_libco.h"

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>

typedef struct h2_libco_test_env {
    uint64_t now_ms;
    size_t allocations;
} h2_libco_test_env_t;

static inline void *h2_libco_test_alloc(void *user, size_t size) {
    h2_libco_test_env_t *env = user;
    void *memory = malloc(size);
    if (memory != NULL) {
        ++env->allocations;
    }
    return memory;
}

static inline void h2_libco_test_free(void *user, void *memory) {
    h2_libco_test_env_t *env = user;
    assert(memory != NULL);
    assert(env->allocations != 0u);
    --env->allocations;
    free(memory);
}

static inline uint64_t h2_libco_test_now(void *user) {
    return ((h2_libco_test_env_t *)user)->now_ms;
}

static inline h2_libco_t *h2_libco_test_create(h2_libco_test_env_t *env) {
    h2_libco_t *core = NULL;
    const h2_libco_config_t config = {
        .user = env,
        .alloc = h2_libco_test_alloc,
        .free = h2_libco_test_free,
        .now_ms = h2_libco_test_now,
    };
    assert(h2_libco_create(&config, &core) == H2_LIBCO_OK);
    return core;
}

static inline void h2_libco_test_schedule(h2_libco_t *core, size_t expected) {
    size_t resumed = SIZE_MAX;
    assert(h2_libco_schedule(core, 64u, &resumed) == H2_LIBCO_OK);
    assert(resumed == expected);
}

static inline const h2_pal_mem_api_t *h2_libco_test_mem(
    h2_libco_test_env_t *env) {
    static h2_pal_mem_api_t api;
    static const h2_pal_mem_vtable_t vtable = {
        .alloc = h2_libco_test_alloc,
        .free = h2_libco_test_free,
    };
    api = (h2_pal_mem_api_t){.user = env, .vtable = &vtable};
    return &api;
}

#endif
