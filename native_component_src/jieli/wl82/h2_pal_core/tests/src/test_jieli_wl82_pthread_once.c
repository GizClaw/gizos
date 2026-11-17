#include "h2_jieli_wl82_sdk_port.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

int __wrap_pthread_once(int* control, void (*initialize)(void));

static pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int owns_lock;
static int once_word, nested_word, runs, nested_runs, published;

void h2_jieli_sdk_atomic_lock(void) {
  assert(pthread_mutex_lock(&guard) == 0);
  assert(!owns_lock);
  owns_lock = 1;
}
void h2_jieli_sdk_atomic_unlock(void) {
  assert(owns_lock);
  owns_lock = 0;
  assert(pthread_mutex_unlock(&guard) == 0);
}
void h2_jieli_sdk_sleep_ms(uint32_t ms) {
  const struct timespec delay = {0, (long)ms * 1000000L};
  assert(!owns_lock);
  (void)nanosleep(&delay, NULL);
}
static void nested_initialize(void) {
  assert(!owns_lock);
  ++nested_runs;
}
static void initialize(void) {
  assert(!owns_lock);
  ++runs;
  assert(__wrap_pthread_once(&nested_word, nested_initialize) == 0);
  h2_jieli_sdk_sleep_ms(10u);
  published = 42;
}
static void* worker(void* unused) {
  (void)unused;
  for (unsigned i = 0; i < 1000u; ++i) {
    assert(__wrap_pthread_once(&once_word, initialize) == 0);
    assert(published == 42);
  }
  return NULL;
}
int main(void) {
  assert(__wrap_pthread_once(NULL, initialize) == EINVAL);
  assert(__wrap_pthread_once(&once_word, NULL) == EINVAL);
  assert(once_word == 0);
  pthread_t threads[8];
  for (unsigned i = 0; i < 8u; ++i)
    assert(pthread_create(&threads[i], NULL, worker, NULL) == 0);
  for (unsigned i = 0; i < 8u; ++i) assert(pthread_join(threads[i], NULL) == 0);
  assert(runs == 1 && nested_runs == 1 && once_word == 2 && nested_word == 2);
  return 0;
}
