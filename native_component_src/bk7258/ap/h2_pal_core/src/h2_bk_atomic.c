#include <os/os.h>
#include <os/mem.h>
#include <stdint.h>

typedef uint32_t h2_atomic_platform_lock_state_t;
static h2_atomic_platform_lock_state_t h2_atomic_platform_lock(void) {
    return rtos_enter_critical();
}
static void h2_atomic_platform_unlock(h2_atomic_platform_lock_state_t state) {
    rtos_exit_critical(state);
}

#define H2_ATOMIC_PLATFORM_ALLOC(size) os_malloc(size)
#define H2_ATOMIC_PLATFORM_FREE(pointer) os_free(pointer)
#include "h2_atomic_locked_impl.h"
