#include "h2_jieli_wl82_platform_core.h"
#include "h2_jieli_wl82_sdk_port.h"

static void *mem_alloc(void *user, size_t length)
{
    (void)user;
    if (length == 0u) {
        return NULL;
    }
    return h2_jieli_sdk_malloc(length);
}

static void *mem_realloc(void *user, void *ptr, size_t length)
{
    (void)user;
    if (length == 0u) {
        h2_jieli_sdk_free(ptr);
        return NULL;
    }
    return h2_jieli_sdk_realloc(ptr, length);
}

static void mem_free(void *user, void *ptr)
{
    (void)user;
    if (ptr != NULL) {
        h2_jieli_sdk_free(ptr);
    }
}

static const h2_pal_mem_vtable_t s_mem_vtable = {
    .alloc = mem_alloc,
    .realloc = mem_realloc,
    .free = mem_free,
};

static const h2_pal_mem_api_t s_mem_api = {
    .user = NULL,
    .vtable = &s_mem_vtable,
};

const h2_pal_mem_api_t *h2_jieli_wl82_platform_mem_api(void)
{
    return &s_mem_api;
}
