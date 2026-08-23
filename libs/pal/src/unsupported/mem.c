#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static void * unsupported_mem_alloc(void *p0, size_t p1) {
    (void)p0;
    (void)p1;
    return NULL;
}

static void * unsupported_mem_realloc(void *p0, void *p1, size_t p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return NULL;
}

static void unsupported_mem_free(void *p0, void *p1) {
    (void)p0;
    (void)p1;
}

static const h2_pal_mem_vtable_t unsupported_mem_vtable = {
    .alloc = unsupported_mem_alloc,
    .realloc = unsupported_mem_realloc,
    .free = unsupported_mem_free,
};
static const h2_pal_mem_api_t unsupported_mem_api = { .user = NULL, .vtable = &unsupported_mem_vtable };
const h2_pal_mem_api_t *h2_pal_unsupported_mem_api(void) { return &unsupported_mem_api; }
