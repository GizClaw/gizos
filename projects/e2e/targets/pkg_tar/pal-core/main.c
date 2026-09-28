#include "h2_pal_core_e2e.h"
#include "h2_pal_core_e2e_task_names.h"
#include "h2_smoke_host_runtime.h"
#include "h2_web_main_thread.h"
#include "h2_web_platform.h"
#include <emscripten.h>
#include <emscripten/stack.h>
#include <emscripten/threading.h>
#include <inttypes.h>
#include <malloc.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

typedef struct worker {
  unsigned id, calls, output;
  size_t stack_bytes;
} worker_t;
typedef struct app {
  h2_web_platform_t *platform;
  h2_runtime_t *runtime;
  h2_pal_core_e2e_config_t config;
  h2_pal_core_e2e_result_t result;
  h2_pal_result_t run_result;
  worker_t workers[8];
  h2_pal_task_t *tasks[8];
  h2_pal_semaphore_t *entered, *release, *unsub_started, *unsub_done;
  h2_pal_system_event_subscription_t *subscription;
  h2_pal_system_event_t event;
  h2_pal_netif_default_changed_t payload;
  h2_pal_result_t poster_rc, handler_rc;
  int event_initialized, diagnostic_failures, probes_complete;
} app_t;
static app_t app;
/* clang-format off */
EM_JS(void, browser_isolated,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, [], "i32",
    () => { return globalThis.crossOriginIsolated === true; });
});
/* clang-format on */

/* clang-format off */
EM_JS(int, worker_shared_memory, (), { return HEAPU8.buffer instanceof SharedArrayBuffer; });
/* clang-format on */

/* clang-format off */
EM_JS(void, browser_now,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, [], "double",
    () => { return performance.now(); });
});
/* clang-format on */

/* clang-format off */
EM_JS(void, observed_log,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["i32", "pointer", "pointer"], "i32",
    (level, scope, message) => {
  const entry = globalThis.h2CoreConsole && h2CoreConsole.shift();
  if (!entry) return -2;
  const expected = "[" + UTF8ToString(scope) + "] " + UTF8ToString(message);
  return entry.level === level && entry.text === expected ? 0 : -7;
});
});
/* clang-format on */

/* clang-format off */
EM_JS(void, show_result,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["i32", "i32", "i32", "i32", "i32"], null,
    (passed, failed, blocked, qualified, diagnostic_failures) => {
  document.getElementById("result").textContent =
    "Core v2: " + passed + " PASS / " + failed + " FAIL / " + blocked + " BLOCKED; " +
    "qualified=" + qualified + "; diagnostic_failures=" + diagnostic_failures;
});
});
/* clang-format on */
static h2_pal_result_t clock_us(void *user, uint64_t *out) {
  (void)user;
  *out = (uint64_t)(((double)h2_web_main_call(browser_now, NULL).f64) * 1000.0);
  return H2_PAL_OK;
}
static h2_pal_result_t log_observer(void *user, h2_pal_log_level_t level,
                                    const char *scope, const char *message) {
  (void)user;
  return (h2_pal_result_t)((int)h2_web_main_call(
                               observed_log,
                               (const void *[]){&(int){level},
                                                &(const char *){scope},
                                                &(const char *){message}})
                               .i32);
}
static void case_begin(void *user, const char *id) {
  (void)user;
  printf("H2_WEB_CORE_BEGIN case=%s\n", id);
}
static _Atomic int allocation_fail_after = -1;
static void *task_alloc(void *user, size_t bytes) {
  (void)user;
  int remaining = atomic_load(&allocation_fail_after);
  if (remaining == 0)
    return NULL;
  if (remaining > 0)
    atomic_fetch_sub(&allocation_fail_after, 1);
  return h2_pal_mem_alloc(h2_web_platform_mem_api(), bytes);
}
static void task_free(void *user, void *memory) {
  (void)user;
  h2_pal_mem_free(h2_web_platform_mem_api(), memory);
}
static h2_pal_result_t allocation_fault(void *user, int after) {
  (void)user;
  atomic_store(&allocation_fail_after, after);
  return H2_PAL_OK;
}
static h2_pal_result_t stack_observer(void *user, size_t *out) {
  (void)user;
  *out = emscripten_stack_get_base() - emscripten_stack_get_end();
  return H2_PAL_OK;
}
static h2_pal_result_t resources(void *user, h2_pal_core_resources_t *out) {
  (void)user;
  h2_web_platform_resource_stats_t s;
  h2_pal_result_t rc = h2_web_platform_get_resource_stats(app.platform, &s);
  if (rc != H2_PAL_OK)
    return rc;
  *out = (h2_pal_core_resources_t){.tasks = s.live_tasks,
                                   .task_stack_bytes = s.task_stack_bytes,
                                   .queues = s.live_queues,
                                   .mutexes = s.live_mutexes,
                                   .semaphores = s.live_semaphores,
                                   .conditions = s.live_conditions,
                                   .timers = s.live_timers,
                                   .firmware_infos = s.live_firmware_infos,
                                   .allocations = s.allocations,
                                   .allocation_bytes = s.allocation_bytes};
  return H2_PAL_OK;
}
static _Atomic unsigned parallel_entered;
static pthread_t parallel_ids[2];
static int parallel_pass[2];
static void parallel_worker(void *argument) {
  unsigned index = (unsigned)(uintptr_t)argument;
  parallel_ids[index] = pthread_self();
  atomic_fetch_or(&parallel_entered, 1u << index);
  const double deadline = emscripten_get_now() + 5000.0;
  /* No sleep, yield, browser proxy or coroutine suspension. Both Workers must
   * execute shared-memory atomics concurrently to make this barrier pass. */
  while (atomic_load(&parallel_entered) != 3u &&
         emscripten_get_now() < deadline) {
  }
  parallel_pass[index] = atomic_load(&parallel_entered) == 3u &&
                         !emscripten_is_main_runtime_thread();
}
static h2_pal_result_t parallel_probe(void) {
  h2_pal_task_t *workers[2] = {NULL, NULL};
  h2_pal_task_options_t options = {.name = h2_pal_core_e2e_core_task_name};
  h2_pal_result_t rc = H2_PAL_OK;
  for (unsigned i = 0; i < 2u; ++i) {
    rc = h2_pal_task_start(app.runtime->task, &options, parallel_worker,
                           (void *)(uintptr_t)i, &workers[i]);
    if (rc != H2_PAL_OK)
      break;
  }
  for (unsigned i = 0; i < 2u; ++i)
    if (workers[i] != NULL)
      h2_pal_task_join(app.runtime->task, workers[i]);
  int passed = rc == H2_PAL_OK && parallel_pass[0] && parallel_pass[1] &&
               !pthread_equal(parallel_ids[0], parallel_ids[1]) &&
               !pthread_equal(parallel_ids[0], pthread_self()) &&
               !pthread_equal(parallel_ids[1], pthread_self());
  printf("H2_WEB_CORE_PARALLEL passed=%d distinct_workers=%d no_yield=1\n",
         passed, !pthread_equal(parallel_ids[0], parallel_ids[1]));
  return passed ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}
static size_t heap_bytes(void) { return (size_t)mallinfo().uordblks; }
static h2_pal_result_t join(unsigned index) {
  const double deadline =
      ((double)h2_web_main_call(browser_now, NULL).f64) + 5000.0;
  while (app.tasks[index] != NULL) {
    h2_pal_result_t rc = h2_pal_task_join(app.runtime->task, app.tasks[index]);
    if (rc == H2_PAL_OK) {
      app.tasks[index] = NULL;
      return rc;
    }
    if (rc != H2_PAL_ERR_BUSY)
      return rc;
    if (((double)h2_web_main_call(browser_now, NULL).f64) >= deadline)
      return H2_PAL_ERR_TIMEOUT;
    rc = h2_pal_time_sleep_ms(app.runtime->time, 1u);
    if (rc != H2_PAL_OK)
      return rc;
  }
  return H2_PAL_OK;
}
static void worker_entry(void *user) {
  worker_t *worker = user;
  ++worker->calls;
  worker->stack_bytes =
      (size_t)(emscripten_stack_get_base() - emscripten_stack_get_end());
  worker->output = (worker->id + 1u) * 7919u;
}
static h2_pal_result_t start_worker(unsigned index, size_t stack) {
  app.workers[index] = (worker_t){.id = index};
  const h2_pal_task_options_t options = {.name = h2_pal_core_e2e_core_task_name,
                                         .min_stack_size = stack};
  return h2_pal_task_start(app.runtime->task, &options, worker_entry,
                           &app.workers[index], &app.tasks[index]);
}
static h2_pal_result_t task_probe(void) {
  /* Warm up the Worker pool and libc before repeated native Task cycles. */
  for (unsigned i = 0; i < 5u; ++i) {
    h2_pal_result_t rc = start_worker(0, 0);
    if (rc != H2_PAL_OK)
      return rc;
    rc = join(0);
    if (rc != H2_PAL_OK)
      return rc;
  }
  const size_t before = heap_bytes();
  for (unsigned i = 0; i < 100u; ++i) {
    h2_pal_result_t rc = start_worker(0, 0);
    if (rc != H2_PAL_OK)
      return rc;
    rc = join(0);
    if (rc != H2_PAL_OK)
      return rc;
    if (app.workers[0].calls != 1u || app.workers[0].output != 7919u)
      return H2_PAL_ERR_INVALID_STATE;
    if ((i + 1u) % 25u == 0u)
      printf("H2_WEB_CORE_TASK_HEAP cycles=%u heap=%zu delta=%" PRId64 "\n",
             i + 1u, heap_bytes(), (int64_t)heap_bytes() - (int64_t)before);
  }
  const size_t after = heap_bytes();
  for (unsigned i = 0; i < 8u; ++i) {
    h2_pal_result_t rc = start_worker(i, 0);
    if (rc != H2_PAL_OK)
      return rc;
  }
  for (unsigned i = 8u; i > 0u; --i) {
    h2_pal_result_t rc = join(i - 1u);
    if (rc != H2_PAL_OK)
      return rc;
    if (app.workers[i - 1u].calls != 1u ||
        app.workers[i - 1u].output != i * 7919u)
      return H2_PAL_ERR_INVALID_STATE;
  }
  const size_t stacks[] = {4096u, 16384u, 65536u};
  for (unsigned i = 0; i < 3u; ++i) {
    h2_pal_result_t rc = start_worker(0, stacks[i]);
    if (rc != H2_PAL_OK)
      return rc;
    rc = join(0);
    if (rc != H2_PAL_OK)
      return rc;
    printf("H2_WEB_CORE_TASK_STACK requested=%zu observed=%zu\n", stacks[i],
           app.workers[0].stack_bytes);
    if (app.workers[0].stack_bytes < stacks[i])
      return H2_PAL_ERR_INVALID_STATE;
  }
  printf("H2_WEB_CORE_TASK_PROBE complete=1 calls=100 batch=8 "
         "execution=pthread heap_before=%zu heap_after=%zu "
         "retained_bytes=%" PRId64 "\n",
         before, after, (int64_t)after - (int64_t)before);
  return after > before ? H2_PAL_ERR_INVALID_STATE : H2_PAL_OK;
}

static int handler(void *user, const h2_pal_system_event_t *event) {
  (void)user;
  if (event->source_id != 0x574542u)
    return H2_PAL_ERR_INVALID_STATE;
  app.handler_rc = h2_pal_semaphore_give(app.runtime->sync, app.entered);
  if (app.handler_rc == H2_PAL_OK)
    app.handler_rc =
        h2_pal_semaphore_take(app.runtime->sync, app.release, 3000u);
  return app.handler_rc;
}
static void poster(void *user) {
  (void)user;
  app.poster_rc = (h2_pal_result_t)h2_pal_system_event_post(
      app.config.event_fixture, &app.event, 3000u);
}
static void unsubscribe_worker(void *user) {
  (void)user;
  (void)h2_pal_semaphore_give(app.runtime->sync, app.unsub_started);
  h2_pal_system_event_unsubscribe(app.config.event_fixture, app.subscription);
  app.subscription = NULL;
  (void)h2_pal_semaphore_give(app.runtime->sync, app.unsub_done);
}
#define TRY(expr)                                                              \
  do {                                                                         \
    h2_pal_result_t rc_ = (h2_pal_result_t)(expr);                             \
    if (rc_ != H2_PAL_OK)                                                      \
      return rc_;                                                              \
  } while (0)
static h2_pal_result_t event_probe(void) {
  h2_pal_semaphore_t **sems[] = {&app.entered, &app.release, &app.unsub_started,
                                 &app.unsub_done};
  const h2_pal_semaphore_config_t cfg = {.name = "web-event-probe",
                                         .allocator = app.runtime->mem,
                                         .max_count = 4u};
  for (unsigned i = 0; i < 4u; ++i)
    TRY(h2_pal_semaphore_create(app.runtime->sync, &cfg, sems[i]));
  TRY(h2_pal_system_event_init(app.config.event_fixture));
  app.event_initialized = 1;
  app.event = (h2_pal_system_event_t){
      .type = H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED,
      .source_id = 0x574542u,
      .payload = &app.payload,
      .payload_size = sizeof(app.payload)};
  TRY(h2_pal_system_event_subscribe(app.config.event_fixture, app.event.type,
                                    handler, NULL, &app.subscription));
  const h2_pal_task_options_t options = {.name =
                                             h2_pal_core_e2e_core_task_name};
  TRY(h2_pal_task_start(app.runtime->task, &options, poster, NULL,
                        &app.tasks[0]));
  TRY(h2_pal_semaphore_take(app.runtime->sync, app.entered, 3000u));
  TRY(h2_pal_task_start(app.runtime->task, &options, unsubscribe_worker, NULL,
                        &app.tasks[1]));
  TRY(h2_pal_semaphore_take(app.runtime->sync, app.unsub_started, 3000u));
  h2_pal_result_t early =
      h2_pal_semaphore_take(app.runtime->sync, app.unsub_done, 20u);
  /* Keep context alive even if the provider violates quiescence. Release the
   * original handler before joining, so this diagnostic itself cannot UAF. */
  TRY(h2_pal_semaphore_give(app.runtime->sync, app.release));
  TRY(join(0));
  TRY(join(1));
  int violation = early == H2_PAL_OK;
  if (violation)
    ++app.diagnostic_failures;
  printf("H2_WEB_CORE_EVENT_PROBE complete=1 "
         "unsubscribe_returned_before_handler=%d quiescence=%s early_result=%d "
         "handler=%d post=%d\n",
         violation, violation ? "FAIL" : "PASS", (int)early,
         (int)app.handler_rc, (int)app.poster_rc);
  return early == H2_PAL_OK || early == H2_PAL_ERR_TIMEOUT ? H2_PAL_OK : early;
}
/* A browser-originated retirement must leave the JS event loop free while
 * the still-running Worker callback makes an asynchronous main-thread call. */
static _Atomic int ui_entered, ui_release, ui_returned, ui_retired, ui_early;
/* clang-format off */
EM_JS(void, ui_event_loop_tick,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, [], "i32",
    () => new Promise(resolve => setTimeout(() => resolve(1), 20)));
});
/* clang-format on */
static int ui_waiting_handler(void *user, const h2_pal_system_event_t *event) {
  (void)user; (void)event;
  atomic_store(&ui_entered, 1);
  const double deadline = emscripten_get_now() + 3000.0;
  while (!atomic_load(&ui_release) && emscripten_get_now() < deadline)
    h2_web_worker_sleep(1);
  if (!atomic_load(&ui_release)) return H2_PAL_ERR_TIMEOUT;
  int tick = h2_web_main_call(ui_event_loop_tick, NULL).i32;
  atomic_store(&ui_returned, 1);
  return tick == 1 ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}
static void ui_unsubscribe_completed(void *user) {
  (void)user;
  if (!atomic_load(&ui_returned) || emscripten_is_main_runtime_thread())
    atomic_store(&ui_early, 1);
  atomic_store(&ui_retired, 1);
}
static void ui_request_unsubscribe(void *context, h2_web_main_result_t *result,
                                   h2_web_main_completion_t *completion) {
  (void)context;
  result->i32 = emscripten_is_main_runtime_thread() &&
      h2_web_platform_system_event_api(app.platform) == NULL
      ? h2_web_platform_system_event_unsubscribe_async(app.platform,
          app.subscription, ui_unsubscribe_completed, NULL)
      : H2_PAL_ERR_INVALID_STATE;
  h2_web_main_complete(completion);
}
static h2_pal_result_t ui_unsubscribe_probe(void) {
  TRY(h2_pal_system_event_subscribe(app.config.event_fixture, app.event.type,
                                    ui_waiting_handler, NULL, &app.subscription));
  const h2_pal_task_options_t options = {.name = h2_pal_core_e2e_core_task_name};
  TRY(h2_pal_task_start(app.runtime->task, &options, poster, NULL, &app.tasks[0]));
  double deadline = emscripten_get_now() + 3000.0;
  while (!atomic_load(&ui_entered) && emscripten_get_now() < deadline)
    h2_web_worker_sleep(1);
  if (!atomic_load(&ui_entered)) return H2_PAL_ERR_TIMEOUT;
  int rc = H2_PAL_ERR_BUSY;
  while (rc == H2_PAL_ERR_BUSY && emscripten_get_now() < deadline) {
    rc = h2_web_main_call(ui_request_unsubscribe, NULL).i32;
    if (rc == H2_PAL_ERR_BUSY) h2_web_worker_sleep(1);
  }
  if (rc != H2_PAL_OK) {
    atomic_store(&ui_release, 1);
    return (h2_pal_result_t)rc;
  }
  app.subscription = NULL; // ownership transferred to the retire queue
  if (atomic_load(&ui_retired)) atomic_store(&ui_early, 1);
  atomic_store(&ui_release, 1);
  deadline = emscripten_get_now() + 3000.0;
  while (!atomic_load(&ui_retired) && emscripten_get_now() < deadline)
    h2_web_worker_sleep(1);
  if (!atomic_load(&ui_retired)) return H2_PAL_ERR_TIMEOUT;
  TRY(join(0));
  TRY(h2_web_platform_pump(app.platform, 16u, NULL));
  if (atomic_load(&ui_early) || app.poster_rc != H2_PAL_OK)
    return H2_PAL_ERR_INVALID_STATE;
  puts("H2_WEB_CORE_UI_UNSUBSCRIBE complete=1 main_responsive=1 quiescent=1");
  return H2_PAL_OK;
}
static h2_pal_result_t cleanup_probes(void) {
  if (app.release != NULL)
    (void)h2_pal_semaphore_give(app.runtime->sync, app.release);
  for (unsigned i = 0; i < 8u; ++i)
    TRY(join(i));
  if (app.event_initialized) {
    h2_pal_system_event_unsubscribe(app.config.event_fixture, app.subscription);
    app.subscription = NULL;
    h2_pal_system_event_deinit(app.config.event_fixture);
    app.event_initialized = 0;
  }
  h2_pal_semaphore_t **sems[] = {&app.entered, &app.release, &app.unsub_started,
                                 &app.unsub_done};
  for (unsigned i = 0; i < 4u; ++i)
    if (*sems[i] != NULL) {
      TRY(h2_pal_semaphore_destroy(app.runtime->sync, *sems[i]));
      *sems[i] = NULL;
    }
  return H2_PAL_OK;
}
static void run(void *user) {
  (void)user;
  app.run_result = h2_pal_core_e2e_run(app.runtime, &app.config, &app.result);
  for (size_t i = 0; i < H2_PAL_CORE_E2E_CASE_COUNT; ++i)
    printf("H2_WEB_CORE_CASE id=%s status=%s rc=%d\n", app.result.cases[i].id,
           h2_pal_core_e2e_status_name(app.result.cases[i].status),
           (int)app.result.cases[i].result);
  printf("H2_WEB_CORE_REPORT contract=%u passed=%zu failed=%zu blocked=%zu "
         "not_run=%zu complete=%d qualified=%d cleanup=%d\n",
         H2_PAL_CORE_E2E_CONTRACT_VERSION, app.result.passed, app.result.failed,
         app.result.blocked, app.result.not_run, app.result.complete,
         app.result.qualified, (int)app.result.cleanup_result);
  if (app.result.retained_cleanup || app.result.baseline.retained_cleanup)
    return;
  h2_pal_result_t task_rc = parallel_probe();
  if (task_rc == H2_PAL_OK)
    task_rc = task_probe();
  h2_pal_result_t event_rc = event_probe();
  if (event_rc == H2_PAL_OK) event_rc = ui_unsubscribe_probe();
  h2_pal_result_t cleanup = cleanup_probes();
  if (task_rc != H2_PAL_OK || event_rc != H2_PAL_OK || cleanup != H2_PAL_OK) {
    printf("H2_WEB_CORE_RUNNER_ERROR task=%d event=%d cleanup=%d\n",
           (int)task_rc, (int)event_rc, (int)cleanup);
    return;
  }
  app.probes_complete = 1;
}
int main(void) {
  int worker_main = !emscripten_is_main_runtime_thread();
  int isolated = ((int)h2_web_main_call(browser_isolated, NULL).i32);
  int shared = worker_shared_memory();
  printf("H2_WEB_CORE_BOOT execution=pthread main_worker=%d isolated=%d "
         "shared_memory=%d\n",
         worker_main, isolated, shared);
  if (!worker_main || !isolated || !shared)
    return 2;
  const size_t before = heap_bytes();
  const h2_web_platform_config_t pcfg = {.display_width = 1,
                                         .display_height = 1};
  const h2_pal_mem_vtable_t alloc_vtable = {.alloc = task_alloc,
                                            .free = task_free};
  const h2_pal_mem_api_t allocator = {.vtable = &alloc_vtable};
  app.platform = h2_web_platform_create_with_task_allocator(&pcfg, &allocator);
  if (app.platform == NULL)
    return 2;
  const char *version = "pal-core-web-v2";
  if (h2_web_platform_configure_firmware_info(app.platform, version) !=
      H2_PAL_OK)
    return 2;
  h2_runtime_config_t rcfg = h2_smoke_host_runtime_config(
      "browser", "wasm32", "web", h2_web_platform_mem_api(),
      h2_web_platform_time_api(app.platform),
      h2_web_platform_queue_api(app.platform),
      h2_web_platform_display_api(app.platform));
  rcfg.firmware_info = h2_web_platform_firmware_info_api(app.platform);
  rcfg.log = h2_web_platform_log_api();
  rcfg.timer = h2_web_platform_timer_api(app.platform);
  rcfg.task = h2_web_platform_task_api(app.platform);
  rcfg.sync = h2_web_platform_sync_api(app.platform);
  if (h2_runtime_init(&rcfg, &app.runtime) != H2_PAL_OK)
    return 2;
  app.config = (h2_pal_core_e2e_config_t){
      .timeout_ms = 3000u,
      .firmware_version = version,
      .observe_resources = resources,
      .observe_task_stack = stack_observer,
      .task_allocation_fault = allocation_fault,
      .queue_latest = H2_PAL_CORE_QUEUE_LATEST_REPLACE,
      .observe_monotonic_us = clock_us,
      .observe_log = log_observer,
      .case_begin = case_begin,
      .allow_wall_set = 1,
      .event_fixture = h2_web_platform_system_event_api(app.platform)};
  h2_pal_task_t *runner = NULL;
  const h2_pal_task_options_t options = {.name =
                                             h2_pal_core_e2e_runner_task_name};
  h2_pal_result_t rc =
      h2_pal_task_start(rcfg.task, &options, run, NULL, &runner);
  const double deadline =
      ((double)h2_web_main_call(browser_now, NULL).f64) + 60000.0;
  while (rc == H2_PAL_OK && runner != NULL &&
         ((double)h2_web_main_call(browser_now, NULL).f64) < deadline) {
    rc = h2_web_platform_pump(app.platform, 64u, NULL);
    if (rc != H2_PAL_OK)
      break;
    h2_pal_result_t joined = h2_pal_task_join(rcfg.task, runner);
    if (joined == H2_PAL_OK)
      runner = NULL;
    else if (joined != H2_PAL_ERR_BUSY) {
      rc = joined;
      break;
    }
    if (runner != NULL)
      h2_pal_time_sleep_ms(rcfg.time, 1u);
  }
  if (rc != H2_PAL_OK || runner != NULL || !app.probes_complete) {
    printf("H2_WEB_CORE_RUNNER_ERROR pump=%d joined=%d probes=%d\n", (int)rc,
           runner == NULL, app.probes_complete);
    return 2;
  }
  (void)h2_web_main_call(show_result,
                         (const void *[]){&(int){(int)app.result.passed},
                                          &(int){(int)app.result.failed},
                                          &(int){(int)app.result.blocked},
                                          &(int){app.result.qualified},
                                          &(int){app.diagnostic_failures}});
  const size_t live = heap_bytes();
  h2_runtime_deinit(app.runtime);
  h2_pal_result_t destroy = h2_web_platform_destroy(app.platform);
  printf("H2_WEB_CORE_TEARDOWN destroy=%d heap_before_platform=%zu "
         "heap_live=%zu heap_after_destroy=%zu\n",
         (int)destroy, before, live, heap_bytes());
  if (destroy != H2_PAL_OK)
    return 2;
  printf("H2_WEB_CORE_PROBE_COMPLETE qualified=%d diagnostic_failures=%d\n",
         app.result.qualified, app.diagnostic_failures);
  return app.result.qualified && app.diagnostic_failures == 0 ? 0 : 1;
}
