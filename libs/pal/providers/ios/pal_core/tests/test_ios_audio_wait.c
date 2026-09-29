#if !defined(__APPLE__)
#define _POSIX_C_SOURCE 200809L
#endif
#include "h2_ios_audio_wait.h"
#include "h2/pal/core/h2_pal_errors.h"

#include <assert.h>
#include <stdio.h>
#include <time.h>

typedef struct noise {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  unsigned wakes;
  int stop;
} noise_t;

static uint64_t now_ns(void) {
  struct timespec now;
  assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static void *unrelated_callbacks(void *user) {
  noise_t *noise = user;
  /* Stop after one second even with the old resetting timeout. A broken
   * waiter then returns late and fails the elapsed-budget assertion. */
  for (unsigned i = 0u; i < 500u; ++i) {
    assert(pthread_mutex_lock(&noise->mutex) == 0);
    const int stop = noise->stop;
    ++noise->wakes;
    assert(pthread_cond_broadcast(&noise->changed) == 0);
    assert(pthread_mutex_unlock(&noise->mutex) == 0);
    if (stop) break;
    const struct timespec delay = {.tv_nsec = 2000000L};
    (void)nanosleep(&delay, NULL);
  }
  return NULL;
}

int main(void) {
  noise_t noise = {0};
  assert(pthread_mutex_init(&noise.mutex, NULL) == 0);
  assert(pthread_cond_init(&noise.changed, NULL) == 0);
  assert(pthread_mutex_lock(&noise.mutex) == 0);
  h2_ios_audio_wait_budget_t immediate;
  assert(h2_ios_audio_wait_begin(0u, &immediate) == H2_AUDIO_OK);
  assert(h2_ios_audio_wait_changed(&noise.changed, &noise.mutex, &immediate) ==
         H2_AUDIO_ERR_WOULD_BLOCK);
  h2_ios_audio_wait_budget_t budget;
  assert(h2_ios_audio_wait_begin(80u, &budget) == H2_AUDIO_OK);
  const uint64_t start = now_ns();
  pthread_t callbacks;
  assert(pthread_create(&callbacks, NULL, unrelated_callbacks, &noise) == 0);
  int rc;
  do {
    /* No callback ever changes the pending-output condition. */
    rc = h2_ios_audio_wait_changed(&noise.changed, &noise.mutex, &budget);
  } while (rc == H2_AUDIO_OK);
  const uint64_t elapsed_ms = (now_ns() - start) / UINT64_C(1000000);
  assert(rc == H2_AUDIO_ERR_WOULD_BLOCK);
  assert(elapsed_ms >= 60u && elapsed_ms < 500u);
  assert(noise.wakes >= 5u);
  noise.stop = 1;
  assert(pthread_mutex_unlock(&noise.mutex) == 0);
  assert(pthread_join(callbacks, NULL) == 0);
  assert(pthread_cond_destroy(&noise.changed) == 0);
  assert(pthread_mutex_destroy(&noise.mutex) == 0);
  printf("IOS_AUDIO_WAIT budget_ms=80 elapsed_ms=%llu unrelated_wakes=%u PASS\n",
         (unsigned long long)elapsed_ms, noise.wakes);
  return 0;
}
