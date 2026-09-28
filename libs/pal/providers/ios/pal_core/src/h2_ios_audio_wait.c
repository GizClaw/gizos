#if !defined(__APPLE__)
#define _POSIX_C_SOURCE 200809L
#endif
#include "h2_ios_audio_wait.h"

#include "h2/pal/core/h2_pal_errors.h"

#include <errno.h>
#include <time.h>

int h2_ios_audio_wait_begin(uint32_t timeout_ms,
                           h2_ios_audio_wait_budget_t *out_budget) {
  if (out_budget == NULL) return H2_AUDIO_ERR_INVALID_ARG;
  *out_budget = (h2_ios_audio_wait_budget_t){
      .infinite = timeout_ms == UINT32_MAX};
  if (out_budget->infinite) return H2_AUDIO_OK;
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return H2_AUDIO_ERR_IO;
  out_budget->deadline_ns = (uint64_t)now.tv_sec * UINT64_C(1000000000) +
      (uint64_t)now.tv_nsec + (uint64_t)timeout_ms * UINT64_C(1000000);
  return H2_AUDIO_OK;
}

int h2_ios_audio_wait_changed(pthread_cond_t *changed, pthread_mutex_t *mutex,
                             const h2_ios_audio_wait_budget_t *budget) {
  if (changed == NULL || mutex == NULL || budget == NULL)
    return H2_AUDIO_ERR_INVALID_ARG;
  if (budget->infinite)
    return pthread_cond_wait(changed, mutex) == 0 ? H2_AUDIO_OK : H2_AUDIO_ERR_IO;
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return H2_AUDIO_ERR_IO;
  const uint64_t now_ns = (uint64_t)now.tv_sec * UINT64_C(1000000000) +
      (uint64_t)now.tv_nsec;
  if (now_ns >= budget->deadline_ns) return H2_AUDIO_ERR_WOULD_BLOCK;
  const uint64_t remaining = budget->deadline_ns - now_ns;
#if defined(__APPLE__)
  const struct timespec relative = {
      .tv_sec = (time_t)(remaining / UINT64_C(1000000000)),
      .tv_nsec = (long)(remaining % UINT64_C(1000000000))};
  /* Darwin's relative wait avoids a wall-clock adjustment changing the
   * per-call monotonic budget. Every wake still uses the original deadline. */
  const int rc = pthread_cond_timedwait_relative_np(changed, mutex, &relative);
#else
  /* Host regression tests use the default realtime pthread condition clock. */
  if (clock_gettime(CLOCK_REALTIME, &now) != 0) return H2_AUDIO_ERR_IO;
  const uint64_t absolute_ns = (uint64_t)now.tv_sec * UINT64_C(1000000000) +
      (uint64_t)now.tv_nsec + remaining;
  const struct timespec absolute = {
      .tv_sec = (time_t)(absolute_ns / UINT64_C(1000000000)),
      .tv_nsec = (long)(absolute_ns % UINT64_C(1000000000))};
  const int rc = pthread_cond_timedwait(changed, mutex, &absolute);
#endif
  return rc == 0 ? H2_AUDIO_OK : rc == ETIMEDOUT
      ? H2_AUDIO_ERR_WOULD_BLOCK : H2_AUDIO_ERR_IO;
}
