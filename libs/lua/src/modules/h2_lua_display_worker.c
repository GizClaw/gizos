#include "h2_lua_display_worker.h"

#include <string.h>

static h2_pal_result_t submit_rect(h2_lua_display_worker_t *worker,
                                   h2_lua_display_plan_rect_t rect) {
  h2_display_rect_t area = {rect.left, rect.top,
                            rect.right - rect.left, rect.bottom - rect.top};
  h2_pal_result_t result = (h2_pal_result_t)h2_pal_display_draw_bitmap(
      worker->runtime->display, &area,
      worker->pixels + (size_t)rect.top * worker->info.width + rect.left,
      (size_t)worker->info.width * sizeof(uint16_t), H2_DISPLAY_PIXEL_RGB565);
  if (result == H2_PAL_OK) {
    worker->sent_pixels += (size_t)area.width * area.height;
    ++worker->sent_rects;
  }
  return result;
}

static void execute(h2_lua_display_worker_t *worker) {
  const h2_pal_display_api_t *display = worker->runtime->display;
  h2_pal_result_t result = H2_PAL_OK;
  if (worker->operation == H2_LUA_DISPLAY_OPEN) {
    if (!worker->borrowed) result = (h2_pal_result_t)h2_pal_display_open(display);
    if (result == H2_PAL_OK) {
      worker->opened = 1;
      result = (h2_pal_result_t)h2_pal_display_get_info(display, &worker->info);
    }
  } else if (worker->operation == H2_LUA_DISPLAY_CLOSE) {
    if (worker->opened && !worker->borrowed)
      result = (h2_pal_result_t)h2_pal_display_close(display);
    if (result == H2_PAL_OK) worker->opened = 0;
  } else if (worker->operation == H2_LUA_DISPLAY_FRAME) {
    worker->sent_pixels = worker->sent_rects = 0;
    worker->started_us = worker->completed_us = 0;
    worker->clock_valid = h2_pal_time_get_monotonic_us(
        worker->runtime->time, &worker->started_us) == H2_PAL_OK;
    if (worker->tiled) {
      int cursor = 0;
      h2_lua_display_plan_rect_t rect;
      while (h2_lua_display_plan_next_tile(worker->tiles, worker->info.width,
          worker->info.height, worker->gap, &cursor, &rect)) {
        result = submit_rect(worker, rect);
        if (result != H2_PAL_OK) break;
      }
    } else {
      for (int i = 0; i < worker->plan.count; ++i) {
        result = submit_rect(worker, worker->plan.rects[i]);
        if (result != H2_PAL_OK) break;
      }
    }
    if (result == H2_PAL_OK)
      result = (h2_pal_result_t)h2_pal_display_present(display);
    if (h2_pal_time_get_monotonic_us(worker->runtime->time,
          &worker->completed_us) != H2_PAL_OK ||
        worker->completed_us < worker->started_us) worker->clock_valid = 0;
  }
  worker->result = result;
  if (result != H2_PAL_OK) worker->fault = result;
}

static void worker_entry(void *context) {
  h2_lua_display_worker_t *worker = context;
  for (;;) {
    h2_pal_result_t result = h2_pal_semaphore_take(worker->runtime->sync,
        worker->wake, H2_PAL_SYNC_WAIT_FOREVER);
    if (result != H2_PAL_OK) {
      /* Do not race the producer reading the previous completion payload. */
      h2_atomic_store(&worker->phase, (int)result);
      return;
    }
    if (h2_atomic_load(&worker->phase) != H2_LUA_DISPLAY_PENDING) continue;
    if (worker->operation == H2_LUA_DISPLAY_EXIT) {
      h2_atomic_store(&worker->phase, H2_LUA_DISPLAY_EXITED);
      return;
    }
    execute(worker);
    h2_atomic_store(&worker->phase, H2_LUA_DISPLAY_DONE);
  }
}

h2_pal_result_t h2_lua_display_worker_init(h2_lua_display_worker_t *worker,
    const h2_runtime_t *runtime, const h2_pal_mem_api_t *allocator,
    const char *task_name, int borrowed, size_t stack_size) {
  memset(worker, 0, sizeof(*worker));
  worker->runtime = runtime;
  worker->allocator = allocator;
  worker->threaded = task_name != NULL;
  worker->borrowed = borrowed;
  if (h2_atomic_int_init(&worker->phase, H2_LUA_DISPLAY_IDLE) != H2_ATOMIC_OK)
    return H2_PAL_ERR_NO_MEMORY;
  worker->initialized = 1;
  if (!worker->threaded) return H2_PAL_OK;
  h2_pal_result_t result = h2_pal_semaphore_create(runtime->sync,
      &(h2_pal_semaphore_config_t){.name = "$lua/display/wake",
          .allocator = allocator, .max_count = 1}, &worker->wake);
  if (result == H2_PAL_OK)
    result = h2_pal_task_start(runtime->task,
        &(h2_pal_task_options_t){.name = task_name,
                                 .min_stack_size = stack_size},
        worker_entry, worker, &worker->task);
  if (result != H2_PAL_OK) {
    if (worker->wake != NULL) {
      h2_pal_result_t cleanup = h2_pal_semaphore_destroy(runtime->sync, worker->wake);
      if (cleanup != H2_PAL_OK) {
        /* No task started, but the semaphore still owns allocator/context
         * dependencies. Keep initialized storage rooted for checked cleanup. */
        h2_atomic_store(&worker->phase, H2_LUA_DISPLAY_EXITED);
        return result;
      }
      worker->wake = NULL;
    }
    h2_atomic_int_destroy(&worker->phase);
    worker->initialized = 0;
  }
  return result;
}

int h2_lua_display_worker_phase(h2_lua_display_worker_t *worker) {
  return h2_atomic_load(&worker->phase);
}

void h2_lua_display_worker_ack(h2_lua_display_worker_t *worker) {
  int expected = H2_LUA_DISPLAY_DONE;
  (void)h2_atomic_int_compare_exchange(&worker->phase, &expected,
      H2_LUA_DISPLAY_IDLE, H2_ATOMIC_SEQ_CST, H2_ATOMIC_SEQ_CST);
}

h2_pal_result_t h2_lua_display_worker_post(h2_lua_display_worker_t *worker,
                                         int operation) {
  if (h2_lua_display_worker_phase(worker) != H2_LUA_DISPLAY_IDLE)
    return H2_PAL_ERR_BUSY;
  worker->operation = operation;
  int expected = H2_LUA_DISPLAY_IDLE;
  if (!h2_atomic_int_compare_exchange(&worker->phase, &expected,
          H2_LUA_DISPLAY_PENDING, H2_ATOMIC_SEQ_CST, H2_ATOMIC_SEQ_CST))
    return expected < 0 ? (h2_pal_result_t)expected : H2_PAL_ERR_BUSY;
  if (!worker->threaded) {
    if (operation == H2_LUA_DISPLAY_EXIT)
      h2_atomic_store(&worker->phase, H2_LUA_DISPLAY_EXITED);
    else {
      execute(worker);
      h2_atomic_store(&worker->phase, H2_LUA_DISPLAY_DONE);
    }
    return H2_PAL_OK;
  }
  /* A failed wake does not prove the task stopped. Keep the mailbox/root. */
  return h2_pal_semaphore_give(worker->runtime->sync, worker->wake);
}

h2_pal_result_t h2_lua_display_worker_join(h2_lua_display_worker_t *worker) {
  if (!worker->initialized) return H2_PAL_OK;
  int phase = h2_lua_display_worker_phase(worker);
  if (phase != H2_LUA_DISPLAY_EXITED && phase >= 0)
    return H2_PAL_ERR_BUSY;
  if (worker->task != NULL) {
    h2_pal_result_t result = h2_pal_task_join(worker->runtime->task, worker->task);
    if (result != H2_PAL_OK) return result;
    worker->task = NULL;
  }
  if (worker->wake != NULL) {
    h2_pal_result_t result = h2_pal_semaphore_destroy(worker->runtime->sync,
                                                     worker->wake);
    if (result != H2_PAL_OK) return result;
    worker->wake = NULL;
  }
  h2_atomic_int_destroy(&worker->phase);
  worker->initialized = 0;
  return H2_PAL_OK;
}
