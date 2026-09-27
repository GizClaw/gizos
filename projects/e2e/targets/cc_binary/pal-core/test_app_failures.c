#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "h2_desktop_platform.h"
#include "h2_pal_core_e2e.h"
#include "host_config.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Controlled faults test the App/report/ownership logic; these are never
 * provider qualification evidence. Runtime itself is initialized normally. */
typedef struct allocation {
  void *ptr;
  size_t len;
} allocation_t;
static allocation_t allocations[512];
static size_t live_allocations;

static void *tracked_alloc(void *user, size_t len) {
  (void)user;
  void *p = malloc(len);
  if (p != NULL) {
    size_t i = 0;
    while (i < 512u && allocations[i].ptr != NULL)
      ++i;
    assert(i < 512u);
    allocations[i] = (allocation_t){p, len};
    ++live_allocations;
  }
  return p;
}
static void tracked_free(void *user, void *ptr) {
  (void)user;
  if (ptr == NULL)
    return;
  size_t i = 0;
  while (i < 512u && allocations[i].ptr != ptr)
    ++i;
  assert(i < 512u);
  allocations[i] = (allocation_t){0};
  --live_allocations;
  free(ptr);
}
static void *tracked_realloc(void *user, void *ptr, size_t len) {
  if (ptr == NULL)
    return tracked_alloc(user, len);
  size_t i = 0;
  while (i < 512u && allocations[i].ptr != ptr)
    ++i;
  assert(i < 512u);
  void *p = realloc(ptr, len);
  if (p != NULL)
    allocations[i] = (allocation_t){p, len};
  return p;
}
static int is_live(const void *ptr) {
  uintptr_t p = (uintptr_t)ptr;
  for (size_t i = 0; i < 512u; ++i) {
    uintptr_t base = (uintptr_t)allocations[i].ptr;
    if (base != 0u && p >= base && p - base < allocations[i].len)
      return 1;
  }
  return 0;
}
static const h2_pal_mem_vtable_t memory_vtable = {
    tracked_alloc, tracked_realloc, tracked_free};
static const h2_pal_mem_api_t memory_api = {NULL, &memory_vtable};

static h2_pal_result_t clock_now(void *user, uint64_t *us) {
  (void)user;
  struct timespec ts;
  assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
  *us = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
  return H2_PAL_OK;
}
static h2_pal_core_e2e_config_t app_config(void) {
  h2_pal_core_e2e_config_t config = {0};
  config.timeout_ms = 200u;
  config.observe_monotonic_us = clock_now;
  config.queue_latest = H2_PAL_CORE_QUEUE_LATEST_REPLACE;
  return config;
}
static h2_runtime_t *make_runtime(h2_runtime_config_t *config) {
  config->mem = &memory_api;
  h2_runtime_t *runtime = NULL;
  assert(h2_runtime_init(config, &runtime) == H2_PAL_OK);
  return runtime;
}
static const h2_pal_core_e2e_case_result_t *
find_case(const h2_pal_core_e2e_result_t *result, const char *id) {
  for (size_t i = 0; i < H2_PAL_CORE_E2E_CASE_COUNT; ++i)
    if (strcmp(result->cases[i].id, id) == 0)
      return &result->cases[i];
  abort();
}

static const h2_pal_queue_api_t *fallback_backend;
static int proxy_latest(void *user, h2_pal_queue_t *queue, const void *item) {
  (void)user;
  return h2_pal_queue_send_latest(fallback_backend, queue, item);
}

static void test_missing_observation_and_queue_fallback(void) {
  h2_runtime_config_t config = h2_pal_core_host_config();
  h2_pal_queue_vtable_t queue_vtable = *config.queue->vtable;
  queue_vtable.send_latest = NULL;
  const h2_pal_queue_api_t backend = {config.queue->user, &queue_vtable};
  fallback_backend = &backend;
  h2_pal_queue_vtable_t proxy_vtable = queue_vtable;
  proxy_vtable.send_latest = proxy_latest;
  const h2_pal_queue_api_t queue = {config.queue->user, &proxy_vtable};
  config.queue = &queue;
  h2_runtime_t *runtime = make_runtime(&config);
  assert(runtime->queue->vtable->send_latest != NULL); /* actual proxy */
  const size_t baseline = live_allocations;
  h2_pal_core_e2e_config_t app = app_config();
  app.queue_latest = H2_PAL_CORE_QUEUE_LATEST_FALLBACK;
  h2_pal_core_e2e_result_t result;
  assert(h2_pal_core_e2e_run(runtime, &app, &result) != H2_PAL_OK);
  assert(result.complete && !result.qualified && result.failed == 0u);
  assert(find_case(&result, "pal.core.queue.boundaries")->status ==
         H2_PAL_CORE_E2E_PASS);
  /* Missing resource telemetry must not suppress independent functional
   * evidence, while complete qualification must still remain blocked. */
  assert(find_case(&result, "pal.core.task.once-context")->status ==
         H2_PAL_CORE_E2E_PASS);
  assert(find_case(&result, "pal.core.queue.copy-buffered-close")->status ==
         H2_PAL_CORE_E2E_PASS);
  assert(find_case(&result, "pal.core.mutex.contention")->status ==
         H2_PAL_CORE_E2E_PASS);
  assert(find_case(&result, "pal.core.condition.broadcast")->status ==
         H2_PAL_CORE_E2E_PASS);
  assert(find_case(&result, "pal.core.resources.recovered")->status ==
         H2_PAL_CORE_E2E_BLOCKED);
  assert(find_case(&result, "pal.core.log.output")->status ==
         H2_PAL_CORE_E2E_BLOCKED);
  assert(find_case(&result, "pal.core.time.wall-set")->status ==
         H2_PAL_CORE_E2E_BLOCKED);
  assert(find_case(&result, "pal.core.system-event.lifecycle")->status ==
         H2_PAL_CORE_E2E_BLOCKED);
  assert(live_allocations == baseline);
  app.queue_latest = H2_PAL_CORE_QUEUE_LATEST_REPLACE;
  assert(h2_pal_core_e2e_run(runtime, &app, &result) != H2_PAL_OK);
  assert(find_case(&result, "pal.core.queue.boundaries")->status ==
         H2_PAL_CORE_E2E_FAIL);
  assert(live_allocations == baseline);
  h2_runtime_deinit(runtime);
  assert(live_allocations == 0u);
}

static void test_invalid_config_and_missing_clock(void) {
  h2_runtime_config_t config = h2_pal_core_host_config();
  h2_runtime_t *runtime = make_runtime(&config);
  const size_t baseline = live_allocations;
  h2_pal_core_e2e_config_t app = app_config();
  h2_pal_core_e2e_result_t result;
  app.queue_latest = (h2_pal_core_queue_latest_t)99;
  assert(h2_pal_core_e2e_run(runtime, &app, &result) == H2_PAL_ERR_INVALID_ARG);
  assert(result.not_run == H2_PAL_CORE_E2E_CASE_COUNT && !result.qualified);
  assert(live_allocations == baseline);
  app = app_config();
  app.observe_monotonic_us = NULL;
  assert(h2_pal_core_e2e_run(runtime, &app, &result) != H2_PAL_OK);
  assert(result.complete && !result.qualified && result.failed == 0u);
  assert(find_case(&result, "pal.core.time.monotonic")->status ==
         H2_PAL_CORE_E2E_BLOCKED);
  assert(find_case(&result, "pal.core.task.lifecycle")->status ==
         H2_PAL_CORE_E2E_BLOCKED);
  assert(find_case(&result, "pal.core.queue.close-wake")->status ==
         H2_PAL_CORE_E2E_BLOCKED);
  assert(live_allocations == baseline);
  h2_runtime_deinit(runtime);
  assert(live_allocations == 0u);
}

static h2_pal_task_entry_t deferred_entry;
static void *deferred_context;
static int permit_join;
static int fake_task;
static int deferred_start(void *user, const h2_pal_task_options_t *options,
                          h2_pal_task_entry_t entry, void *context,
                          h2_pal_task_t **out) {
  (void)user;
  (void)options;
  assert(deferred_entry == NULL);
  deferred_entry = entry;
  deferred_context = context;
  *out = (h2_pal_task_t *)&fake_task;
  return H2_PAL_OK;
}
static int deferred_join(void *user, h2_pal_task_t *task) {
  (void)user;
  assert(task == (h2_pal_task_t *)&fake_task);
  if (!permit_join)
    return H2_PAL_ERR_IO;
  assert(is_live(deferred_context));
  deferred_entry(deferred_context);
  deferred_entry = NULL;
  deferred_context = NULL;
  return H2_PAL_OK;
}
static void test_join_retains_context(void) {
  h2_runtime_config_t config = h2_pal_core_host_config();
  const h2_pal_task_vtable_t vtable = {deferred_start, deferred_join};
  const h2_pal_task_api_t task = {NULL, &vtable};
  config.task = &task;
  h2_runtime_t *runtime = make_runtime(&config);
  const size_t baseline = live_allocations;
  h2_pal_core_e2e_config_t app = app_config();
  h2_pal_core_e2e_result_t result;
  assert(h2_pal_core_e2e_run(runtime, &app, &result) != H2_PAL_OK);
  assert(!result.complete && result.not_run != 0u &&
         result.baseline.retained_cleanup != NULL);
  assert(find_case(&result, "pal.core.task.lifecycle")->status ==
         H2_PAL_CORE_E2E_FAIL);
  assert(is_live(deferred_context));
  assert(h2_pal_core_e2e_cleanup(runtime, &result) == H2_PAL_ERR_IO);
  assert(is_live(deferred_context));
  permit_join = 1;
  assert(h2_pal_core_e2e_cleanup(runtime, &result) == H2_PAL_OK);
  assert(!result.qualified && result.cleanup_result == H2_PAL_ERR_IO);
  assert(result.retained_cleanup == NULL &&
         result.baseline.retained_cleanup == NULL);
  assert(live_allocations == baseline);
  h2_runtime_deinit(runtime);
  assert(live_allocations == 0u);
}

static h2_pal_timer_config_t timer_config;
static int fake_timer;
static int permit_destroy;
static h2_pal_result_t timer_create(void *u, const h2_pal_timer_config_t *cfg,
                                    h2_pal_timer_t **out) {
  (void)u;
  timer_config = *cfg;
  *out = (h2_pal_timer_t *)&fake_timer;
  if (cfg->flags & H2_PAL_TIMER_FLAG_AUTO_START)
    cfg->cb(cfg->cb_user, *out);
  return H2_PAL_OK;
}
static h2_pal_result_t timer_destroy(void *u, h2_pal_timer_t *t) {
  (void)u;
  (void)t;
  if (!permit_destroy)
    return H2_PAL_ERR_BUSY;
  timer_config.cb = NULL;
  return H2_PAL_OK;
}
static h2_pal_result_t timer_start(void *u, h2_pal_timer_t *t) {
  (void)u;
  timer_config.cb(timer_config.cb_user, t);
  return H2_PAL_OK;
}
static h2_pal_result_t timer_stop(void *u, h2_pal_timer_t *t) {
  (void)u;
  (void)t;
  return H2_PAL_OK;
}
static h2_pal_result_t timer_period(void *u, h2_pal_timer_t *t, uint32_t ms) {
  (void)u;
  (void)t;
  (void)ms;
  return H2_PAL_OK;
}
static h2_pal_result_t timer_running(void *u, h2_pal_timer_t *t, int *running) {
  (void)u;
  (void)t;
  *running = 0;
  return H2_PAL_OK;
}
static void test_timer_destroy_retains_callback(void) {
  const h2_pal_timer_vtable_t vtable = {
      timer_create, timer_destroy, timer_start,  timer_stop,
      timer_start,  timer_period,  timer_running};
  const h2_pal_timer_api_t timer = {NULL, &vtable};
  h2_runtime_config_t config = h2_pal_core_host_config();
  config.timer = &timer;
  h2_runtime_t *runtime = make_runtime(&config);
  const size_t baseline = live_allocations;
  h2_pal_core_e2e_config_t app = app_config();
  h2_pal_core_e2e_result_t result;
  assert(h2_pal_core_e2e_run(runtime, &app, &result) != H2_PAL_OK);
  assert(find_case(&result, "pal.core.timer.oneshot")->status ==
         H2_PAL_CORE_E2E_FAIL);
  assert(result.retained_cleanup != NULL && result.not_run != 0u);
  assert(is_live(timer_config.cb_user));
  timer_config.cb(timer_config.cb_user, (h2_pal_timer_t *)&fake_timer);
  assert(h2_pal_core_e2e_cleanup(runtime, &result) == H2_PAL_ERR_BUSY);
  permit_destroy = 1;
  assert(h2_pal_core_e2e_cleanup(runtime, &result) == H2_PAL_OK);
  assert(!result.qualified && result.cleanup_result == H2_PAL_ERR_BUSY);
  assert(live_allocations == baseline);
  h2_runtime_deinit(runtime);
  assert(live_allocations == 0u);
}

static int wall_sets;
static int allow_restore;
static h2_pal_result_t wall_set(void *u, uint64_t wall) {
  (void)u;
  ++wall_sets;
  if (wall_sets > 1 && !allow_restore)
    return H2_PAL_ERR_IO;
  return h2_pal_time_set_wall_ms(h2_desktop_platform_time_api(), wall);
}
static void test_wall_restore_retained(void) {
  h2_runtime_config_t config = h2_pal_core_host_config();
  h2_pal_time_vtable_t vtable = *config.time->vtable;
  vtable.set_wall_ms = wall_set;
  const h2_pal_time_api_t time = {config.time->user, &vtable};
  config.time = &time;
  h2_runtime_t *runtime = make_runtime(&config);
  const size_t baseline = live_allocations;
  h2_pal_core_e2e_config_t app = app_config();
  app.allow_wall_set = 1;
  h2_pal_core_e2e_result_t result;
  assert(h2_pal_core_e2e_run(runtime, &app, &result) != H2_PAL_OK);
  assert(find_case(&result, "pal.core.time.wall-set")->status ==
         H2_PAL_CORE_E2E_FAIL);
  assert(result.retained_cleanup != NULL &&
         result.cleanup_result == H2_PAL_ERR_IO);
  assert(result.not_run != 0u);
  allow_restore = 1;
  assert(h2_pal_core_e2e_cleanup(runtime, &result) == H2_PAL_OK);
  assert(!result.qualified && result.cleanup_result == H2_PAL_ERR_IO);
  assert(live_allocations == baseline);
  h2_runtime_deinit(runtime);
  assert(live_allocations == 0u);
}

/* Returning success without closing never wakes the reader. The worker times
 * out rather than observing CLOSED; the report must reject this provider. */
static int ignored_close(void *u, h2_pal_queue_t *q) {
  (void)u;
  (void)q;
  return H2_PAL_OK;
}
static void test_broken_close_detected(void) {
  h2_runtime_config_t config = h2_pal_core_host_config();
  h2_pal_queue_vtable_t vtable = *config.queue->vtable;
  vtable.close = ignored_close;
  const h2_pal_queue_api_t queue = {config.queue->user, &vtable};
  config.queue = &queue;
  h2_runtime_t *runtime = make_runtime(&config);
  h2_pal_core_e2e_config_t app = app_config();
  h2_pal_core_e2e_result_t result;
  assert(h2_pal_core_e2e_run(runtime, &app, &result) != H2_PAL_OK);
  assert(find_case(&result, "pal.core.queue.close-wake")->status ==
         H2_PAL_CORE_E2E_FAIL);
  assert(!result.qualified && result.retained_cleanup == NULL);
  h2_runtime_deinit(runtime);
  assert(live_allocations == 0u);
}

int main(void) {
  alarm(30u);
  test_missing_observation_and_queue_fallback();
  test_invalid_config_and_missing_clock();
  test_join_retains_context();
  test_timer_destroy_retains_callback();
  test_wall_restore_retained();
  test_broken_close_detected();
  return 0;
}
