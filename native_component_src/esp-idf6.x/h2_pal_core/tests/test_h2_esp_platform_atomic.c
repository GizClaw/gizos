#include "h2_esp_platform_core.h"
#include "esp_heap_caps.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

static unsigned allocations;
static unsigned frees;
static unsigned last_caps;
void *heap_caps_malloc(size_t size, unsigned caps) {
    ++allocations;
    last_caps = caps;
    return malloc(size);
}
void heap_caps_free(void *value) { ++frees; free(value); }

int main(void) {
    const h2_pal_atomic_api_t *api = h2_esp_platform_atomic_api();
    _Atomic uint32_t *slots[65] = {0};
    assert(api != NULL);
    for (unsigned n = 0; n < 64; ++n) {
        assert(h2_pal_atomic_alloc_u32(api, n, &slots[n]) == H2_PAL_OK);
        assert(atomic_load(slots[n]) == n);
    }
#if CONFIG_SPIRAM
    assert(allocations == 0);
#endif
    assert(h2_pal_atomic_alloc_u32(api, 64, &slots[64]) == H2_PAL_OK);
#if CONFIG_SPIRAM
    assert(allocations == 1);
    assert(last_caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
#endif
    assert(atomic_fetch_add(slots[64], 1) == 64);
    assert(h2_pal_atomic_free(api, slots[0]) == H2_PAL_OK);
    _Atomic uint32_t *reused = NULL;
    assert(h2_pal_atomic_alloc_u32(api, 99, &reused) == H2_PAL_OK);
#if CONFIG_SPIRAM
    assert(reused == slots[0] && allocations == 1);
#endif
    assert(atomic_load(reused) == 99);
    assert(h2_pal_atomic_free(api, reused) == H2_PAL_OK);
    for (unsigned n = 1; n < 65; ++n) assert(h2_pal_atomic_free(api, slots[n]) == H2_PAL_OK);
#if CONFIG_SPIRAM
    assert(frees == 1);
#endif
    _Atomic uint32_t *bad = NULL;
    assert(h2_pal_atomic_alloc_u32(api, 0, NULL) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_atomic_alloc_raw(api, 4, 3, (void **)&bad) == H2_PAL_ERR_INVALID_ARG);
    assert(bad == NULL);
    return 0;
}
