#include "h2/pal/os/h2_pal_atomic.h"
#include <assert.h>

#ifndef H2_ATOMIC_PROVIDER_GETTER
#error H2_ATOMIC_PROVIDER_GETTER must name the platform provider
#endif
extern const h2_pal_atomic_api_t *H2_ATOMIC_PROVIDER_GETTER(void);

int main(void) {
    const h2_pal_atomic_api_t *api = H2_ATOMIC_PROVIDER_GETTER();
    _Atomic uint32_t *count = NULL;
    atomic_flag *flag = NULL;
    assert(api != NULL);
    assert(h2_pal_atomic_alloc_u32(api, 3, &count) == H2_PAL_OK);
    assert(atomic_fetch_add(count, 2) == 3);
    assert(atomic_load(count) == 5);
    assert(h2_pal_atomic_alloc_flag(api, &flag) == H2_PAL_OK);
    assert(!atomic_flag_test_and_set(flag));
    atomic_flag_clear(flag);
    assert(h2_pal_atomic_free(api, count) == H2_PAL_OK);
    assert(h2_pal_atomic_free(api, flag) == H2_PAL_OK);
    const size_t alignments[] = {
        _Alignof(_Atomic uint32_t), _Alignof(_Atomic int32_t),
        _Alignof(_Atomic bool), _Alignof(_Atomic(void *)), _Alignof(atomic_flag),
    };
    size_t maximum_alignment = 1;
    for (size_t n = 0; n < sizeof(alignments) / sizeof(alignments[0]); ++n)
        if (alignments[n] > maximum_alignment) maximum_alignment = alignments[n];
    void *aligned = NULL;
    assert(h2_pal_atomic_alloc_raw(api, 16, maximum_alignment, &aligned) == H2_PAL_OK);
    assert(((uintptr_t)aligned & (maximum_alignment - 1)) == 0);
    assert(h2_pal_atomic_free(api, aligned) == H2_PAL_OK);
    return 0;
}
