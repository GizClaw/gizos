#include "h2_web_platform_internal.h"

#include <emscripten.h>

/*
 * Browser Promises complete on the JS event loop. A completion only records
 * its result and requests a pump; the pump's external poll wakes the waiting
 * libco task. Task waiters therefore yield and every other task, timer and
 * the root keep running. Root callers (no task) fall back to Asyncify sleeps.
 */

void h2_web_async_begin(h2_web_platform_t *platform, h2_web_async_t *op) {
  *op = (h2_web_async_t){
      .result = H2_PAL_ERR_WOULD_BLOCK,
  };
  do {
    op->id = ++platform->async_next_id;
  } while (op->id == 0u);
  op->next = platform->async_ops;
  platform->async_ops = op;
}

static void h2_web_async_unlink(h2_web_platform_t *platform,
                                h2_web_async_t *op) {
  h2_web_async_t **cursor = &platform->async_ops;
  while (*cursor != NULL && *cursor != op)
    cursor = &(*cursor)->next;
  if (*cursor == op)
    *cursor = op->next;
  op->next = NULL;
}

void h2_web_async_signal(h2_web_platform_t *platform, h2_web_async_t *op,
                         int result) {
  if (op->done)
    return;
  op->done = true;
  op->result = result;
  platform->async_wake_pending = true;
  h2_web_platform_request_pump(platform, 0u);
}

/* Returns 1 when a waiter received the result, 0 when it was abandoned. */
EMSCRIPTEN_KEEPALIVE int h2_web_async_complete(uintptr_t platform_address,
                                               uint32_t id, int result) {
  h2_web_platform_t *platform = (h2_web_platform_t *)platform_address;
  if (platform == NULL || id == 0u)
    return 0;
  // Abandoned operations are no longer registered; late results are dropped.
  for (h2_web_async_t *op = platform->async_ops; op != NULL; op = op->next) {
    if (op->id == id) {
      if (op->done)
        return 0;
      h2_web_async_signal(platform, op, result);
      return 1;
    }
  }
  return 0;
}

void h2_web_platform_async_poll(h2_web_platform_t *platform,
                                h2_libco_t *executor) {
  if (!platform->async_wake_pending)
    return;
  platform->async_wake_pending = false;
  for (h2_web_async_t *op = platform->async_ops; op != NULL; op = op->next) {
    if (op->done && !op->woken) {
      op->woken = true;
      (void)h2_libco_wake(executor, (uintptr_t)op, H2_LIBCO_WAKE_ALL, NULL);
    }
  }
}

h2_pal_result_t h2_web_async_wait(h2_web_platform_t *platform,
                                  h2_web_async_t *op, uint32_t timeout_ms) {
  const bool forever = timeout_ms == H2_WEB_ASYNC_WAIT_FOREVER;
  const double deadline_ms = emscripten_get_now() + (double)timeout_ms;
  h2_pal_result_t result = H2_PAL_OK;
  ++platform->async_waiters;
  while (!op->done) {
    const double now_ms = emscripten_get_now();
    if (!forever && now_ms >= deadline_ms) {
      result = H2_PAL_ERR_TIMEOUT;
      break;
    }
    const uint32_t remaining =
        forever ? H2_LIBCO_WAIT_FOREVER
                : (uint32_t)(deadline_ms - now_ms) + 1u;
    const h2_libco_result_t wait =
        h2_libco_wait(platform->executor, (uintptr_t)op, remaining);
    if (wait == H2_LIBCO_ERR_INVALID_ARG) {
      // Not in a task: the root suspends through Asyncify.
      emscripten_sleep(1u);
    } else if (wait == H2_LIBCO_ERR_CANCELLED) {
      result = H2_PAL_ERR_CLOSED;
      break;
    } else if (wait != H2_LIBCO_WOKEN && wait != H2_LIBCO_ERR_TIMEOUT) {
      result = H2_PAL_ERR_INVALID_STATE;
      break;
    }
  }
  --platform->async_waiters;
  return op->done ? H2_PAL_OK : result;
}

void h2_web_async_end(h2_web_platform_t *platform, h2_web_async_t *op) {
  h2_web_async_unlink(platform, op);
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
  const h2_pal_result_t result = h2_pal_time_sleep_ms(
      h2_libco_time_api(platform->executor), duration_ms);
  if (result == H2_PAL_ERR_INVALID_STATE) {
    emscripten_sleep(duration_ms);
    return H2_PAL_OK;
  }
  return result;
}
