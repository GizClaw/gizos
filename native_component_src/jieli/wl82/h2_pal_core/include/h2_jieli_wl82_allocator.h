#ifndef H2_JIELI_WL82_ALLOCATOR_H
#define H2_JIELI_WL82_ALLOCATOR_H

#include "h2/pal/os/h2_pal_mem.h"
#include "h2_jieli_wl82_sdk_port.h"

/* PAL object ownership follows the allocator supplied at creation. SDK-native
 * handle storage remains owned by its SDK create/destroy pair. */
static inline void *h2_jieli_core_alloc(
    const h2_pal_mem_api_t *allocator, size_t bytes)
{
    return allocator != NULL ? h2_pal_mem_alloc(allocator, bytes)
                             : h2_jieli_sdk_malloc(bytes);
}

static inline void h2_jieli_core_free(
    const h2_pal_mem_api_t *allocator, void *pointer)
{
    if (allocator != NULL) {
        h2_pal_mem_free(allocator, pointer);
    } else {
        h2_jieli_sdk_free(pointer);
    }
}

#endif
