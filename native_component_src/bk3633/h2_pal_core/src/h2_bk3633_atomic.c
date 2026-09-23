#include "arch.h"
#include <stddef.h>
#include <stdint.h>

extern void *h2_bk3633_atomic_alloc(size_t length);
extern void h2_bk3633_atomic_free(void *pointer);

typedef uint32_t h2_atomic_platform_lock_state_t;
static h2_atomic_platform_lock_state_t h2_atomic_platform_lock(void) {
    uint32_t state = __disable_fiq() != 0 ? 1u : 0u;
    if (__disable_irq() != 0) state |= 2u;
    return state;
}
static void h2_atomic_platform_unlock(h2_atomic_platform_lock_state_t state) {
    if ((state & 1u) == 0u) __enable_fiq();
    if ((state & 2u) == 0u) __enable_irq();
}

#define H2_ATOMIC_PLATFORM_ALLOC(size) h2_bk3633_atomic_alloc(size)
#define H2_ATOMIC_PLATFORM_FREE(pointer) h2_bk3633_atomic_free(pointer)
#include "h2_atomic_locked_impl.h"
