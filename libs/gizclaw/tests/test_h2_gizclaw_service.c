#include "h2_desktop_platform.h"
#include "h2_gizclaw_service_internal.h"
#include "h2_gizclaw_task_names.h"

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

typedef struct test_env {
  atomic_uint init_count;
  atomic_uint connect_count;
  atomic_uint poll_count;
  atomic_uint event_handler_count;
  atomic_uint event_dispatch_count;
  atomic_uint close_count;
  atomic_uint deinit_count;
  atomic_uint cleanup_count;
  atomic_uint run_count;
  atomic_uint run_exit_count;
  atomic_uint prepare_count;
  atomic_uint progress_call_count;
  atomic_uint progress_callback_count;
  atomic_uint progress_exit_count;
  atomic_bool run_gate;
  atomic_bool connect_gate;
  atomic_bool event_dispatch_gate;
  atomic_bool original_cancel;
  h2_pal_result_t init_result;
  h2_pal_result_t connect_result;
  h2_pal_result_t poll_result;
  h2_pal_result_t event_dispatch_result;
  h2_pal_result_t prepare_result;
  h2_pal_result_t progress_result;
  h2_pal_result_t progress_call_result;
  h2_gizclaw_service_t *service;
  h2_gizclaw_operation_t *cancel_from_progress;
  uint64_t completed[8];
  h2_gizclaw_operation_terminal_kind_t terminal_kinds[8];
  h2_pal_result_t completion_results[8];
  size_t completion_count;
  pthread_t dispatch_thread;
  unsigned terminal_count;
  h2_pal_result_t terminal_result;
} test_env_t;

static test_env_t *s_env;

static h2_pal_result_t fake_client_init(const h2_gizclaw_config_t *config,
                                        h2_gizclaw_client_t **out_client) {
  assert(config != NULL);
  assert(config->cancel_requested != NULL);
  atomic_fetch_add_explicit(&s_env->init_count, 1u, memory_order_relaxed);
  if (s_env->init_result != H2_PAL_OK)
    return s_env->init_result;
  *out_client = (h2_gizclaw_client_t *)s_env;
  return H2_PAL_OK;
}

static h2_pal_result_t fake_client_connect(h2_gizclaw_client_t *client) {
  assert(client == (h2_gizclaw_client_t *)s_env);
  atomic_fetch_add_explicit(&s_env->connect_count, 1u, memory_order_relaxed);
  while (!atomic_load_explicit(&s_env->connect_gate, memory_order_acquire))
    sched_yield();
  return s_env->connect_result;
}

static h2_pal_result_t fake_client_poll(h2_gizclaw_client_t *client,
                                        int timeout_ms) {
  assert(client == (h2_gizclaw_client_t *)s_env);
  assert(timeout_ms > 0);
  atomic_fetch_add_explicit(&s_env->poll_count, 1u, memory_order_relaxed);
  return s_env->poll_result;
}

static h2_pal_result_t
fake_client_set_event_handler(h2_gizclaw_client_t *client,
                              h2_gizclaw_client_event_fn on_event,
                              void *event_user) {
  assert(client == (h2_gizclaw_client_t *)s_env);
  assert(on_event != NULL);
  assert(event_user == s_env);
  atomic_fetch_add_explicit(&s_env->event_handler_count, 1u,
                            memory_order_relaxed);
  return H2_PAL_OK;
}

static h2_pal_result_t fake_client_dispatch_event(h2_gizclaw_client_t *client) {
  assert(client == (h2_gizclaw_client_t *)s_env);
  atomic_fetch_add_explicit(&s_env->event_dispatch_count, 1u,
                            memory_order_relaxed);
  while (!atomic_load_explicit(&s_env->event_dispatch_gate,
                               memory_order_acquire)) {
    sched_yield();
  }
  return s_env->event_dispatch_result;
}

static h2_pal_result_t fake_client_close(h2_gizclaw_client_t *client) {
  assert(client == (h2_gizclaw_client_t *)s_env);
  atomic_fetch_add_explicit(&s_env->close_count, 1u, memory_order_relaxed);
  return H2_PAL_OK;
}

static void fake_client_deinit(h2_gizclaw_client_t *client) {
  assert(client == (h2_gizclaw_client_t *)s_env);
  atomic_fetch_add_explicit(&s_env->deinit_count, 1u, memory_order_relaxed);
}

static void cleanup_client(void *user, h2_gizclaw_client_t *client) {
  test_env_t *env = user;
  assert(client == (h2_gizclaw_client_t *)env);
  assert(!pthread_equal(pthread_self(), env->dispatch_thread));
  assert(atomic_load_explicit(&env->close_count, memory_order_acquire) == 0u);
  atomic_fetch_add_explicit(&env->cleanup_count, 1u, memory_order_release);
}

static const h2_gizclaw_service_client_ops_t s_client_ops = {
    .init = fake_client_init,
    .connect = fake_client_connect,
    .poll = fake_client_poll,
    .set_event_handler = fake_client_set_event_handler,
    .dispatch_event = fake_client_dispatch_event,
    .close = fake_client_close,
    .deinit = fake_client_deinit,
};

static bool original_cancel_requested(void *user) {
  test_env_t *env = user;
  return atomic_load_explicit(&env->original_cancel, memory_order_acquire);
}

static void receive_event(void *user, const h2_gizclaw_client_event_t *event) {
  (void)user;
  (void)event;
}

static h2_pal_result_t prepare_client(void *user,
                                      h2_gizclaw_cancel_fn cancel_requested,
                                      void *cancel_user) {
  test_env_t *env = user;
  assert(cancel_requested != NULL);
  (void)cancel_requested(cancel_user);
  atomic_fetch_add_explicit(&env->prepare_count, 1u, memory_order_relaxed);
  return env->prepare_result;
}

static h2_pal_result_t
run_immediate(void *user, h2_gizclaw_client_t *client,
              const h2_gizclaw_cancel_token_t *cancel_token) {
  test_env_t *env = user;
  assert(client == (h2_gizclaw_client_t *)env);
  assert(!h2_gizclaw_cancel_requested(cancel_token));
  atomic_fetch_add_explicit(&env->run_count, 1u, memory_order_relaxed);
  return H2_PAL_OK;
}

static h2_pal_result_t
run_until_released(void *user, h2_gizclaw_client_t *client,
                   const h2_gizclaw_cancel_token_t *cancel_token) {
  test_env_t *env = user;
  assert(client == (h2_gizclaw_client_t *)env);
  atomic_fetch_add_explicit(&env->run_count, 1u, memory_order_relaxed);
  while (!atomic_load_explicit(&env->run_gate, memory_order_acquire) &&
         !h2_gizclaw_cancel_requested(cancel_token)) {
    sched_yield();
  }
  const h2_pal_result_t result =
      h2_gizclaw_cancel_requested(cancel_token) ? H2_PAL_ERR_CLOSED : H2_PAL_OK;
  atomic_fetch_add_explicit(&env->run_exit_count, 1u, memory_order_release);
  return result;
}

static h2_pal_result_t
run_io_failure(void *user, h2_gizclaw_client_t *client,
               const h2_gizclaw_cancel_token_t *cancel_token) {
  test_env_t *env = user;
  assert(client == (h2_gizclaw_client_t *)env);
  assert(!h2_gizclaw_cancel_requested(cancel_token));
  atomic_fetch_add_explicit(&env->run_count, 1u, memory_order_relaxed);
  return H2_PAL_ERR_IO;
}

static h2_pal_result_t
run_transport_closed(void *user, h2_gizclaw_client_t *client,
                     const h2_gizclaw_cancel_token_t *cancel_token) {
  test_env_t *env = user;
  assert(client == (h2_gizclaw_client_t *)env);
  assert(!h2_gizclaw_cancel_requested(cancel_token));
  atomic_fetch_add_explicit(&env->run_count, 1u, memory_order_relaxed);
  return H2_PAL_ERR_CLOSED;
}

static h2_pal_result_t record_progress(void *user) {
  test_env_t *env = user;
  assert(pthread_equal(pthread_self(), env->dispatch_thread));
  atomic_fetch_add_explicit(&env->progress_callback_count, 1u,
                            memory_order_relaxed);
  if (env->cancel_from_progress != NULL) {
    assert(h2_gizclaw_operation_cancel(env->cancel_from_progress) == H2_PAL_OK);
    assert(atomic_load_explicit(&env->progress_exit_count,
                                memory_order_acquire) == 0u);
  }
  return env->progress_result;
}

static h2_pal_result_t
run_progress_call(void *user, h2_gizclaw_client_t *client,
                  const h2_gizclaw_cancel_token_t *cancel_token) {
  test_env_t *env = user;
  assert(client == (h2_gizclaw_client_t *)env);
  atomic_fetch_add_explicit(&env->run_count, 1u, memory_order_relaxed);
  atomic_fetch_add_explicit(&env->progress_call_count, 1u,
                            memory_order_release);
  env->progress_call_result =
      h2_gizclaw_operation_dispatch_call(cancel_token, record_progress, env);
  atomic_fetch_add_explicit(&env->progress_exit_count, 1u,
                            memory_order_release);
  return env->progress_call_result;
}

static void record_completion(void *user, h2_gizclaw_operation_t *operation,
                              const h2_gizclaw_operation_result_t *result) {
  test_env_t *env = user;
  assert(pthread_equal(pthread_self(), env->dispatch_thread));
  assert(env->completion_count < 8u);
  env->completed[env->completion_count] = result->identity;
  env->terminal_kinds[env->completion_count] = result->terminal_kind;
  env->completion_results[env->completion_count] = result->result;
  ++env->completion_count;
  h2_gizclaw_operation_release(operation);
}

static void
submit_and_cancel_from_completion(void *user, h2_gizclaw_operation_t *operation,
                                  const h2_gizclaw_operation_result_t *result) {
  test_env_t *env = user;
  assert(result->identity == 40u);
  assert(result->terminal_kind == H2_GIZCLAW_OPERATION_FINISHED);
  record_completion(user, operation, result);
  h2_gizclaw_operation_t *blocker = NULL;
  assert(h2_gizclaw_service_submit(env->service, 42u, run_until_released,
                                   record_completion, env,
                                   &blocker) == H2_PAL_OK);
  h2_gizclaw_operation_t *next = NULL;
  assert(h2_gizclaw_service_submit(env->service, 41u, run_immediate,
                                   record_completion, env, &next) == H2_PAL_OK);
  assert(h2_gizclaw_operation_cancel(next) == H2_PAL_OK);
  assert(h2_gizclaw_operation_cancel(next) == H2_PAL_OK);
  atomic_store_explicit(&env->run_gate, true, memory_order_release);
}

static void record_terminal(void *user, h2_pal_result_t result) {
  test_env_t *env = user;
  assert(pthread_equal(pthread_self(), env->dispatch_thread));
  ++env->terminal_count;
  env->terminal_result = result;
}

static void wait_for_count(atomic_uint *value, unsigned expected) {
  for (unsigned spin = 0u; spin < 1000000u; ++spin) {
    if (atomic_load_explicit(value, memory_order_acquire) >= expected)
      return;
    sched_yield();
  }
  assert(false && "worker did not make progress");
}

static size_t dispatch_until(h2_gizclaw_service_t *service, test_env_t *env,
                             size_t completion_count, unsigned terminal_count,
                             unsigned progress_callback_count) {
  size_t total_dispatched = 0u;
  for (unsigned spin = 0u; spin < 1000000u; ++spin) {
    size_t dispatched = 0u;
    assert(h2_gizclaw_service_dispatch(service, 1u, &dispatched) == H2_PAL_OK);
    total_dispatched += dispatched;
    if (env->completion_count >= completion_count &&
        env->terminal_count >= terminal_count &&
        atomic_load_explicit(&env->progress_callback_count,
                             memory_order_acquire) >= progress_callback_count) {
      return total_dispatched;
    }
    sched_yield();
  }
  assert(false && "worker did not enqueue callback");
  return total_dispatched;
}

static int fail_task_start(void *user, const h2_pal_task_options_t *options,
                           h2_pal_task_entry_t entry, void *ctx,
                           h2_pal_task_t **out_task) {
  (void)user;
  assert(options != NULL);
  assert(strcmp(options->name, h2_gizclaw_service_task_name) == 0);
  (void)entry;
  (void)ctx;
  *out_task = NULL;
  return H2_PAL_ERR_TASK;
}

static const h2_pal_task_vtable_t s_fail_task_vtable = {
    .start = fail_task_start,
};

static const h2_pal_task_api_t s_fail_task_api = {
    .vtable = &s_fail_task_vtable,
};

static h2_gizclaw_service_t *create_service(test_env_t *env, size_t capacity) {
  memset(env, 0, sizeof(*env));
  env->connect_result = H2_PAL_OK;
  env->poll_result = H2_PAL_ERR_TIMEOUT;
  env->event_dispatch_result = H2_PAL_ERR_WOULD_BLOCK;
  env->prepare_result = H2_PAL_OK;
  env->progress_result = H2_PAL_OK;
  env->dispatch_thread = pthread_self();
  atomic_init(&env->connect_gate, true);
  atomic_init(&env->event_dispatch_gate, true);
  atomic_init(&env->run_gate, false);
  s_env = env;
  h2_gizclaw_service_test_set_client_ops(&s_client_ops);

  static h2_gizclaw_config_t client_config;
  memset(&client_config, 0, sizeof(client_config));
  client_config.allocator = h2_desktop_platform_default_allocator();
  client_config.cancel_requested = original_cancel_requested;
  client_config.cancel_user = env;
  h2_gizclaw_service_config_t config = {
      .client_config = &client_config,
      .task = h2_desktop_platform_task_api(),
      .queue = h2_desktop_platform_queue_api(),
      .sync = h2_desktop_platform_sync_api(),
      .task_options = {.name = "gizclaw-test", .min_stack_size = 0u},
      .operation_capacity = capacity,
      .client_poll_timeout_ms = 1,
      .on_event = receive_event,
      .event_user = env,
      .prepare = prepare_client,
      .prepare_user = env,
      .cleanup = cleanup_client,
      .cleanup_user = env,
      .terminal = record_terminal,
      .terminal_user = env,
  };
  h2_gizclaw_service_t *service = NULL;
  assert(h2_gizclaw_service_init(&config, &service) == H2_PAL_OK);
  assert(service != NULL);
  env->service = service;
  return service;
}

static void test_fifo_capacity_and_dispatch(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_service(&env, 2u);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_start(service) == H2_PAL_ERR_INVALID_STATE);

  h2_gizclaw_operation_t *first = NULL;
  h2_gizclaw_operation_t *second = NULL;
  h2_gizclaw_operation_t *rejected = NULL;
  assert(h2_gizclaw_service_submit(service, 1u, run_immediate,
                                   record_completion, &env,
                                   &first) == H2_PAL_OK);
  assert(h2_gizclaw_service_submit(service, 2u, run_immediate,
                                   record_completion, &env,
                                   &second) == H2_PAL_OK);
  assert(h2_gizclaw_service_submit(service, 3u, run_immediate,
                                   record_completion, &env,
                                   &rejected) == H2_PAL_ERR_WOULD_BLOCK);
  assert(rejected == NULL);
  wait_for_count(&env.run_count, 2u);
  assert(atomic_load_explicit(&env.event_handler_count, memory_order_acquire) ==
         1u);
  assert(atomic_load_explicit(&env.event_dispatch_count, memory_order_acquire) >
         0u);
  assert(env.completion_count == 0u);

  size_t dispatched = 99u;
  assert(h2_gizclaw_service_dispatch(service, 1u, &dispatched) == H2_PAL_OK);
  assert(dispatched == 1u);
  assert(env.completed[0] == 1u);
  assert(dispatch_until(service, &env, 2u, 0u, 0u) == 1u);
  assert(env.completed[1] == 2u);
  assert(h2_gizclaw_service_dispatch(service, 8u, &dispatched) == H2_PAL_OK);
  assert(dispatched == 0u);
  assert(h2_gizclaw_service_dispatch(service, 0u, NULL) ==
         H2_PAL_ERR_INVALID_ARG);

  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(atomic_load(&env.close_count) == 1u);
  assert(atomic_load(&env.deinit_count) == 1u);
  assert(atomic_load(&env.cleanup_count) == 1u);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

static void test_queued_cancel_and_stop_drain(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_service(&env, 3u);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  h2_gizclaw_operation_t *first = NULL;
  h2_gizclaw_operation_t *second = NULL;
  assert(h2_gizclaw_service_submit(service, 10u, run_until_released,
                                   record_completion, &env,
                                   &first) == H2_PAL_OK);
  assert(h2_gizclaw_service_submit(service, 11u, run_immediate,
                                   record_completion, &env,
                                   &second) == H2_PAL_OK);
  wait_for_count(&env.run_count, 1u);
  assert(h2_gizclaw_operation_cancel(second) == H2_PAL_OK);
  atomic_store_explicit(&env.run_gate, true, memory_order_release);
  wait_for_count(&env.run_count, 1u);
  for (unsigned spin = 0u; spin < 1000000u && env.completion_count < 2u;
       ++spin) {
    size_t dispatched = 0u;
    assert(h2_gizclaw_service_dispatch(service, 3u, &dispatched) == H2_PAL_OK);
    if (env.completion_count < 2u)
      sched_yield();
  }
  assert(env.completion_count == 2u);
  assert(env.terminal_kinds[0] == H2_GIZCLAW_OPERATION_FINISHED);
  assert(env.terminal_kinds[1] == H2_GIZCLAW_OPERATION_CANCELED);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

static void test_stop_cancels_running_without_inline_callback(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_service(&env, 1u);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  h2_gizclaw_operation_t *operation = NULL;
  assert(h2_gizclaw_service_submit(service, 20u, run_until_released,
                                   record_completion, &env,
                                   &operation) == H2_PAL_OK);
  wait_for_count(&env.run_count, 1u);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(env.completion_count == 0u);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_ERR_INVALID_STATE);
  size_t dispatched = 0u;
  assert(h2_gizclaw_service_dispatch(service, 1u, &dispatched) == H2_PAL_OK);
  assert(dispatched == 1u);
  assert(env.terminal_kinds[0] == H2_GIZCLAW_OPERATION_SERVICE_CLOSED);
  assert(env.terminal_count == 0u);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

static void test_connect_failure_is_terminal(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_service(&env, 1u);
  env.connect_result = H2_PAL_ERR_IO;
  atomic_store_explicit(&env.connect_gate, false, memory_order_release);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  h2_gizclaw_operation_t *operation = NULL;
  assert(h2_gizclaw_service_submit(service, 30u, run_immediate,
                                   record_completion, &env,
                                   &operation) == H2_PAL_OK);
  atomic_store_explicit(&env.connect_gate, true, memory_order_release);
  wait_for_count(&env.connect_count, 1u);
  assert(dispatch_until(service, &env, 1u, 1u, 0u) == 1u);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(env.terminal_kinds[0] == H2_GIZCLAW_OPERATION_SERVICE_CLOSED);
  assert(env.terminal_count == 1u);
  assert(env.terminal_result == H2_PAL_ERR_IO);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

static void test_event_dispatch_failure_is_terminal(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_service(&env, 1u);
  env.event_dispatch_result = H2_PAL_ERR_IO;
  atomic_store_explicit(&env.event_dispatch_gate, false, memory_order_release);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  wait_for_count(&env.event_dispatch_count, 1u);

  h2_gizclaw_operation_t *operation = NULL;
  assert(h2_gizclaw_service_submit(service, 33u, run_immediate,
                                   record_completion, &env,
                                   &operation) == H2_PAL_OK);
  atomic_store_explicit(&env.event_dispatch_gate, true, memory_order_release);

  wait_for_count(&env.cleanup_count, 1u);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(atomic_load_explicit(&env.run_count, memory_order_acquire) == 0u);
  assert(atomic_load_explicit(&env.close_count, memory_order_acquire) == 1u);
  assert(atomic_load_explicit(&env.deinit_count, memory_order_acquire) == 1u);
  assert(atomic_load_explicit(&env.cleanup_count, memory_order_acquire) == 1u);

  size_t dispatched = 0u;
  assert(h2_gizclaw_service_dispatch(service, 2u, &dispatched) == H2_PAL_OK);
  assert(dispatched == 1u);
  assert(env.completion_count == 1u);
  assert(env.terminal_kinds[0] == H2_GIZCLAW_OPERATION_SERVICE_CLOSED);
  assert(env.completion_results[0] == H2_PAL_ERR_CLOSED);
  assert(env.terminal_count == 1u);
  assert(env.terminal_result == H2_PAL_ERR_IO);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

static void test_operation_error_and_transport_closed(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_service(&env, 1u);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  h2_gizclaw_operation_t *operation = NULL;
  assert(h2_gizclaw_service_submit(service, 31u, run_io_failure,
                                   record_completion, &env,
                                   &operation) == H2_PAL_OK);
  wait_for_count(&env.run_count, 1u);
  size_t dispatched = 0u;
  for (unsigned spin = 0u; spin < 1000000u && env.completion_count == 0u;
       ++spin) {
    assert(h2_gizclaw_service_dispatch(service, 1u, &dispatched) == H2_PAL_OK);
    if (env.completion_count == 0u)
      sched_yield();
  }
  assert(env.completion_count == 1u);
  assert(env.terminal_kinds[0] == H2_GIZCLAW_OPERATION_FINISHED);
  assert(env.completion_results[0] == H2_PAL_ERR_IO);
  assert(env.terminal_count == 0u);

  operation = NULL;
  assert(h2_gizclaw_service_submit(service, 32u, run_transport_closed,
                                   record_completion, &env,
                                   &operation) == H2_PAL_OK);
  wait_for_count(&env.run_count, 2u);
  for (unsigned spin = 0u; spin < 1000000u && env.completion_count < 2u;
       ++spin) {
    assert(h2_gizclaw_service_dispatch(service, 1u, &dispatched) == H2_PAL_OK);
    if (env.completion_count < 2u)
      sched_yield();
  }
  assert(env.completion_count == 2u);
  assert(env.terminal_kinds[1] == H2_GIZCLAW_OPERATION_SERVICE_CLOSED);
  assert(env.completion_results[1] == H2_PAL_ERR_CLOSED);
  assert(env.terminal_count == 1u);
  assert(env.terminal_result == H2_PAL_ERR_CLOSED);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

static void test_running_cancel_is_idempotent_and_late_safe(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_service(&env, 1u);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  h2_gizclaw_operation_t *operation = NULL;
  assert(h2_gizclaw_service_submit(service, 35u, run_until_released,
                                   record_completion, &env,
                                   &operation) == H2_PAL_OK);
  wait_for_count(&env.run_count, 1u);
  assert(h2_gizclaw_operation_cancel(operation) == H2_PAL_OK);
  assert(h2_gizclaw_operation_cancel(operation) == H2_PAL_OK);
  wait_for_count(&env.run_exit_count, 1u);
  assert(h2_gizclaw_operation_cancel(operation) == H2_PAL_OK);
  assert(dispatch_until(service, &env, 1u, 0u, 0u) == 1u);
  assert(env.terminal_kinds[0] == H2_GIZCLAW_OPERATION_CANCELED);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

static void test_callback_can_submit_cancel_and_release(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_service(&env, 3u);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  h2_gizclaw_operation_t *first = NULL;
  assert(h2_gizclaw_service_submit(service, 40u, run_immediate,
                                   submit_and_cancel_from_completion, &env,
                                   &first) == H2_PAL_OK);
  wait_for_count(&env.run_count, 1u);
  (void)dispatch_until(service, &env, 3u, 0u, 0u);
  assert(env.completion_count == 3u);
  assert(env.completed[1] == 42u);
  assert(env.terminal_kinds[1] == H2_GIZCLAW_OPERATION_FINISHED);
  assert(env.completed[2] == 41u);
  assert(env.terminal_kinds[2] == H2_GIZCLAW_OPERATION_CANCELED);
  assert(atomic_load(&env.run_count) == 2u);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

static void test_prepare_init_and_poll_failures_are_terminal(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_service(&env, 1u);
  env.prepare_result = H2_PAL_ERR_IO;
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  wait_for_count(&env.prepare_count, 1u);
  (void)dispatch_until(service, &env, 0u, 1u, 0u);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(env.terminal_count == 1u);
  assert(env.terminal_result == H2_PAL_ERR_IO);
  assert(atomic_load(&env.init_count) == 0u);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);

  service = create_service(&env, 1u);
  env.init_result = H2_PAL_ERR_NO_MEMORY;
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  wait_for_count(&env.init_count, 1u);
  (void)dispatch_until(service, &env, 0u, 1u, 0u);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(env.terminal_count == 1u);
  assert(env.terminal_result == H2_PAL_ERR_NO_MEMORY);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);

  service = create_service(&env, 1u);
  env.poll_result = H2_PAL_ERR_IO;
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  wait_for_count(&env.poll_count, 1u);
  (void)dispatch_until(service, &env, 0u, 1u, 0u);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(env.terminal_count == 1u);
  assert(env.terminal_result == H2_PAL_ERR_IO);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

static void test_original_cancel_and_unstarted_lifecycle(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_service(&env, 1u);
  h2_gizclaw_operation_t *operation = NULL;
  assert(h2_gizclaw_service_submit(service, 50u, run_immediate,
                                   record_completion, &env,
                                   &operation) == H2_PAL_ERR_INVALID_STATE);
  assert(operation == NULL);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_start(service) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);

  service = create_service(&env, 1u);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  atomic_store_explicit(&env.original_cancel, true, memory_order_release);
  assert(h2_gizclaw_service_submit(service, 51u, run_until_released,
                                   record_completion, &env,
                                   &operation) == H2_PAL_OK);
  wait_for_count(&env.run_count, 1u);
  wait_for_count(&env.run_exit_count, 1u);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  size_t dispatched = 0u;
  assert(h2_gizclaw_service_dispatch(service, 1u, &dispatched) == H2_PAL_OK);
  assert(dispatched == 1u);
  assert(env.terminal_count == 1u);
  assert(env.terminal_result == H2_PAL_ERR_CLOSED);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

static void test_task_start_failure_can_deinit(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_service(&env, 1u);
  service->config.task = &s_fail_task_api;
  assert(h2_gizclaw_service_start(service) == H2_PAL_ERR_TASK);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

static void test_progress_runs_on_dispatch_thread_and_returns_result(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_service(&env, 1u);
  env.progress_result = H2_PAL_ERR_IO;
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  h2_gizclaw_operation_t *operation = NULL;
  assert(h2_gizclaw_service_submit(service, 60u, run_progress_call,
                                   record_completion, &env,
                                   &operation) == H2_PAL_OK);
  wait_for_count(&env.progress_call_count, 1u);
  assert(atomic_load_explicit(&env.progress_exit_count, memory_order_acquire) ==
         0u);
  assert(dispatch_until(service, &env, 0u, 0u, 1u) == 1u);
  wait_for_count(&env.progress_exit_count, 1u);
  assert(env.progress_call_result == H2_PAL_ERR_IO);
  assert(atomic_load(&env.progress_callback_count) == 1u);
  size_t dispatched = 0u;
  for (unsigned spin = 0u; spin < 1000000u && env.completion_count == 0u;
       ++spin) {
    assert(h2_gizclaw_service_dispatch(service, 1u, &dispatched) == H2_PAL_OK);
    if (env.completion_count == 0u)
      sched_yield();
  }
  assert(env.completion_count == 1u);
  assert(env.terminal_kinds[0] == H2_GIZCLAW_OPERATION_FINISHED);
  assert(env.completion_results[0] == H2_PAL_ERR_IO);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

static void test_cancel_while_progress_pending_skips_progress_callback(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_service(&env, 1u);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  h2_gizclaw_operation_t *operation = NULL;
  assert(h2_gizclaw_service_submit(service, 61u, run_progress_call,
                                   record_completion, &env,
                                   &operation) == H2_PAL_OK);
  wait_for_count(&env.progress_call_count, 1u);
  assert(h2_gizclaw_operation_cancel(operation) == H2_PAL_OK);
  wait_for_count(&env.progress_exit_count, 1u);
  size_t dispatched = 0u;
  assert(h2_gizclaw_service_dispatch(service, 2u, &dispatched) == H2_PAL_OK);
  assert(dispatched == 1u);
  assert(atomic_load(&env.progress_callback_count) == 0u);
  assert(env.completion_count == 1u);
  assert(env.terminal_kinds[0] == H2_GIZCLAW_OPERATION_CANCELED);
  assert(h2_gizclaw_service_dispatch(service, 2u, &dispatched) == H2_PAL_OK);
  assert(dispatched == 0u);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

static void test_cancel_after_progress_claim_waits_for_callback(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_service(&env, 1u);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  h2_gizclaw_operation_t *operation = NULL;
  assert(h2_gizclaw_service_submit(service, 63u, run_progress_call,
                                   record_completion, &env,
                                   &operation) == H2_PAL_OK);
  env.cancel_from_progress = operation;
  wait_for_count(&env.progress_call_count, 1u);
  assert(dispatch_until(service, &env, 0u, 0u, 1u) == 1u);
  wait_for_count(&env.progress_exit_count, 1u);
  assert(atomic_load(&env.progress_callback_count) == 1u);
  size_t dispatched = 0u;
  for (unsigned spin = 0u; spin < 1000000u && env.completion_count == 0u;
       ++spin) {
    assert(h2_gizclaw_service_dispatch(service, 1u, &dispatched) == H2_PAL_OK);
    if (env.completion_count == 0u)
      sched_yield();
  }
  assert(env.completion_count == 1u);
  assert(env.terminal_kinds[0] == H2_GIZCLAW_OPERATION_CANCELED);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

static void test_stop_while_progress_pending_drains_once(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_service(&env, 1u);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  h2_gizclaw_operation_t *operation = NULL;
  assert(h2_gizclaw_service_submit(service, 62u, run_progress_call,
                                   record_completion, &env,
                                   &operation) == H2_PAL_OK);
  wait_for_count(&env.progress_call_count, 1u);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  wait_for_count(&env.progress_exit_count, 1u);
  size_t dispatched = 0u;
  assert(h2_gizclaw_service_dispatch(service, 2u, &dispatched) == H2_PAL_OK);
  assert(dispatched == 1u);
  assert(atomic_load(&env.progress_callback_count) == 0u);
  assert(env.completion_count == 1u);
  assert(env.terminal_kinds[0] == H2_GIZCLAW_OPERATION_SERVICE_CLOSED);
  assert(h2_gizclaw_service_dispatch(service, 2u, &dispatched) == H2_PAL_OK);
  assert(dispatched == 0u);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

int main(void) {
  test_fifo_capacity_and_dispatch();
  test_queued_cancel_and_stop_drain();
  test_stop_cancels_running_without_inline_callback();
  test_connect_failure_is_terminal();
  test_event_dispatch_failure_is_terminal();
  test_operation_error_and_transport_closed();
  test_running_cancel_is_idempotent_and_late_safe();
  test_callback_can_submit_cancel_and_release();
  test_prepare_init_and_poll_failures_are_terminal();
  test_original_cancel_and_unstarted_lifecycle();
  test_task_start_failure_can_deinit();
  test_progress_runs_on_dispatch_thread_and_returns_result();
  test_cancel_while_progress_pending_skips_progress_callback();
  test_cancel_after_progress_claim_waits_for_callback();
  test_stop_while_progress_pending_drains_once();
  h2_gizclaw_service_test_set_client_ops(NULL);
  puts("h2_gizclaw service tests passed");
  return 0;
}
