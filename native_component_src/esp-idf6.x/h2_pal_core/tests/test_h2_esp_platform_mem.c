#include "h2_esp_platform_core.h"
#include "esp_heap_caps.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* An SDK heap with rounded usable sizes and allocation failure, so the PAL
 * must preserve data, alignment and ownership across realloc transitions. */
static struct { void *raw, *ptr; size_t usable; } blocks[32];
static int fail_next;
static unsigned last_caps;
void *heap_caps_aligned_alloc(size_t alignment, size_t size, unsigned caps) {
    if (fail_next) { fail_next = 0; return NULL; }
    assert(alignment >= _Alignof(max_align_t));
    last_caps = caps;
    size_t usable = (size + 15u) & ~(size_t)15u;
    if (usable == 0) usable = 16u;
    void *raw = malloc(usable + alignment);
    assert(raw != NULL);
    void *ptr = (void *)(((uintptr_t)raw + alignment - 1u) & ~(uintptr_t)(alignment - 1u));
    for (unsigned i = 0; i < 32u; ++i) {
        if (blocks[i].ptr != NULL) continue;
        blocks[i].raw = raw; blocks[i].ptr = ptr; blocks[i].usable = usable;
        return ptr;
    }
    abort();
}
size_t heap_caps_get_allocated_size(void *ptr) {
    for (unsigned i = 0; i < 32u; ++i)
        if (blocks[i].ptr == ptr) return blocks[i].usable;
    abort();
}
void heap_caps_free(void *ptr) {
    if (ptr == NULL) return;
    for (unsigned i = 0; i < 32u; ++i) {
        if (blocks[i].ptr != ptr) continue;
        free(blocks[i].raw);
        memset(&blocks[i], 0, sizeof(blocks[i]));
        return;
    }
    abort();
}
static h2_esp_platform_resource_stats_t stats(void) {
    h2_esp_platform_resource_stats_t value;
    assert(h2_esp_platform_get_resource_stats(&value) == H2_PAL_OK);
    return value;
}
int main(void) {
    h2_pal_mem_api_t *apis[] = {
        h2_esp_platform_default_allocator(), h2_esp_platform_psram_allocator(),
        h2_esp_platform_internal_allocator(), h2_esp_platform_dma_allocator(),
    };
    const unsigned caps[] = {MALLOC_CAP_8BIT, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM,
        MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL, MALLOC_CAP_8BIT | MALLOC_CAP_DMA};
    assert(h2_esp_platform_get_resource_stats(NULL) == H2_PAL_ERR_INVALID_ARG);
    for (unsigned i = 0; i < 4u; ++i) {
        h2_pal_mem_api_t *api = apis[i];
        for (size_t size = 1u; size < 200u; size += 17u) {
            unsigned char *p = h2_pal_mem_alloc(api, size);
            assert(p != NULL && (uintptr_t)p % _Alignof(max_align_t) == 0);
            assert(last_caps == caps[i]);
            memset(p, 0x5a, size);
            fail_next = 1;
            assert(h2_pal_mem_realloc(api, p, size + 31u) == NULL);
            assert(stats().allocations == 1u);
            assert(stats().allocation_bytes == heap_caps_get_allocated_size(p));
            for (size_t k = 0; k < size; ++k) assert(p[k] == 0x5a);
            p = h2_pal_mem_realloc(api, p, size + 31u);
            assert(p != NULL && (uintptr_t)p % _Alignof(max_align_t) == 0);
            for (size_t k = 0; k < size; ++k) assert(p[k] == 0x5a);
            p = h2_pal_mem_realloc(api, p, 1u);
            assert(p != NULL && p[0] == 0x5a);
            assert((uintptr_t)p % _Alignof(max_align_t) == 0);
            assert(stats().allocations == 1u && stats().allocation_bytes == 16u);
            assert(h2_pal_mem_realloc(api, p, 0u) == NULL);
            assert(stats().allocations == 0u && stats().allocation_bytes == 0u);
        }
        fail_next = 1;
        assert(h2_pal_mem_alloc(api, 64u) == NULL);
        void *p = h2_pal_mem_realloc(api, NULL, 5u);
        assert(p != NULL);
        h2_pal_mem_free(api, p);
        h2_pal_mem_free(api, NULL);
        assert(stats().allocations == 0u && stats().allocation_bytes == 0u);
    }
    return 0;
}
