#include "h2_web_platform_internal.h"

#include <emscripten.h>
#include <errno.h>
#include <time.h>

/* The browser UI only publishes completion. The waiting Worker releases the
 * mutex atomically with its condition wait, so early completions are retained.
 */
void h2_web_async_begin(h2_web_platform_t *platform, h2_web_async_t *op) {
  pthread_mutex_lock(&platform->async_mutex);
  *op = (h2_web_async_t){
      .result = H2_PAL_ERR_WOULD_BLOCK};
  do {
    op->id = ++platform->async_next_id;
  } while (op->id == 0u);
  op->next = platform->async_ops;
  platform->async_ops = op;
  pthread_mutex_unlock(&platform->async_mutex);
}

void h2_web_async_signal(h2_web_platform_t *platform, h2_web_async_t *op,
                         int result) {
  pthread_mutex_lock(&platform->async_mutex);
  if (!op->done) {
    op->result = result;
    op->done = true;
    pthread_cond_broadcast(&platform->async_changed);
  }
  pthread_mutex_unlock(&platform->async_mutex);
}

EMSCRIPTEN_KEEPALIVE int h2_web_async_complete(uintptr_t address, uint32_t id,
                                               int result) {
  h2_web_platform_t *platform = (h2_web_platform_t *)address;
  if (platform == NULL || id == 0u)
    return 0;
  pthread_mutex_lock(&platform->async_mutex);
  int delivered = 0;
  for (h2_web_async_t *op = platform->async_ops; op != NULL; op = op->next) {
    if (op->id == id && !op->done) {
      op->result = result;
      op->done = true;
      delivered = 1;
      pthread_cond_broadcast(&platform->async_changed);
      break;
    }
  }
  pthread_mutex_unlock(&platform->async_mutex);
  return delivered;
}

h2_pal_result_t h2_web_async_wait(h2_web_platform_t *platform,
                                  h2_web_async_t *op, uint32_t timeout_ms) {
  const bool forever = timeout_ms == H2_WEB_ASYNC_WAIT_FOREVER;
  const double deadline = emscripten_get_now() + timeout_ms;
  h2_pal_result_t result = H2_PAL_OK;
  unsigned depth = h2_web_state_pause();
  pthread_mutex_lock(&platform->async_mutex);
  ++platform->async_waiters;
  while (!op->done) {
    if (h2_web_thread_core_is_current_task_cancelled(platform->executor)) {
      result = H2_PAL_ERR_CLOSED;
      break;
    }
    double remaining = deadline - emscripten_get_now();
    if (!forever && remaining <= 0) {
      result = H2_PAL_ERR_TIMEOUT;
      break;
    }
    /* Bounded waits also observe cooperative cancellation without killing a
     * thread while it owns C objects or a browser Promise. */
    uint32_t slice = forever || remaining > 10 ? 10u : (uint32_t)remaining + 1u;
    struct timespec wake;
    clock_gettime(CLOCK_REALTIME, &wake);
    wake.tv_nsec += (long)slice * 1000000L;
    wake.tv_sec += wake.tv_nsec / 1000000000L;
    wake.tv_nsec %= 1000000000L;
    pthread_cond_timedwait(&platform->async_changed, &platform->async_mutex,
                           &wake);
  }
  if (op->done)
    result = H2_PAL_OK;
  --platform->async_waiters;
  pthread_mutex_unlock(&platform->async_mutex);
  h2_web_state_resume(depth);
  return result;
}

void h2_web_async_end(h2_web_platform_t *platform,
                                h2_web_async_t *op) {
  pthread_mutex_lock(&platform->async_mutex);
  h2_web_async_t **cursor = &platform->async_ops;
  while (*cursor != NULL && *cursor != op)
    cursor = &(*cursor)->next;
  if (*cursor == op)
    *cursor = op->next;
  op->next = NULL;
  pthread_mutex_unlock(&platform->async_mutex);
}

int h2_web_async_finish(h2_web_platform_t *platform, h2_web_async_t *op,
                         int started) {
  int result = started;
  if (result == H2_PAL_OK) {
    result = h2_web_async_wait(platform, op, H2_WEB_ASYNC_WAIT_FOREVER);
    if (result == H2_PAL_OK)
      result = op->result;
  }
  h2_web_async_end(platform, op);
  return result;
}

h2_pal_result_t h2_web_platform_sleep_ms(h2_web_platform_t *platform,
                                         uint32_t duration_ms) {
  unsigned depth = h2_web_state_pause();
  h2_pal_result_t rc =
      h2_web_thread_core_sleep_ms(platform->executor, duration_ms);
  h2_web_state_resume(depth);
  return rc;
}
