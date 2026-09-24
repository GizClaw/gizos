#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <pthread.h>
#include <stddef.h>

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned s_allocations;

typedef int h2_atomic_platform_lock_state_t;
static h2_atomic_platform_lock_state_t h2_atomic_platform_lock(void) {
    assert(pthread_mutex_lock(&s_lock) == 0);
    return 0;
}
static void h2_atomic_platform_unlock(h2_atomic_platform_lock_state_t state) {
    (void)state;
    assert(pthread_mutex_unlock(&s_lock) == 0);
}
static void *failed_allocation(size_t size) {
    (void)size;
    ++s_allocations;
    return NULL;
}
static void ignored_free(void *pointer) { (void)pointer; }

#define H2_ATOMIC_PLATFORM_ALLOC(size) failed_allocation(size)
#define H2_ATOMIC_PLATFORM_FREE(pointer) ignored_free(pointer)
#include "h2_atomic_locked_impl.h"

int main(void) {
    h2_atomic_flag_t flag = {0};
    assert(h2_atomic_flag_init(&flag) == H2_ATOMIC_NO_MEMORY);
    assert(flag.storage == NULL);
    assert(s_allocations == 1u);
    h2_atomic_int_t unavailable = {0};
    assert(h2_atomic_int_init(&unavailable, 0) == H2_ATOMIC_NO_MEMORY);
    assert(s_allocations == 2u);
    return 0;
}
