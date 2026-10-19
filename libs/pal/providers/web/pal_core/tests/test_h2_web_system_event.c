#include "h2_web_main_thread.h"
#include "h2_web_platform.h"

#include <emscripten.h>
#include <emscripten/eventloop.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expression)                                                      \
  do {                                                                         \
    if (!(expression)) {                                                       \
      fprintf(stderr, "Web system event failure line=%d: %s\n", __LINE__,      \
              #expression);                                                    \
      emscripten_force_exit(1);                                                \
    }                                                                          \
  } while (0)

#define EVENT_TYPE H2_PAL_SYSTEM_EVENT_TYPE_GPIO_IRQ
#define SECOND_TYPE H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_CONNECTED

static h2_web_platform_t *platform;
static const h2_pal_task_api_t *tasks;
static const h2_pal_sync_api_t *sync_api;
static const h2_pal_system_event_api_t *events;

/* clang-format off */
EM_JS(void, set_online,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["i32", "i32"], null,
    (online, notify) => { globalThis.h2TestNetif.set(!!online, !!notify); });
});
/* clang-format on */

static void watchdog(void *unused) {
  (void)unused;
  fprintf(stderr, "Web system event regression exceeded its deadline\n");
  emscripten_force_exit(2);
}

static void wait_for(_Atomic int *value) {
  const double deadline = emscripten_get_now() + 3000.0;
  while (!atomic_load(value) && emscripten_get_now() < deadline)
    h2_web_worker_sleep(1u);
  CHECK(atomic_load(value));
}

static h2_pal_task_t *start(h2_pal_task_entry_t entry, void *user) {
  h2_pal_task_t *task = NULL;
  const h2_pal_task_options_t options = {.name = "web-event-regression",
                                         .min_stack_size = 65536u};
  CHECK(h2_pal_task_start(tasks, &options, entry, user, &task) == H2_PAL_OK);
  return task;
}

static void join(h2_pal_task_t *task) {
  CHECK(h2_pal_task_join(tasks, task) == H2_PAL_OK);
}

static void post(h2_pal_system_event_type_t type, uint32_t source) {
  const h2_pal_system_event_t event = {.type = type, .source_id = source};
  CHECK(h2_pal_system_event_post(events, &event, 100u) == H2_PAL_OK);
}

static h2_pal_semaphore_t *semaphore(void) {
  h2_pal_semaphore_t *sem = NULL;
  const h2_pal_semaphore_config_t config = {.name = "web-event-gate",
                                            .allocator =
                                                h2_web_platform_mem_api(),
                                            .initial_count = 0u,
                                            .max_count = 1u};
  CHECK(h2_pal_semaphore_create(sync_api, &config, &sem) == H2_PAL_OK);
  return sem;
}

static void give(h2_pal_semaphore_t *sem) {
  CHECK(h2_pal_semaphore_give(sync_api, sem) == H2_PAL_OK);
}

static void take(h2_pal_semaphore_t *sem) {
  CHECK(h2_pal_semaphore_take(sync_api, sem, H2_PAL_SYNC_WAIT_FOREVER) ==
        H2_PAL_OK);
}

static void destroy_sem(h2_pal_semaphore_t *sem) {
  CHECK(h2_pal_semaphore_destroy(sync_api, sem) == H2_PAL_OK);
}

typedef struct gate_state {
  h2_pal_system_event_subscription_t *subscription;
  h2_pal_semaphore_t *gate;
  h2_pal_system_event_type_t type;
  _Atomic int entered;
  _Atomic int returned;
  int self_on_release;
  _Atomic int poster_done;
} gate_state_t;

static int gated_handler(void *user, const h2_pal_system_event_t *event) {
  gate_state_t *state = user;
  CHECK(event->type == state->type);
  ++state->entered;
  /* The posting Worker is held until the owner opens this gate. */
  take(state->gate);
  if (state->self_on_release) {
    h2_pal_system_event_unsubscribe(events, state->subscription);
    state->subscription = NULL;
  }
  ++state->returned;
  return H2_PAL_OK;
}

static gate_state_t make_gate(h2_pal_system_event_type_t type) {
  return (gate_state_t){.gate = semaphore(), .type = type};
}

static void subscribe_gate(gate_state_t *state) {
  CHECK(h2_pal_system_event_subscribe(events, state->type, gated_handler, state,
                                      &state->subscription) == H2_PAL_OK);
}

static void posting_task(void *user) {
  gate_state_t *state = user;
  post(state->type, 0u);
  state->poster_done = 1;
}

typedef struct unsubscribe_state {
  gate_state_t *target;
  _Atomic(h2_pal_task_t *) self;
  int cancel_before;
  int expect_cancel;
  _Atomic int entered;
  _Atomic int done;
} unsubscribe_state_t;

static void unsubscribe_task(void *user) {
  unsubscribe_state_t *state = user;
  while (state->self == NULL)
    h2_web_worker_sleep(1u);
  if (state->cancel_before) {
    /* Cancel an already-running task immediately before its cleanup wait. */
    CHECK(h2_web_platform_task_cancel(platform, state->self) == H2_PAL_OK);
  }
  state->entered = 1;
  h2_pal_system_event_unsubscribe(events, state->target->subscription);
  state->target->subscription = NULL;
  CHECK(state->target->returned == 1);
  if (state->expect_cancel) {
    CHECK(h2_pal_time_sleep_ms(h2_web_platform_time_api(platform), 0u) ==
          H2_PAL_EXIT);
  }
  state->done = 1;
}

static void test_external_and_cancel(int cancel_mode) {
  gate_state_t gate = make_gate(EVENT_TYPE);
  subscribe_gate(&gate);
  h2_pal_task_t *poster = start(posting_task, &gate);
  wait_for(&gate.entered);
  CHECK(gate.entered == 1 && gate.returned == 0);
  unsubscribe_state_t unsub = {.target = &gate,
                               .cancel_before = cancel_mode == 1,
                               .expect_cancel = cancel_mode != 0};
  unsub.self = start(unsubscribe_task, &unsub);
  wait_for(&unsub.entered);
  h2_web_worker_sleep(10u);
  CHECK(!unsub.done);
  /* An unrelated worker must not inherit the posting worker's self status. */
  CHECK(h2_web_platform_destroy(platform) == H2_PAL_ERR_BUSY);
  if (cancel_mode == 2) {
    CHECK(h2_web_platform_task_cancel(platform, unsub.self) == H2_PAL_OK);
    h2_web_worker_sleep(10u);
    CHECK(!unsub.done); // cancellation must not abandon callback ownership
  }
  CHECK(!unsub.done && !gate.returned);
  post(EVENT_TYPE, 7u); // admission has stopped, so no new gated callback
  CHECK(gate.entered == 1);
  give(gate.gate);
  wait_for(&gate.poster_done);
  wait_for(&unsub.done);
  CHECK(gate.returned == 1);
  join(poster);
  join(unsub.self);
  destroy_sem(gate.gate);
}

typedef struct self_state {
  h2_pal_system_event_subscription_t *outer;
  h2_pal_system_event_subscription_t *inner;
  int outer_calls;
  int inner_calls;
  int nested;
  _Atomic int done;
} self_state_t;

static int inner_self_handler(void *user, const h2_pal_system_event_t *event) {
  (void)event;
  self_state_t *state = user;
  ++state->inner_calls;
  h2_pal_system_event_unsubscribe(events, state->outer);
  state->outer = NULL;
  return H2_PAL_OK;
}

static int outer_self_handler(void *user, const h2_pal_system_event_t *event) {
  (void)event;
  self_state_t *state = user;
  ++state->outer_calls;
  CHECK(h2_web_platform_destroy(platform) == H2_PAL_ERR_BUSY);
  if (state->nested)
    post(SECOND_TYPE, 0u);
  else {
    h2_pal_system_event_unsubscribe(events, state->outer);
    state->outer = NULL;
  }
  return H2_PAL_OK;
}

static void self_post_task(void *user) {
  self_state_t *state = user;
  post(EVENT_TYPE, 0u);
  state->done = 1;
}

static void test_self_and_nested(int nested, int worker) {
  self_state_t state = {.nested = nested};
  CHECK(h2_pal_system_event_subscribe(events, EVENT_TYPE, outer_self_handler,
                                      &state, &state.outer) == H2_PAL_OK);
  if (nested)
    CHECK(h2_pal_system_event_subscribe(events, SECOND_TYPE, inner_self_handler,
                                        &state, &state.inner) == H2_PAL_OK);
  if (worker) {
    h2_pal_task_t *task = start(self_post_task, &state);
    wait_for(&state.done); // self must not park waiting for itself
    join(task);
  } else {
    post(EVENT_TYPE,
         0u); // no tasks: BUSY above is specifically dispatch ownership
  }
  post(EVENT_TYPE, 0u);
  CHECK(state.outer_calls == 1 && state.inner_calls == nested);
  if (state.inner != NULL)
    h2_pal_system_event_unsubscribe(events, state.inner);
}

static void test_non_lifo_workers(void) {
  gate_state_t first = make_gate(EVENT_TYPE);
  gate_state_t second = make_gate(SECOND_TYPE);
  second.self_on_release = 1;
  subscribe_gate(&first);
  subscribe_gate(&second);
  h2_pal_task_t *first_task = start(posting_task, &first);
  wait_for(&first.entered);
  h2_pal_task_t *second_task = start(posting_task, &second);
  wait_for(&second.entered);
  /* Frame chain is second -> first. End first while second's frame survives. */
  give(first.gate);
  wait_for(&first.poster_done);
  CHECK(!second.poster_done);
  join(first_task); // frees the first worker stack; no frame may point into it
  h2_pal_system_event_unsubscribe(events, first.subscription);
  first.subscription = NULL;
  CHECK(h2_web_platform_destroy(platform) == H2_PAL_ERR_BUSY);
  give(second.gate);
  wait_for(&second.poster_done);
  CHECK(second.subscription == NULL);
  join(second_task);
  destroy_sem(first.gate);
  destroy_sem(second.gate);
}

typedef struct replace_state {
  h2_pal_system_event_subscription_t *killer;
  h2_pal_system_event_subscription_t *retired;
  h2_pal_system_event_subscription_t *replacement;
  int killer_calls;
  int retired_calls;
  int replacement_calls;
  int reused;
} replace_state_t;

static int replacement_handler(void *user, const h2_pal_system_event_t *event) {
  (void)event;
  ++((replace_state_t *)user)->replacement_calls;
  return H2_PAL_OK;
}

static int retired_handler(void *user, const h2_pal_system_event_t *event) {
  (void)event;
  ++((replace_state_t *)user)->retired_calls;
  return H2_PAL_OK;
}

static int killer_handler(void *user, const h2_pal_system_event_t *event) {
  (void)event;
  replace_state_t *state = user;
  if (++state->killer_calls == 1) {
    const uintptr_t retired_address = (uintptr_t)state->retired;
    h2_pal_system_event_unsubscribe(events, state->retired);
    state->retired = NULL;
    CHECK(h2_pal_system_event_subscribe(events, EVENT_TYPE, replacement_handler,
                                        state,
                                        &state->replacement) == H2_PAL_OK);
    state->reused = (uintptr_t)state->replacement == retired_address;
  }
  return H2_PAL_OK;
}

static void test_snapshot_address_reuse(void) {
  int observed_reuse = 0;
  for (unsigned cycle = 0u; cycle < 100u; ++cycle) {
    replace_state_t state = {0};
    /* The real provider prepends subscriptions: killer dispatches first. */
    CHECK(h2_pal_system_event_subscribe(events, EVENT_TYPE, retired_handler,
                                        &state, &state.retired) == H2_PAL_OK);
    CHECK(h2_pal_system_event_subscribe(events, EVENT_TYPE, killer_handler,
                                        &state, &state.killer) == H2_PAL_OK);
    post(EVENT_TYPE, 0u);
    CHECK(state.killer_calls == 1 && state.retired_calls == 0 &&
          state.replacement_calls == 0);
    observed_reuse |= state.reused;
    post(EVENT_TYPE, 0u);
    CHECK(state.killer_calls == 2 && state.retired_calls == 0 &&
          state.replacement_calls == 1);
    h2_pal_system_event_unsubscribe(events, state.killer);
    h2_pal_system_event_unsubscribe(events, state.replacement);
  }
  CHECK(observed_reuse); // Require actual ABA address reuse, not a hypothetical
                         // claim.
}

typedef struct release_state {
  gate_state_t *target;
  _Atomic int done;
} release_state_t;

static void release_task(void *user) {
  release_state_t *state = user;
  give(state->target->gate);
  state->done = 1;
}

static int root_unsubscribe_handler(void *user,
                                    const h2_pal_system_event_t *event) {
  (void)event;
  gate_state_t *gate = user;
  CHECK(h2_web_platform_destroy(platform) == H2_PAL_ERR_BUSY);
  h2_pal_system_event_unsubscribe(events, gate->subscription);
  gate->subscription = NULL;
  CHECK(gate->returned == 1);
  return H2_PAL_OK;
}

static void test_root_can_drive_progress(int inside_pump) {
  gate_state_t gate = make_gate(EVENT_TYPE);
  subscribe_gate(&gate);
  h2_pal_task_t *poster = start(posting_task, &gate);
  wait_for(&gate.entered);
  CHECK(!gate.returned);
  release_state_t release = {.target = &gate};
  h2_pal_task_t *releaser = start(release_task, &release);
  if (inside_pump) {
    h2_pal_system_event_subscription_t *root_subscription = NULL;
    CHECK(h2_pal_system_event_subscribe(
              events, H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED,
              root_unsubscribe_handler, &gate,
              &root_subscription) == H2_PAL_OK);
    (void)h2_web_main_call(set_online, (const void *[]){&(int){0}, &(int){1}});
    CHECK(h2_web_platform_pump(platform, 64u, NULL) == H2_PAL_OK);
    CHECK(gate.subscription == NULL);
    h2_pal_system_event_unsubscribe(events, root_subscription);
    (void)h2_web_main_call(set_online, (const void *[]){&(int){1}, &(int){0}});
  } else {
    h2_pal_system_event_unsubscribe(events, gate.subscription);
    gate.subscription = NULL;
  }
  wait_for(&release.done);
  wait_for(&gate.poster_done);
  CHECK(gate.returned == 1);
  join(poster);
  join(releaser);
  destroy_sem(gate.gate);
}

int main(void) {
  const long watchdog_id = emscripten_set_timeout(watchdog, 10000.0, NULL);
  const h2_web_platform_config_t config = {.display_width = 1,
                                           .display_height = 1};
  platform = h2_web_platform_create(&config);
  CHECK(platform != NULL);
  tasks = h2_web_platform_task_api(platform);
  sync_api = h2_web_platform_sync_api(platform);
  events = h2_web_platform_system_event_api(platform);
  CHECK(h2_pal_system_event_init(events) == H2_PAL_OK);
  test_external_and_cancel(0);
  test_external_and_cancel(1);
  test_external_and_cancel(2);
  test_self_and_nested(0, 0);
  test_self_and_nested(1, 0);
  test_self_and_nested(0, 1);
  test_self_and_nested(1, 1);
  test_non_lifo_workers();
  test_snapshot_address_reuse();
  test_root_can_drive_progress(0);
  test_root_can_drive_progress(1);
  h2_pal_system_event_deinit(events);
  CHECK(h2_web_platform_destroy(platform) == H2_PAL_OK);
  emscripten_clear_timeout(watchdog_id);
  puts("H2_WEB_SYSTEM_EVENT_TEST passed=11 failed=0");
  emscripten_force_exit(0);
  return 0;
}
