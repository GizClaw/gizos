#include "h2_esp_platform_core.h"

#include <assert.h>
#include <stdint.h>

int h2_test_critical_entries;
uintptr_t h2_test_extram_low;
uintptr_t h2_test_extram_high;

static void mark_external(const void *address, size_t bytes) {
    h2_test_extram_low = (uintptr_t)address;
    h2_test_extram_high = h2_test_extram_low + bytes;
}

int main(void) {
    const h2_pal_atomic_api_t *api = h2_esp_platform_atomic_api();
    h2_pal_atomic_u32_t count = H2_PAL_ATOMIC_U32_INIT(1);
    h2_pal_atomic_flag_t flag = H2_PAL_ATOMIC_FLAG_INIT;
    uint32_t old = 0, expected = 2;
    bool changed = false, previous = false;
    assert(api != NULL);
    mark_external(&count, sizeof(count));
    assert(h2_pal_atomic_u32_fetch_add(api, &count, 1, &old, H2_PAL_ATOMIC_SEQ_CST) == H2_PAL_OK);
    assert(old == 1);
    assert(h2_pal_atomic_u32_compare_exchange(api, &count, &expected, 7, &changed,
        H2_PAL_ATOMIC_ACQ_REL, H2_PAL_ATOMIC_ACQUIRE) == H2_PAL_OK);
    assert(changed);
#if CONFIG_SPIRAM
    assert(h2_test_critical_entries == 2);
#else
    assert(h2_test_critical_entries == 0);
#endif
    mark_external(&flag, sizeof(flag));
    assert(h2_pal_atomic_flag_test_and_set(api, &flag, &previous, H2_PAL_ATOMIC_ACQUIRE) == H2_PAL_OK);
    assert(!previous);
    assert(h2_pal_atomic_flag_clear(api, &flag, H2_PAL_ATOMIC_RELEASE) == H2_PAL_OK);
#if CONFIG_SPIRAM
    assert(h2_test_critical_entries == 4);
#endif
    h2_test_extram_low = 0;
    h2_test_extram_high = 0;
    assert(h2_pal_atomic_u32_load(api, &count, &old, H2_PAL_ATOMIC_ACQUIRE) == H2_PAL_OK);
    assert(old == 7);
#if CONFIG_SPIRAM
    assert(h2_test_critical_entries == 4);
#else
    assert(h2_test_critical_entries == 0);
#endif
    return 0;
}
