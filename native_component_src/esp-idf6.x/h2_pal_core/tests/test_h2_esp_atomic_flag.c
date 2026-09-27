#include "h2_atomic.h"
#include "esp_heap_caps.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

static unsigned s_allocations;
static unsigned s_frees;
static bool s_fail_next;

void *heap_caps_malloc(size_t size, unsigned int caps) {
    assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    ++s_allocations;
    if (s_fail_next) {
        s_fail_next = false;
        return NULL;
    }
    return malloc(size);
}

void heap_caps_free(void *pointer) {
    ++s_frees;
    free(pointer);
}

int main(void) {
    h2_atomic_flag_t first = {0};
    h2_atomic_flag_t second = {0};
    assert(h2_atomic_flag_init(&first) == H2_ATOMIC_OK);
    assert(h2_atomic_flag_init(&second) == H2_ATOMIC_OK);
    assert(first.storage != second.storage);
    assert(((uintptr_t)first.storage % _Alignof(uint32_t)) == 0u);
    assert(((uintptr_t)second.storage % _Alignof(uint32_t)) == 0u);
    assert(s_allocations == 2u);
    assert(!h2_atomic_flag_test_and_set(&first, H2_ATOMIC_ACQUIRE));
    assert(h2_atomic_flag_test_and_set(&first, H2_ATOMIC_ACQUIRE));
    assert(!h2_atomic_flag_test_and_set(&second, H2_ATOMIC_ACQUIRE));
    h2_atomic_flag_clear(&first, H2_ATOMIC_RELEASE);
    assert(!h2_atomic_flag_test_and_set(&first, H2_ATOMIC_ACQUIRE));
    h2_atomic_flag_destroy(&first);
    h2_atomic_flag_destroy(&second);
    assert(s_frees == 2u);
    h2_atomic_flag_t failed = {0};
    s_fail_next = true;
    assert(h2_atomic_flag_init(&failed) == H2_ATOMIC_NO_MEMORY);
    assert(failed.storage == NULL);
    h2_atomic_flag_destroy(&failed);
    return 0;
}
