#ifndef H2_LUA_DISPLAY_WORKER_H
#define H2_LUA_DISPLAY_WORKER_H

#include "h2_lua_display_plan.h"
#include "h2_lua.h"
#include "h2_atomic.h"

/* This mailbox and its pixel/tile payload are rooted, VM-accounted storage.
 * Only the producer writes a command; release/acquire transfers ownership.
 * Task/atomic/semaphore implementation storage belongs to the PAL provider. */
typedef struct h2_lua_display_worker {
  const h2_runtime_t *runtime;
  const h2_pal_mem_api_t *allocator;
  h2_pal_task_t *task;
  h2_pal_semaphore_t *wake;
  h2_atomic_int_t phase;
  int initialized;
  int borrowed;
  int opened;
  int operation;
  int fault;
  h2_pal_result_t result;
  h2_display_info_t info;
  h2_lua_display_plan_t plan;
  const uint16_t *pixels;
  uint8_t *tiles;
  int tiled;
  int gap;
  size_t sent_pixels, sent_rects;
  uint64_t started_us, completed_us;
  int clock_valid;
} h2_lua_display_worker_t;

enum {
  H2_LUA_DISPLAY_IDLE,
  H2_LUA_DISPLAY_PENDING,
  H2_LUA_DISPLAY_DONE,
  H2_LUA_DISPLAY_EXITED,
};
enum {
  H2_LUA_DISPLAY_OPEN,
  H2_LUA_DISPLAY_FRAME,
  H2_LUA_DISPLAY_CLOSE,
  H2_LUA_DISPLAY_EXIT,
};

h2_pal_result_t h2_lua_display_worker_init(h2_lua_display_worker_t *worker,
    const h2_runtime_t *runtime, const h2_pal_mem_api_t *allocator,
    int borrowed, size_t stack_size);
h2_pal_result_t h2_lua_display_worker_post(h2_lua_display_worker_t *worker,
                                         int operation);
int h2_lua_display_worker_phase(h2_lua_display_worker_t *worker);
void h2_lua_display_worker_ack(h2_lua_display_worker_t *worker);
/* Joins only after exit is requested or observed. A failed join retains all
 * handles; the PAL join may block or return BUSY according to its contract. */
h2_pal_result_t h2_lua_display_worker_join(h2_lua_display_worker_t *worker);
#endif
