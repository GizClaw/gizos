/* Exercise the production provider with only its SDK dependencies replaced.
 * The native implementation uses an IRQ-masking cross-core SDK spinlock. */
#include "h2_jieli_wl82_sdk_port.h"

#include <assert.h>
#include <pthread.h>
#include <stdlib.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void* h2_jieli_sdk_malloc(size_t size) { return malloc(size); }
void h2_jieli_sdk_free(void* pointer) { free(pointer); }
void h2_jieli_sdk_atomic_lock(void) { assert(pthread_mutex_lock(&lock) == 0); }
void h2_jieli_sdk_atomic_unlock(void) {
  assert(pthread_mutex_unlock(&lock) == 0);
}
