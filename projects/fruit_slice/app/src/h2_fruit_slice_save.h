#ifndef H2_FRUIT_SLICE_SAVE_H
#define H2_FRUIT_SLICE_SAVE_H

#include "h2_atomic.h"
#include "h2_fruit_slice_game.h"

#define H2_FRUIT_SLICE_PROGRESS_KEY "progress_v1"
#define H2_FRUIT_SLICE_CARDS_KEY "cards_v1"

enum {
  H2_FRUIT_SLICE_SAVE_BEST = 1u,
  H2_FRUIT_SLICE_SAVE_PROGRESS = 2u,
  H2_FRUIT_SLICE_SAVE_CARDS = 4u,
};

typedef struct h2_fruit_slice_save_snapshot {
  unsigned fields;
  uint32_t best;
  uint8_t progress[H2_FRUIT_SLICE_PROGRESS_BYTES];
  uint8_t cards[H2_FRUIT_SLICE_CARDS_BYTES];
} h2_fruit_slice_save_snapshot_t;

typedef struct h2_fruit_slice_save_completion {
  h2_fruit_slice_save_snapshot_t snapshot;
  int best_result, progress_result, cards_result;
  uint32_t work_ms;
} h2_fruit_slice_save_completion_t;

/* One copied request at a time; no game state or namespace handle crosses
 * threads. Runtime and this storage remain borrowed until a successful join.
 * The caller alone starts/joins tasks and consumes completions. */
typedef struct h2_fruit_slice_save_worker {
  h2_runtime_t* runtime;
  h2_pal_task_t* task;
  h2_atomic_bool_t done;
  h2_fruit_slice_save_completion_t completion;
  bool initialized;
} h2_fruit_slice_save_worker_t;

/* Identical synchronous operations are also used by the default run path. */
int h2_fruit_slice_save_best(h2_runtime_t* runtime, uint32_t best);
int h2_fruit_slice_save_blob(h2_runtime_t* runtime, const char* key,
                             const void* data, size_t size);

int h2_fruit_slice_save_worker_init(h2_fruit_slice_save_worker_t* worker,
                                    h2_runtime_t* runtime);
/* Copies the entire snapshot before starting the task. BUSY retains the
 * current request. A failed start publishes no task and performs no write. */
int h2_fruit_slice_save_worker_start(
    h2_fruit_slice_save_worker_t* worker,
    const h2_fruit_slice_save_snapshot_t* snapshot);
/* Without wait, do not join while the write is unfinished. Completion is
 * returned only after join succeeds; a failed join retains all ownership. */
int h2_fruit_slice_save_worker_poll(
    h2_fruit_slice_save_worker_t* worker, bool wait,
    h2_fruit_slice_save_completion_t* completion, bool* completed);
/* Requires no active task. Safe after partial initialization. */
void h2_fruit_slice_save_worker_destroy(h2_fruit_slice_save_worker_t* worker);

#endif
