#include <pthread.h>
#include <stdlib.h>

/* Mobile platforms use their native pthread implementation. The one lock
 * also orders operations on separate atomic values. */
static pthread_mutex_t s_h2_atomic_lock = PTHREAD_MUTEX_INITIALIZER;

typedef int h2_atomic_platform_lock_state_t;

static h2_atomic_platform_lock_state_t h2_atomic_platform_lock(void)
{
    if (pthread_mutex_lock(&s_h2_atomic_lock) != 0) abort();
    return 0;
}

static void h2_atomic_platform_unlock(h2_atomic_platform_lock_state_t state)
{
    (void)state;
    if (pthread_mutex_unlock(&s_h2_atomic_lock) != 0) abort();
}

#define H2_ATOMIC_PLATFORM_ALLOC(size) malloc(size)
#define H2_ATOMIC_PLATFORM_FREE(pointer) free(pointer)
#include "h2_atomic_locked_impl.h"
