#ifndef H2_PAL_MEM_H
#define H2_PAL_MEM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_pal_mem_api h2_pal_mem_api_t;

typedef struct h2_pal_mem_vtable {
    void *(*alloc)(void *user, size_t len);
    void *(*realloc)(void *user, void *ptr, size_t len);
    void (*free)(void *user, void *ptr);
} h2_pal_mem_vtable_t;

struct h2_pal_mem_api {
    void *user;
    const h2_pal_mem_vtable_t *vtable;
};

static inline void *h2_pal_mem_alloc(const h2_pal_mem_api_t *api, size_t len) {
    if (api == NULL || api->vtable == NULL || api->vtable->alloc == NULL) {
        return NULL;
    }
    return api->vtable->alloc(api->user, len);
}

static inline void *h2_pal_mem_realloc(const h2_pal_mem_api_t *api, void *ptr, size_t len) {
    if (api == NULL || api->vtable == NULL || api->vtable->realloc == NULL) {
        return NULL;
    }
    return api->vtable->realloc(api->user, ptr, len);
}

static inline void h2_pal_mem_free(const h2_pal_mem_api_t *api, void *ptr) {
    if (api == NULL || api->vtable == NULL || api->vtable->free == NULL || ptr == NULL) {
        return;
    }
    api->vtable->free(api->user, ptr);
}

#ifdef __cplusplus
}
#endif

#endif
