#include "h2_atomic_e2e.h"
#include "h2/pal/core/h2_pal_errors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct deferred_worker {
  h2_pal_task_entry_t entry;
  void *user;
  unsigned joined;
} deferred_worker_t;
typedef struct fixture {
  void *allocations[2];
  unsigned allocation_count, destroys, started, failed_index, busy, join_calls;
  deferred_worker_t workers[2];
  unsigned *backend_state;
} fixture_t;
#define CHECK(condition) do { if (!(condition)) { \
  fprintf(stderr, "comparison lifetime check failed: %s\n", #condition); \
  return 1; } } while (0)
static void *allocate(void *user, size_t size) {
  fixture_t *f = user;
  void *p = malloc(size);
  if (p && f->allocation_count < 2)
    f->allocations[f->allocation_count++] = p;
  return p;
}
static void release(void *user, void *p) {
  fixture_t *f = user;
  for (unsigned i = 0; i < 2; ++i)
    if (f->allocations[i] == p) {
      f->allocations[i] = NULL;
      --f->allocation_count;
    }
  free(p);
}
static int create(const h2_pal_mem_api_t *mem, bool psram, void **out) {
  (void)psram;
  *out = h2_pal_mem_alloc(mem, sizeof(unsigned));
  if (!*out) return H2_PAL_ERR_NO_MEMORY;
  *(unsigned *)*out = 0;
  ((fixture_t *)mem->user)->backend_state = *out;
  return 0;
}
static void destroy(const h2_pal_mem_api_t *mem, bool psram, void *state) {
  (void)psram;
  ++((fixture_t *)mem->user)->destroys;
  h2_pal_mem_free(mem, state);
}
static unsigned work(void *state, unsigned iterations) {
  *(unsigned *)state += iterations;
  return 0;
}
static unsigned count(const void *state) { return *(const unsigned *)state; }
static void addresses(const void *state, uintptr_t *wrapper, uintptr_t *storage) {
  *wrapper = *storage = (uintptr_t)state;
}
static int start(void *user, const h2_pal_task_options_t *options,
                 h2_pal_task_entry_t entry, void *argument, h2_pal_task_t **out) {
  (void)options;
  fixture_t *f = user;
  deferred_worker_t *worker = &f->workers[f->started++];
  *worker = (deferred_worker_t){.entry = entry, .user = argument};
  *out = (h2_pal_task_t *)worker;
  return 0;
}
static int join(void *user, h2_pal_task_t *handle) {
  fixture_t *f = user;
  deferred_worker_t *worker = (deferred_worker_t *)handle;
  if (worker == &f->workers[f->failed_index]) {
    ++f->join_calls;
    return f->busy ? H2_PAL_ERR_WOULD_BLOCK : H2_PAL_ERR_IO;
  }
  worker->entry(worker->user);
  worker->joined = 1;
  return 0;
}
static int monotonic(void *user, uint64_t *out) { (void)user; *out = 1; return 0; }
static int sleep_ms(void *user, uint32_t ms) { (void)user; (void)ms; return 0; }
static int run_case(unsigned failed_index, unsigned busy) {
  fixture_t f = {.failed_index = failed_index, .busy = busy};
  const h2_pal_mem_vtable_t memory = {.alloc = allocate, .free = release};
  const h2_pal_mem_api_t mem = {.user = &f, .vtable = &memory};
  const h2_pal_task_vtable_t tasks = {.start = start, .join = join};
  const h2_pal_task_api_t task = {.user = &f, .vtable = &tasks};
  const h2_pal_time_vtable_t clock = {.get_monotonic_us = monotonic,
                                     .sleep_ms = sleep_ms};
  const h2_pal_time_api_t time = {.user = &f, .vtable = &clock};
  static const h2_atomic_e2e_backend_t backend = {
      "deferred-fixture", create, destroy, work, count, count, addresses};
  h2_atomic_e2e_result_t result;
  int rc = h2_atomic_e2e_run(&mem, &task, &time, &backend, 8, false, false,
                             NULL, NULL, NULL, NULL, &result);
  CHECK(rc == (busy ? H2_PAL_ERR_TIMEOUT : H2_PAL_ERR_IO));
  CHECK(f.started == 2 && f.allocation_count == 2 && f.destroys == 0);
  CHECK(f.join_calls == (busy ? 20000u : 1u));
  /* A successful start does not guarantee the task ran before a failed join.
   * Invoke every remaining entry after run() returned; its borrowed backend,
   * state, time API and heap worker context must still be usable. */
  for (unsigned i = 0; i < 2; ++i)
    if (!f.workers[i].joined) f.workers[i].entry(f.workers[i].user);
  CHECK(*f.backend_state == 16);
  /* The fixture owns its allocator and reclaims retained blocks only once all
   * deferred entries have stopped. No private worker layout is inspected. */
  for (unsigned i = 0; i < 2; ++i) {
    free(f.allocations[i]);
    f.allocations[i] = NULL;
  }
  return 0;
}
int main(void) {
  for (unsigned index = 0; index < 2; ++index)
    for (unsigned busy = 0; busy < 2; ++busy)
      if (run_case(index, busy)) return 1;
  return 0;
}
