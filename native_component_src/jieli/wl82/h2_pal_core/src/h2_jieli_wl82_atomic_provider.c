#include "h2_jieli_wl82_sdk_port.h"

/* The SDK testset spinlock masks local interrupts and excludes the other
 * core. Every access takes the same lock, providing at least the requested
 * ordering. Use the existing SDK heap; allocation is task-context only. */
typedef unsigned h2_atomic_platform_lock_state_t;
static h2_atomic_platform_lock_state_t h2_atomic_platform_lock(void) {
  h2_jieli_sdk_atomic_lock();
  return 0u;
}
static void h2_atomic_platform_unlock(h2_atomic_platform_lock_state_t state) {
  (void)state;
  h2_jieli_sdk_atomic_unlock();
}

#define H2_ATOMIC_PLATFORM_ALLOC(size) h2_jieli_sdk_malloc(size)
#define H2_ATOMIC_PLATFORM_FREE(pointer) h2_jieli_sdk_free(pointer)
#include "h2_atomic_locked_impl.h"
