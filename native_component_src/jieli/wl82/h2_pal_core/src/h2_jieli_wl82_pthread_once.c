#include "system/os/pthread.h"
#include "h2_jieli_wl82_sdk_port.h"

#include <errno.h>

/* The pinned SDK's pthread_once unconditionally asserts. Compiler-rt's emutls
 * calls it on the first _Thread_local access (including the KCP allocator).
 * The board layout wraps only this missing SDK operation at link time.
 * Its ABI uses an int control word initialized to zero. SDK threads do not
 * support POSIX cancellation; each successful initializer runs to completion.
 */
_Static_assert(sizeof(pthread_once_t) == sizeof(int),
               "JieLi pthread_once control ABI changed");
int __wrap_pthread_once(pthread_once_t* control, void (*initialize)(void)) {
  if (control == NULL || initialize == NULL) return EINVAL;
  for (;;) {
    h2_jieli_sdk_atomic_lock();
    if (*control == 2) {
      h2_jieli_sdk_atomic_unlock();
      return 0;
    }
    if (*control == 0) {
      *control = 1;
      h2_jieli_sdk_atomic_unlock();
      /* Initialization can allocate, block, or initialize another once word.
       * Never run it with interrupts masked or while holding the spinlock. */
      initialize();
      h2_jieli_sdk_atomic_lock();
      *control = 2;
      h2_jieli_sdk_atomic_unlock();
      return 0;
    }
    h2_jieli_sdk_atomic_unlock();
    h2_jieli_sdk_sleep_ms(1u);
  }
}
