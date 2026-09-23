#include "h2/pal/os/h2_pal_atomic.h"

#include <assert.h>

#ifndef H2_ATOMIC_PROVIDER_GETTER
#error H2_ATOMIC_PROVIDER_GETTER must name the provider API getter
#endif

extern const h2_pal_atomic_api_t *H2_ATOMIC_PROVIDER_GETTER(void);

int main(void) {
    const h2_pal_atomic_api_t *api = H2_ATOMIC_PROVIDER_GETTER();
    h2_pal_atomic_u32_t count = H2_PAL_ATOMIC_U32_INIT(0);
    h2_pal_atomic_i32_t signed_count = H2_PAL_ATOMIC_I32_INIT(-1);
    h2_pal_atomic_bool_t state = H2_PAL_ATOMIC_BOOL_INIT(false);
    h2_pal_atomic_ptr_t pointer = H2_PAL_ATOMIC_PTR_INIT(NULL);
    h2_pal_atomic_flag_t flag = H2_PAL_ATOMIC_FLAG_INIT;
    uint32_t old = 0, expected = 3;
    int signed_old = 0;
    bool changed = false, previous = false;
    int marker = 0;
    void *observed = NULL;
    assert(api != NULL && api->vtable != NULL);
    assert(h2_pal_atomic_u32_store(api, &count, 3, H2_PAL_ATOMIC_RELEASE) == H2_PAL_OK);
    assert(h2_pal_atomic_u32_compare_exchange(api, &count, &expected, 5,
        &changed, H2_PAL_ATOMIC_ACQ_REL, H2_PAL_ATOMIC_ACQUIRE) == H2_PAL_OK);
    assert(changed);
    assert(h2_pal_atomic_u32_fetch_add(api, &count, 2, &old,
        H2_PAL_ATOMIC_RELAXED) == H2_PAL_OK && old == 5);
    assert(h2_pal_atomic_u32_fetch_sub(api, &count, 1, &old,
        H2_PAL_ATOMIC_RELAXED) == H2_PAL_OK && old == 7);
    assert(h2_pal_atomic_i32_fetch_add(api, &signed_count, 2, &signed_old,
        H2_PAL_ATOMIC_SEQ_CST) == H2_PAL_OK && signed_old == -1);
    assert(h2_pal_atomic_bool_exchange(api, &state, true, &previous,
        H2_PAL_ATOMIC_SEQ_CST) == H2_PAL_OK && !previous);
    assert(h2_pal_atomic_ptr_store(api, &pointer, &marker,
        H2_PAL_ATOMIC_RELEASE) == H2_PAL_OK);
    assert(h2_pal_atomic_ptr_load(api, &pointer, &observed,
        H2_PAL_ATOMIC_ACQUIRE) == H2_PAL_OK && observed == &marker);
    assert(h2_pal_atomic_flag_test_and_set(api, &flag, &previous,
        H2_PAL_ATOMIC_ACQUIRE) == H2_PAL_OK && !previous);
    assert(h2_pal_atomic_flag_clear(api, &flag, H2_PAL_ATOMIC_RELEASE) == H2_PAL_OK);
    return 0;
}
