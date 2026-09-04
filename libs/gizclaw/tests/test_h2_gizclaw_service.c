#include "gzc_common.h"
#include "h2_desktop_platform.h"
#include "h2_gizclaw_conversation.h"
#include "h2_gizclaw_firmware.h"
#include "h2_gizclaw_internal.h"
#include "h2_gizclaw_pcm_track.h"
#include "h2_gizclaw_pet.h"
#include "h2_gizclaw_points.h"
#include "h2_gizclaw_profile.h"
#include "h2_gizclaw_registration.h"
#include "h2_gizclaw_service_internal.h"
#include "h2_gizclaw_social.h"
#include "h2_gizclaw_task_names.h"
#include "h2_gizclaw_telemetry.h"
#include "h2_gizclaw_workflow.h"
#include "h2_gizclaw_workspace.h"
#include "payload/ai.pb.h"
#include "payload/firmware.pb.h"
#include "payload/gameplay.pb.h"
#include "payload/social.pb.h"
#include "payload/system.pb.h"
#include "payload/workspace.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include "ogg_opus_fixture.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* Internal RPC test vtables include the optional SDK completion hook. */

typedef struct test_env {
  atomic_uint init_count;
  atomic_uint connect_count;
  atomic_uint poll_count;
  atomic_uint event_handler_count;
  atomic_uint event_dispatch_count;
  atomic_uint event_callback_count;
  atomic_uint close_count;
  atomic_uint deinit_count;
  atomic_uint cleanup_count;
  atomic_uint run_count;
  atomic_uint run_exit_count;
  atomic_uint async_start_count;
  atomic_uint async_poll_count;
  atomic_uint rpc_start_count;
  atomic_uint rpc_result_count;
  atomic_uint rpc_destroy_count;
  atomic_uint prepare_count;
  atomic_uint progress_call_count;
  atomic_uint progress_callback_count;
  atomic_uint progress_exit_count;
  atomic_bool run_gate;
  atomic_bool connect_gate;
  atomic_bool event_dispatch_gate;
  atomic_bool event_emitted;
  atomic_bool original_cancel;
  h2_pal_result_t init_result;
  h2_pal_result_t connect_result;
  h2_pal_result_t poll_result;
  h2_pal_result_t event_dispatch_result;
  h2_pal_result_t prepare_result;
  h2_pal_result_t progress_result;
  h2_pal_result_t progress_call_result;
  h2_gizclaw_client_event_sink_fn installed_event_handler;
  void *installed_event_user;
  h2_gizclaw_cancel_fn installed_client_cancel;
  void *installed_client_cancel_user;
  bool release_in_completion;
  h2_gizclaw_service_t *service;
  h2_gizclaw_operation_t *cancel_from_progress;
  uint64_t completed[8];
  h2_gizclaw_operation_terminal_kind_t terminal_kinds[8];
  h2_pal_result_t completion_results[8];
  atomic_size_t completion_count;
  pthread_t app_thread;
  atomic_uint terminal_count;
  h2_pal_result_t terminal_result;
  atomic_bool sync_rpc_returned;
  h2_pal_result_t sync_rpc_result;
  h2_gizclaw_req_t *sync_rpc;
  atomic_uint rpc_callback_count;
  h2_gizclaw_rpc_method_t expected_method;
  const uint8_t *expected_payload;
  size_t expected_payload_len;
  const uint8_t *response_payload;
  size_t response_payload_len;
  h2_pal_result_t rpc_result;
  bool rpc_remote_error;
  atomic_uint_fast64_t clock_ms;
  unsigned retry_mode;
} test_env_t;

static test_env_t *s_env;
static atomic_uint s_queue_send_timeout_ms;
static atomic_uint s_runtime_post_count;
static h2_runtime_custom_event_id_t s_runtime_event_id;

static h2_pal_result_t
fake_runtime_post(h2_runtime_t *runtime,
                  const h2_runtime_custom_event_t *event) {
  assert(runtime == (h2_runtime_t *)s_env);
  assert(event != NULL && event->payload == NULL && event->payload_size == 0u);
  s_runtime_event_id = event->id;
  atomic_fetch_add_explicit(&s_runtime_post_count, 1u, memory_order_release);
  return H2_PAL_OK;
}

static int test_queue_create(void *user, const h2_pal_queue_config_t *config,
                             h2_pal_queue_t **out_queue) {
  (void)user;
  return h2_pal_queue_create(h2_desktop_platform_queue_api(), config,
                             out_queue);
}

static void test_queue_destroy(void *user, h2_pal_queue_t *queue) {
  (void)user;
  h2_pal_queue_destroy(h2_desktop_platform_queue_api(), queue);
}

static int test_queue_send(void *user, h2_pal_queue_t *queue, const void *item,
                           uint32_t timeout_ms) {
  (void)user;
  atomic_store_explicit(&s_queue_send_timeout_ms, timeout_ms,
                        memory_order_release);
  return h2_pal_queue_send(h2_desktop_platform_queue_api(), queue, item,
                           timeout_ms);
}

static int test_queue_send_latest(void *user, h2_pal_queue_t *queue,
                                  const void *item) {
  (void)user;
  return h2_pal_queue_send_latest(h2_desktop_platform_queue_api(), queue, item);
}

static int test_queue_recv(void *user, h2_pal_queue_t *queue, void *out_item,
                           uint32_t timeout_ms) {
  (void)user;
  return h2_pal_queue_recv(h2_desktop_platform_queue_api(), queue, out_item,
                           timeout_ms);
}

static int test_queue_reset(void *user, h2_pal_queue_t *queue) {
  (void)user;
  return h2_pal_queue_reset(h2_desktop_platform_queue_api(), queue);
}

static int test_queue_close(void *user, h2_pal_queue_t *queue) {
  (void)user;
  return h2_pal_queue_close(h2_desktop_platform_queue_api(), queue);
}

static const h2_pal_queue_vtable_t s_queue_vtable = {
    .create = test_queue_create,
    .destroy = test_queue_destroy,
    .send = test_queue_send,
    .send_latest = test_queue_send_latest,
    .recv = test_queue_recv,
    .reset = test_queue_reset,
    .close = test_queue_close,
};

static const h2_pal_queue_api_t s_queue_api = {
    .vtable = &s_queue_vtable,
};

static h2_pal_result_t fake_client_init(const h2_gizclaw_config_t *config,
                                        h2_gizclaw_client_t **out_client) {
  assert(config != NULL);
  assert(config->cancel_requested != NULL);
  s_env->installed_client_cancel = config->cancel_requested;
  s_env->installed_client_cancel_user = config->cancel_user;
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
                              h2_gizclaw_client_event_sink_fn on_event,
                              void *event_user) {
  assert(client == (h2_gizclaw_client_t *)s_env);
  assert(on_event != NULL);
  assert(event_user != NULL);
  s_env->installed_event_handler = on_event;
  s_env->installed_event_user = event_user;
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
  if (s_env->installed_event_handler != NULL &&
      !atomic_load_explicit(&s_env->event_emitted, memory_order_acquire)) {
    h2_pal_result_t rc = s_env->installed_event_handler(
        s_env->installed_event_user,
        &(h2_gizclaw_client_event_t){
            .kind = H2_GIZCLAW_CLIENT_EVENT_WORKSPACE_HISTORY_UPDATED,
            .workspace_name = {.data = "workspace", .len = 9u},
            .last_updated_at_unix_ms = 123u,
        });
    if (rc != H2_PAL_OK)
      return rc;
    atomic_store_explicit(&s_env->event_emitted, true, memory_order_release);
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
  assert(!pthread_equal(pthread_self(), env->app_thread));
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

static int fake_rpc_start(h2_gizclaw_client_t *client,
                          h2_gizclaw_rpc_method_t method,
                          h2_gizclaw_rpc_bytes_t params_payload,
                          uint32_t timeout_ms,
                          h2_gizclaw_rpc_request_t **out_request) {
  assert(client == (h2_gizclaw_client_t *)s_env);
  assert(method == H2_GIZCLAW_RPC_SERVER_WORKFLOW_GET);
  assert(params_payload.len == 3u);
  assert(memcmp(params_payload.data, "req", 3u) == 0);
  assert(timeout_ms > 0u && timeout_ms <= 1234u);
  atomic_fetch_add_explicit(&s_env->rpc_start_count, 1u, memory_order_release);
  *out_request = (h2_gizclaw_rpc_request_t *)s_env;
  return H2_PAL_OK;
}

static int fake_rpc_result(h2_gizclaw_rpc_request_t *request,
                           h2_gizclaw_rpc_response_t *out_response) {
  assert(request == (h2_gizclaw_rpc_request_t *)s_env);
  const unsigned count = atomic_fetch_add_explicit(&s_env->rpc_result_count, 1u,
                                                   memory_order_release) +
                         1u;
  if (count < 2u)
    return H2_PAL_ERR_WOULD_BLOCK;
  const h2_pal_mem_api_t *allocator =
      s_env->service->config.client_config->allocator;
  uint8_t *payload = h2_pal_mem_alloc(allocator, 3u);
  assert(payload != NULL);
  memcpy(payload, "rsp", 3u);
  *out_response = (h2_gizclaw_rpc_response_t){
      .result_payload = payload,
      .result_payload_len = 3u,
  };
  return H2_PAL_OK;
}

static void fake_rpc_cancel(h2_gizclaw_rpc_request_t *request) {
  assert(request == (h2_gizclaw_rpc_request_t *)s_env);
}

static void fake_rpc_destroy(h2_gizclaw_rpc_request_t *request) {
  assert(request == (h2_gizclaw_rpc_request_t *)s_env);
  atomic_fetch_add_explicit(&s_env->rpc_destroy_count, 1u,
                            memory_order_release);
}

static const h2_gizclaw_async_rpc_ops_t s_rpc_ops = {
    .start = fake_rpc_start,
    .result = fake_rpc_result,
    .cancel = fake_rpc_cancel,
    .destroy = fake_rpc_destroy,
};

static int fake_profile_start(h2_gizclaw_client_t *client,
                              h2_gizclaw_rpc_method_t method,
                              h2_gizclaw_rpc_bytes_t payload,
                              uint32_t timeout_ms,
                              h2_gizclaw_rpc_request_t **out_request) {
  assert(client == (h2_gizclaw_client_t *)s_env);
  assert(method == s_env->expected_method);
  assert(payload.len == s_env->expected_payload_len);
  if (payload.len != 0u)
    assert(memcmp(payload.data, s_env->expected_payload, payload.len) == 0);
  assert(timeout_ms == 1234u);
  *out_request = (h2_gizclaw_rpc_request_t *)s_env;
  atomic_fetch_add_explicit(&s_env->rpc_start_count, 1u, memory_order_release);
  return H2_PAL_OK;
}

static int fake_profile_result(h2_gizclaw_rpc_request_t *request,
                               h2_gizclaw_rpc_response_t *out_response) {
  assert(request == (h2_gizclaw_rpc_request_t *)s_env);
  atomic_fetch_add_explicit(&s_env->rpc_result_count, 1u, memory_order_release);
  if (!atomic_load_explicit(&s_env->run_gate, memory_order_acquire))
    return H2_PAL_ERR_WOULD_BLOCK;
  if (s_env->rpc_result != H2_PAL_OK)
    return s_env->rpc_result;
  const h2_pal_mem_api_t *allocator = s_env->service->client_config.allocator;
  uint8_t *payload = NULL;
  if (s_env->response_payload_len != 0u) {
    payload = h2_pal_mem_alloc(allocator, s_env->response_payload_len);
    assert(payload != NULL);
    memcpy(payload, s_env->response_payload, s_env->response_payload_len);
  }
  *out_response = (h2_gizclaw_rpc_response_t){
      .result_payload = payload,
      .result_payload_len = s_env->response_payload_len,
      .has_error = s_env->rpc_remote_error,
      .error_code = s_env->rpc_remote_error ? 400 : 0,
  };
  return H2_PAL_OK;
}

static const h2_gizclaw_async_rpc_ops_t s_profile_ops = {
    .start = fake_profile_start,
    .result = fake_profile_result,
    .cancel = fake_rpc_cancel,
    .destroy = fake_rpc_destroy,
};

static bool original_cancel_requested(void *user) {
  test_env_t *env = user;
  return atomic_load_explicit(&env->original_cancel, memory_order_acquire);
}

static void receive_event(void *user, const h2_gizclaw_client_event_t *event) {
  test_env_t *env = user;
  assert(pthread_equal(pthread_self(), env->app_thread));
  assert(event != NULL);
  assert(event->kind == H2_GIZCLAW_CLIENT_EVENT_WORKSPACE_HISTORY_UPDATED);
  assert(event->workspace_name.len == 9u);
  assert(memcmp(event->workspace_name.data, "workspace", 9u) == 0);
  assert(event->last_updated_at_unix_ms == 123u);
  atomic_fetch_add_explicit(&env->event_callback_count, 1u,
                            memory_order_release);
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
start_pending(void *user, h2_gizclaw_client_t *client,
              const h2_gizclaw_cancel_token_t *cancel_token) {
  test_env_t *env = user;
  assert(client == (h2_gizclaw_client_t *)env);
  assert(!h2_gizclaw_cancel_requested(cancel_token));
  atomic_fetch_add_explicit(&env->async_start_count, 1u, memory_order_release);
  return H2_PAL_ERR_WOULD_BLOCK;
}

static h2_pal_result_t
poll_pending(void *user, h2_gizclaw_client_t *client,
             const h2_gizclaw_cancel_token_t *cancel_token) {
  test_env_t *env = user;
  assert(client == (h2_gizclaw_client_t *)env);
  if (h2_gizclaw_cancel_requested(cancel_token))
    return H2_PAL_ERR_CLOSED;
  const unsigned count = atomic_fetch_add_explicit(&env->async_poll_count, 1u,
                                                   memory_order_release) +
                         1u;
  return count >= 3u ? H2_PAL_OK : H2_PAL_ERR_WOULD_BLOCK;
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

/* Exercise scheduler ordering via its real asynchronous admission path. */
static h2_pal_result_t
submit_test_operation(h2_gizclaw_service_t *service, uint64_t identity,
                      h2_gizclaw_operation_run_fn run,
                      h2_gizclaw_operation_completion_fn completion, void *user,
                      h2_gizclaw_operation_t **out_operation) {
  return h2_gizclaw_service_submit_async_internal(
      service, identity, run, run, completion, user, out_operation);
}

static void record_completion(void *user, h2_gizclaw_operation_t *operation,
                              const h2_gizclaw_operation_result_t *result) {
  test_env_t *env = user;
  assert(pthread_equal(pthread_self(), env->app_thread));
  assert(&operation->result == result);
  const size_t index =
      atomic_load_explicit(&env->completion_count, memory_order_relaxed);
  assert(index < 8u);
  env->completed[index] = result->identity;
  env->terminal_kinds[index] = result->terminal_kind;
  env->completion_results[index] = result->result;
  atomic_store_explicit(&env->completion_count, index + 1u,
                        memory_order_release);
  if (env->release_in_completion)
    h2_gizclaw_operation_release(operation);
}

static void record_req_completion(void *user, h2_gizclaw_req_t *request,
                                  const h2_gizclaw_operation_result_t *result) {
  test_env_t *env = user;
  assert(pthread_equal(pthread_self(), env->app_thread));
  assert(request != NULL && result != NULL);
  const size_t index =
      atomic_load_explicit(&env->completion_count, memory_order_relaxed);
  assert(index < 8u);
  env->completed[index] = result->identity;
  env->terminal_kinds[index] = result->terminal_kind;
  env->completion_results[index] = result->result;
  atomic_store_explicit(&env->completion_count, index + 1u,
                        memory_order_release);
  if (env->release_in_completion)
    h2_gizclaw_req_release(request);
}

static void ignore_notification(void *user) { (void)user; }

static void
submit_and_cancel_from_completion(void *user, h2_gizclaw_operation_t *operation,
                                  const h2_gizclaw_operation_result_t *result) {
  test_env_t *env = user;
  assert(result->identity == 40u);
  assert(result->terminal_kind == H2_GIZCLAW_OPERATION_FINISHED);
  record_completion(user, operation, result);
  h2_gizclaw_operation_t *blocker = NULL;
  assert(submit_test_operation(env->service, 42u, run_until_released,
                               record_completion, env, &blocker) == H2_PAL_OK);
  h2_gizclaw_operation_t *next = NULL;
  assert(submit_test_operation(env->service, 41u, run_immediate,
                               record_completion, env, &next) == H2_PAL_OK);
  assert(h2_gizclaw_operation_cancel(next) == H2_PAL_OK);
  assert(h2_gizclaw_operation_cancel(next) == H2_PAL_OK);
  atomic_store_explicit(&env->run_gate, true, memory_order_release);
}

static void record_terminal(void *user, h2_pal_result_t result) {
  test_env_t *env = user;
  assert(pthread_equal(pthread_self(), env->app_thread));
  atomic_fetch_add_explicit(&env->terminal_count, 1u, memory_order_release);
  env->terminal_result = result;
}

static void wait_for_count_at(atomic_uint *value, unsigned expected,
                              const char *function, int line) {
  uint64_t started = 0u, now = 0u;
  assert(h2_pal_time_get_monotonic_ms(h2_desktop_platform_time_api(),
                                      &started) == H2_PAL_OK);
  do {
    if (atomic_load_explicit(value, memory_order_acquire) >= expected)
      return;
    assert(h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 1u) ==
           H2_PAL_OK);
    assert(h2_pal_time_get_monotonic_ms(h2_desktop_platform_time_api(), &now) ==
           H2_PAL_OK);
  } while (now - started < 5000u);
  fprintf(stderr, "%s:%d: worker count=%u expected=%u\n", function, line,
          atomic_load(value), expected);
  assert(false && "worker did not make progress");
}

#define wait_for_count(value, expected)                                        \
  wait_for_count_at((value), (expected), __func__, __LINE__)

static void wait_until(test_env_t *env, size_t completion_count,
                       unsigned terminal_count,
                       unsigned progress_callback_count) {
  for (unsigned spin = 0u; spin < 1000000u; ++spin) {
    size_t dispatched = 0u;
    assert(h2_gizclaw_service_poll(env->service, 8u, &dispatched) == H2_PAL_OK);
    if (atomic_load_explicit(&env->completion_count, memory_order_acquire) >=
            completion_count &&
        atomic_load_explicit(&env->terminal_count, memory_order_acquire) >=
            terminal_count &&
        atomic_load_explicit(&env->progress_callback_count,
                             memory_order_acquire) >= progress_callback_count) {
      return;
    }
    sched_yield();
  }
  assert(false && "caller did not dispatch callback");
}

static const char s_wait_rpc_tag;

static void *run_sync_rpc(void *user) {
  test_env_t *env = user;
  env->sync_rpc_result = h2_gizclaw_req_create_rpc_internal(
      env->service, 70u, H2_GIZCLAW_RPC_SERVER_WORKFLOW_GET, &s_wait_rpc_tag,
      (h2_gizclaw_rpc_bytes_t){.data = (const uint8_t *)"req", .len = 3u},
      1234u, &env->sync_rpc);
  if (env->sync_rpc_result == H2_PAL_OK)
    env->sync_rpc_result = h2_gizclaw_req_do(env->sync_rpc, env, NULL, NULL,
                                             record_req_completion);
  if (env->sync_rpc_result == H2_PAL_OK)
    env->sync_rpc_result = h2_gizclaw_req_wait(env->sync_rpc, 5000u);
  atomic_store_explicit(&env->sync_rpc_returned, true, memory_order_release);
  return NULL;
}

static int fail_task_start(void *user, const h2_pal_task_options_t *options,
                           h2_pal_task_entry_t entry, void *ctx,
                           h2_pal_task_t **out_task) {
  (void)user;
  assert(options != NULL);
  assert(strcmp(options->name, h2_gizclaw_net_task_name) == 0);
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
  env->release_in_completion = true;
  env->app_thread = pthread_self();
  atomic_init(&env->connect_gate, true);
  atomic_init(&env->event_dispatch_gate, true);
  atomic_init(&env->run_gate, false);
  s_env = env;
  h2_gizclaw_service_test_set_client_ops(&s_client_ops);

  static h2_gizclaw_config_t client_config;
  memset(&client_config, 0, sizeof(client_config));
  client_config.allocator = h2_desktop_platform_default_allocator();
  client_config.time = h2_desktop_platform_time_api();
  client_config.cancel_requested = original_cancel_requested;
  client_config.cancel_user = env;
  h2_gizclaw_service_config_t config = {
      .client_config = &client_config,
      .task = h2_desktop_platform_task_api(),
      .queue = &s_queue_api,
      .sync = h2_desktop_platform_sync_api(),
      .net_task_options = {.name = "gizclaw-net-test", .min_stack_size = 0u},
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
  assert(submit_test_operation(service, 1u, run_immediate, record_completion,
                               &env, &first) == H2_PAL_OK);
  assert(submit_test_operation(service, 2u, run_immediate, record_completion,
                               &env, &second) == H2_PAL_OK);
  assert(submit_test_operation(service, 3u, run_immediate, record_completion,
                               &env, &rejected) == H2_PAL_ERR_WOULD_BLOCK);
  assert(rejected == NULL);
  wait_for_count(&env.run_count, 2u);
  assert(atomic_load_explicit(&env.event_handler_count, memory_order_acquire) ==
         1u);
  assert(atomic_load_explicit(&env.event_dispatch_count, memory_order_acquire) >
         0u);
  wait_until(&env, 2u, 0u, 0u);
  assert(atomic_load_explicit(&env.event_callback_count,
                              memory_order_acquire) == 1u);
  assert(env.completed[0] == 1u);
  assert(env.completed[1] == 2u);

  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(atomic_load(&env.close_count) == 1u);
  assert(atomic_load(&env.deinit_count) == 1u);
  assert(atomic_load(&env.cleanup_count) == 1u);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

static void test_runtime_dispatch_wakeup_is_coalesced(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_service(&env, 2u);
  service->config.runtime = (h2_runtime_t *)&env;
  service->config.on_event = NULL;
  atomic_store_explicit(&s_runtime_post_count, 0u, memory_order_relaxed);
  s_runtime_event_id = 0u;
  h2_gizclaw_service_test_set_runtime_post(fake_runtime_post);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);

  h2_gizclaw_operation_t *first = NULL;
  h2_gizclaw_operation_t *second = NULL;
  assert(submit_test_operation(service, 101u, run_immediate, record_completion,
                               &env, &first) == H2_PAL_OK);
  assert(submit_test_operation(service, 102u, run_immediate, record_completion,
                               &env, &second) == H2_PAL_OK);
  bool both_ready = false;
  for (size_t attempt = 0u; attempt < 1000000u; ++attempt) {
    assert(h2_pal_mutex_lock(service->config.sync, service->mutex) ==
           H2_PAL_OK);
    const size_t ready = service->dispatch_item_count;
    assert(h2_pal_mutex_unlock(service->config.sync, service->mutex) ==
           H2_PAL_OK);
    if (ready >= 2u) {
      both_ready = true;
      break;
    }
    sched_yield();
  }
  assert(both_ready);
  assert(atomic_load_explicit(&s_runtime_post_count, memory_order_acquire) ==
         1u);
  assert(s_runtime_event_id == H2_GIZCLAW_RUNTIME_EVENT_DISPATCH_READY);

  size_t dispatched = 0u;
  assert(h2_gizclaw_service_poll(service, 1u, &dispatched) == H2_PAL_OK);
  assert(dispatched == 1u);
  assert(atomic_load_explicit(&s_runtime_post_count, memory_order_acquire) ==
         2u);
  assert(h2_gizclaw_service_poll(service, 2u, &dispatched) == H2_PAL_OK);
  assert(dispatched == 1u);
  assert(atomic_load_explicit(&env.completion_count, memory_order_acquire) ==
         2u);

  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
  h2_gizclaw_service_test_set_runtime_post(NULL);
}

static void test_pending_operation_does_not_block_following_work(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_service(&env, 2u);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);

  h2_gizclaw_operation_t *pending = NULL;
  assert(h2_gizclaw_service_submit_async_internal(
             service, 4u, start_pending, poll_pending, record_completion, &env,
             &pending) == H2_PAL_OK);
  wait_for_count(&env.async_start_count, 1u);

  h2_gizclaw_operation_t *immediate = NULL;
  assert(submit_test_operation(service, 5u, run_immediate, record_completion,
                               &env, &immediate) == H2_PAL_OK);
  wait_for_count(&env.run_count, 1u);
  wait_for_count(&env.async_poll_count, 3u);
  wait_until(&env, 2u, 0u, 0u);
  assert(env.terminal_kinds[0] == H2_GIZCLAW_OPERATION_FINISHED);
  assert(env.terminal_kinds[1] == H2_GIZCLAW_OPERATION_FINISHED);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

static void test_queued_cancel_and_stop_drain(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_service(&env, 3u);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  h2_gizclaw_operation_t *first = NULL;
  h2_gizclaw_operation_t *second = NULL;
  assert(submit_test_operation(service, 10u, run_until_released,
                               record_completion, &env, &first) == H2_PAL_OK);
  assert(submit_test_operation(service, 11u, run_immediate, record_completion,
                               &env, &second) == H2_PAL_OK);
  wait_for_count(&env.run_count, 1u);
  assert(h2_gizclaw_operation_cancel(second) == H2_PAL_OK);
  atomic_store_explicit(&env.run_gate, true, memory_order_release);
  wait_for_count(&env.run_count, 1u);
  wait_until(&env, 2u, 0u, 0u);
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
  assert(submit_test_operation(service, 20u, run_until_released,
                               record_completion, &env,
                               &operation) == H2_PAL_OK);
  wait_for_count(&env.run_count, 1u);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  wait_until(&env, 1u, 0u, 0u);
  assert(atomic_load_explicit(&env.completion_count, memory_order_acquire) ==
         1u);
  assert(env.terminal_kinds[0] == H2_GIZCLAW_OPERATION_SERVICE_CLOSED);
  assert(atomic_load_explicit(&env.terminal_count, memory_order_acquire) == 0u);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

static void test_connect_failure_is_terminal(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_service(&env, 1u);
  env.connect_result = H2_PAL_ERR_IO;
  atomic_store_explicit(&env.connect_gate, false, memory_order_release);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  h2_gizclaw_operation_t *operation = NULL;
  assert(submit_test_operation(service, 30u, run_immediate, record_completion,
                               &env, &operation) == H2_PAL_OK);
  atomic_store_explicit(&env.connect_gate, true, memory_order_release);
  wait_for_count(&env.connect_count, 1u);
  wait_until(&env, 1u, 1u, 0u);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(env.terminal_kinds[0] == H2_GIZCLAW_OPERATION_SERVICE_CLOSED);
  assert(atomic_load_explicit(&env.terminal_count, memory_order_acquire) == 1u);
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
  assert(submit_test_operation(service, 33u, run_immediate, record_completion,
                               &env, &operation) == H2_PAL_OK);
  atomic_store_explicit(&env.event_dispatch_gate, true, memory_order_release);

  wait_for_count(&env.cleanup_count, 1u);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(atomic_load_explicit(&env.run_count, memory_order_acquire) == 0u);
  assert(atomic_load_explicit(&env.close_count, memory_order_acquire) == 1u);
  assert(atomic_load_explicit(&env.deinit_count, memory_order_acquire) == 1u);
  assert(atomic_load_explicit(&env.cleanup_count, memory_order_acquire) == 1u);

  wait_until(&env, 1u, 1u, 0u);
  assert(env.terminal_kinds[0] == H2_GIZCLAW_OPERATION_SERVICE_CLOSED);
  assert(env.completion_results[0] == H2_PAL_ERR_CLOSED);
  assert(atomic_load_explicit(&env.terminal_count, memory_order_acquire) == 1u);
  assert(env.terminal_result == H2_PAL_ERR_IO);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

static void test_operation_error_and_transport_closed(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_service(&env, 2u);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  h2_gizclaw_operation_t *operation = NULL;
  assert(submit_test_operation(service, 31u, run_io_failure, record_completion,
                               &env, &operation) == H2_PAL_OK);
  wait_for_count(&env.run_count, 1u);
  wait_until(&env, 1u, 0u, 0u);
  assert(env.terminal_kinds[0] == H2_GIZCLAW_OPERATION_FINISHED);
  assert(env.completion_results[0] == H2_PAL_ERR_IO);
  assert(atomic_load_explicit(&env.terminal_count, memory_order_acquire) == 0u);

  operation = NULL;
  assert(submit_test_operation(service, 32u, run_transport_closed,
                               record_completion, &env,
                               &operation) == H2_PAL_OK);
  wait_for_count(&env.run_count, 2u);
  wait_until(&env, 2u, 0u, 0u);
  assert(env.terminal_kinds[1] == H2_GIZCLAW_OPERATION_FINISHED);
  assert(env.completion_results[1] == H2_PAL_ERR_CLOSED);
  assert(atomic_load_explicit(&env.terminal_count, memory_order_acquire) == 0u);
  assert(submit_test_operation(service, 34u, run_immediate, record_completion,
                               &env, &operation) == H2_PAL_OK);
  wait_until(&env, 3u, 0u, 0u);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

static void test_running_cancel_is_idempotent_and_late_safe(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_service(&env, 1u);
  env.release_in_completion = false;
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  h2_gizclaw_operation_t *operation = NULL;
  assert(submit_test_operation(service, 35u, run_until_released,
                               record_completion, &env,
                               &operation) == H2_PAL_OK);
  wait_for_count(&env.run_count, 1u);
  assert(h2_gizclaw_operation_cancel(operation) == H2_PAL_OK);
  assert(h2_gizclaw_operation_cancel(operation) == H2_PAL_OK);
  wait_for_count(&env.run_exit_count, 1u);
  assert(h2_gizclaw_operation_cancel(operation) == H2_PAL_OK);
  wait_until(&env, 1u, 0u, 0u);
  assert(env.terminal_kinds[0] == H2_GIZCLAW_OPERATION_CANCELED);
  h2_gizclaw_operation_release(operation);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

static void test_callback_can_submit_cancel_and_release(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_service(&env, 3u);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  h2_gizclaw_operation_t *first = NULL;
  assert(submit_test_operation(service, 40u, run_immediate,
                               submit_and_cancel_from_completion, &env,
                               &first) == H2_PAL_OK);
  wait_for_count(&env.run_count, 1u);
  wait_until(&env, 3u, 0u, 0u);
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
  wait_until(&env, 0u, 1u, 0u);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(atomic_load_explicit(&env.terminal_count, memory_order_acquire) == 1u);
  assert(env.terminal_result == H2_PAL_ERR_IO);
  assert(atomic_load(&env.init_count) == 0u);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);

  service = create_service(&env, 1u);
  env.init_result = H2_PAL_ERR_NO_MEMORY;
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  wait_for_count(&env.init_count, 1u);
  wait_until(&env, 0u, 1u, 0u);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(atomic_load_explicit(&env.terminal_count, memory_order_acquire) == 1u);
  assert(env.terminal_result == H2_PAL_ERR_NO_MEMORY);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);

  service = create_service(&env, 1u);
  env.poll_result = H2_PAL_ERR_IO;
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  wait_for_count(&env.poll_count, 1u);
  wait_until(&env, 0u, 1u, 0u);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(atomic_load_explicit(&env.terminal_count, memory_order_acquire) == 1u);
  assert(env.terminal_result == H2_PAL_ERR_IO);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

static void test_original_cancel_and_unstarted_lifecycle(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_service(&env, 1u);
  h2_gizclaw_operation_t *operation = NULL;
  assert(submit_test_operation(service, 50u, run_immediate, record_completion,
                               &env, &operation) == H2_PAL_ERR_INVALID_STATE);
  assert(operation == NULL);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_start(service) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);

  service = create_service(&env, 1u);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  atomic_store_explicit(&env.original_cancel, true, memory_order_release);
  assert(submit_test_operation(service, 51u, run_until_released,
                               record_completion, &env,
                               &operation) == H2_PAL_OK);
  wait_for_count(&env.run_count, 1u);
  wait_for_count(&env.run_exit_count, 1u);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  wait_until(&env, 1u, 1u, 0u);
  assert(atomic_load_explicit(&env.completion_count, memory_order_acquire) ==
         1u);
  assert(atomic_load_explicit(&env.terminal_count, memory_order_acquire) == 1u);
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

static void test_req_wait_does_not_require_callback_dispatch(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_service(&env, 1u);
  env.release_in_completion = false;
  h2_gizclaw_async_rpc_test_set_ops(&s_rpc_ops);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  pthread_t waiter;
  assert(pthread_create(&waiter, NULL, run_sync_rpc, &env) == 0);
  assert(pthread_join(waiter, NULL) == 0);
  assert(env.sync_rpc_result == H2_PAL_OK);
  assert(atomic_load(&env.completion_count) == 0u);
  const h2_gizclaw_rpc_response_t *response = NULL;
  assert(h2_gizclaw_req_response_internal(env.sync_rpc, &s_wait_rpc_tag,
                                          &response) == H2_PAL_OK);
  assert(response != NULL && response->result_payload_len == 3u);
  assert(memcmp(response->result_payload, "rsp", 3u) == 0);
  assert(atomic_load(&env.rpc_destroy_count) == 1u);
  assert(h2_gizclaw_req_wait(env.sync_rpc, 0u) == H2_PAL_OK);
  for (unsigned spin = 0u;
       spin < 1000000u && atomic_load(&env.completion_count) == 0u; ++spin) {
    size_t dispatched = 0u;
    assert(h2_gizclaw_service_poll(service, 1u, &dispatched) == H2_PAL_OK);
    if (dispatched == 0u)
      sched_yield();
  }
  assert(atomic_load(&env.completion_count) == 1u);
  h2_gizclaw_req_release(env.sync_rpc);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
  h2_gizclaw_async_rpc_test_set_ops(NULL);
}

static h2_gizclaw_service_t *create_profile_service(test_env_t *env) {
  static const uint8_t response[] = {0x0a, 3, 0x12, 1, 'A'};
  h2_gizclaw_service_t *service = create_service(env, 4u);
  atomic_store(&env->event_emitted, true);
  env->expected_method = H2_GIZCLAW_RPC_SERVER_INFO_GET;
  env->response_payload = response;
  env->response_payload_len = sizeof(response);
  atomic_store(&env->run_gate, true);
  h2_gizclaw_async_rpc_test_set_ops(&s_profile_ops);
  return service;
}

static void test_firmware_public_request_paths(void) {
  for (unsigned mode = 0; mode < 13; ++mode) {
    test_env_t env;
    h2_gizclaw_service_t *service = create_profile_service(&env);
    const int32_t channel = mode == 1 ? 1000 : mode == 2 ? INT32_MAX : 3;
    gizclaw_rpc_v1_FirmwareGetRequest params = {
        .channel = (gizclaw_rpc_v1_FirmwareChannelName)channel};
    uint8_t input[6], payload[4096];
    pb_ostream_t encoded = pb_ostream_from_buffer(input, sizeof(input));
    assert(
        pb_encode(&encoded, gizclaw_rpc_v1_FirmwareGetRequest_fields, &params));
    env.expected_method = H2_GIZCLAW_RPC_SERVER_FIRMWARE_GET;
    env.expected_payload = input;
    env.expected_payload_len = encoded.bytes_written;
    gizclaw_rpc_v1_FirmwareGetResponse message =
        gizclaw_rpc_v1_FirmwareGetResponse_init_zero;
    message.channel = params.channel;
    message.size = 123;
    message.has_description = mode != 12;
    memset(message.description, 'd', sizeof(message.description) - 1);
    memset(message.url, 'u', sizeof(message.url) - 1);
    memcpy(message.url, "https://", 8);
    memset(message.sha256, 'a', 64);
    if (mode == 3)
      message.size = -1;
    if (mode == 4)
      message.channel = 0;
    if (mode == 5)
      message.sha256[0] = 'g';
    if (mode == 6)
      message.sha256[63] = 0;
    if (mode == 7)
      message.url[0] = 0;
    encoded = pb_ostream_from_buffer(payload, sizeof(payload));
    assert(pb_encode(&encoded, gizclaw_rpc_v1_FirmwareGetResponse_fields,
                     &message));
    env.response_payload = payload;
    env.response_payload_len = encoded.bytes_written - (mode == 8 ? 1 : 0);
    if (mode == 11)
      env.response_payload_len = 0;
    env.rpc_remote_error = mode == 9;
    if (mode == 10)
      env.rpc_result = H2_PAL_ERR_IO;
    h2_gizclaw_req_t *request = NULL;
    h2_gizclaw_firmware_t result, empty = {0};
    assert(h2_gizclaw_req_create_firmware_get(service, 1, channel, 1234,
                                              &request) == H2_PAL_OK);
    assert(atomic_load(&env.rpc_start_count) == 0);
    assert(h2_gizclaw_resp_parse_firmware_get(request, &result) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(memcmp(&result, &empty, sizeof(result)) == 0);
    h2_gizclaw_req_t *wrong = NULL;
    assert(h2_gizclaw_req_create_profile_get(service, 2, 1234, &wrong) ==
           H2_PAL_OK);
    assert(h2_gizclaw_resp_parse_firmware_get(wrong, &result) ==
           H2_PAL_ERR_INVALID_ARG);
    h2_gizclaw_req_release(wrong);
    assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    const h2_pal_result_t wire_rc = mode == 9    ? H2_GIZCLAW_ERR_REMOTE
                                    : mode == 10 ? H2_PAL_ERR_IO
                                                 : H2_PAL_OK;
    assert(h2_gizclaw_req_wait(request, 2000) == wire_rc);
    const h2_pal_result_t expected = mode <= 2 || mode == 12 ? H2_PAL_OK
                                     : mode == 9 || mode == 10
                                         ? wire_rc
                                         : H2_PAL_ERR_FORMAT;
    assert(h2_gizclaw_resp_parse_firmware_get(request, &result) == expected);
    h2_gizclaw_req_release(request);
    if (expected == H2_PAL_OK) {
      assert(result.channel == channel && result.size == 123);
      assert(result.has_description == message.has_description);
      assert(memcmp(result.url, message.url, sizeof(result.url)) == 0);
      assert(memcmp(result.sha256, message.sha256, sizeof(result.sha256)) == 0);
      assert(strlen(result.description) ==
             (message.has_description ? 1024u : 0u));
    } else {
      assert(memcmp(&result, &empty, sizeof(result)) == 0);
    }
    memset(&result, 0xa5, sizeof(result));
    assert(h2_gizclaw_rpc_firmware_get(service, channel, 1234, &result) ==
           expected);
    assert(atomic_load(&env.rpc_start_count) == 2);
    assert(atomic_load(&env.rpc_destroy_count) == 2);
    if (expected != H2_PAL_OK)
      assert(memcmp(&result, &empty, sizeof(result)) == 0);
    assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
    assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
    if (expected == H2_PAL_OK)
      assert(result.channel == channel && strcmp(result.url, message.url) == 0);
  }
  h2_gizclaw_req_t *invalid = (h2_gizclaw_req_t *)1;
  assert(h2_gizclaw_req_create_firmware_get(NULL, 0, 0, 1000, &invalid) ==
             H2_PAL_ERR_INVALID_ARG &&
         invalid == NULL);
  assert(h2_gizclaw_req_create_firmware_get(NULL, 0, -1, 1000, &invalid) ==
             H2_PAL_ERR_INVALID_ARG &&
         invalid == NULL);
  assert(h2_gizclaw_req_create_firmware_get(NULL, 0, 3, 1000, &invalid) ==
             H2_PAL_ERR_INVALID_ARG &&
         invalid == NULL);
  assert(h2_gizclaw_resp_parse_firmware_get(NULL, NULL) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_rpc_firmware_get(NULL, 3, 1000, NULL) ==
         H2_PAL_ERR_INVALID_ARG);
}

typedef struct stop_join_test {
  test_env_t *env;
  h2_pal_task_t *tasks[5];
  unsigned starts;
  unsigned start_attempts;
  bool fail_start;
  unsigned start_failure_index;
  unsigned attempts[5];
  unsigned joined[5];
  unsigned failure_index;
  unsigned failures_remaining;
  unsigned notifications;
  unsigned callbacks;
} stop_join_test_t;

static int stop_join_start(void *user, const h2_pal_task_options_t *options,
                           h2_pal_task_entry_t entry, void *ctx,
                           h2_pal_task_t **out) {
  stop_join_test_t *test = user;
  assert(test->starts < 5);
  ++test->start_attempts;
  if (test->fail_start && test->starts == test->start_failure_index) {
    *out = NULL;
    return H2_PAL_ERR_NO_MEMORY;
  }
  static const char *const names[] = {h2_gizclaw_net_task_name,
                                      h2_gizclaw_audio_uplink_task_name,
                                      h2_gizclaw_audio_downlink_task_name,
                                      "$gizclaw/data-up", "$gizclaw/data-down"};
  assert(strcmp(options->name, names[test->starts]) == 0);
  int rc = h2_pal_task_start(h2_desktop_platform_task_api(), options, entry,
                             ctx, out);
  if (rc == H2_PAL_OK)
    test->tasks[test->starts++] = *out;
  return rc;
}

static int stop_join_retry(void *user, h2_pal_task_t *task) {
  stop_join_test_t *test = user;
  unsigned index = 0;
  while (index < 5 && test->tasks[index] != task)
    ++index;
  assert(index < 5 && test->joined[index] == 0);
  ++test->attempts[index];
  if (index == test->failure_index && test->failures_remaining != 0) {
    --test->failures_remaining;
    return H2_PAL_ERR_TASK;
  }
  int rc = h2_pal_task_join(h2_desktop_platform_task_api(), task);
  if (rc == H2_PAL_OK)
    ++test->joined[index];
  return rc;
}

static void stop_join_notification(void *user) {
  stop_join_test_t *test = user;
  assert(pthread_equal(pthread_self(), test->env->app_thread));
  assert(test->callbacks == 0);
  ++test->notifications;
}

static __attribute__((unused)) void
test_service_stop_join_retry_preserves_pending_callbacks(void) {
  for (unsigned failure = 0; failure < 5; ++failure) {
    test_env_t env;
    h2_gizclaw_service_t *service = create_profile_service(&env);
    atomic_store(&env.run_gate, false);
    stop_join_test_t test = {
        .env = &env, .failure_index = failure, .failures_remaining = 2};
    const h2_pal_task_vtable_t vt = {.start = stop_join_start,
                                     .join = stop_join_retry};
    const h2_pal_task_api_t tasks = {.user = &test, .vtable = &vt};
    service->config.task = &tasks;
    assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
    assert(test.starts == 5);
    for (unsigned i = 0; i < 4; ++i)
      assert(h2_gizclaw_service_post_internal(service, stop_join_notification,
                                              &test) == H2_PAL_OK);
    assert(h2_gizclaw_service_post_internal(service, stop_join_notification,
                                            &test) == H2_PAL_ERR_WOULD_BLOCK);
    h2_gizclaw_req_t *request = NULL;
    assert(h2_gizclaw_req_create_profile_get(service, 1, 1234, &request) ==
           H2_PAL_OK);
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    wait_for_count(&env.rpc_start_count, 1);
    for (unsigned retry = 0; retry < 2; ++retry) {
      assert(h2_gizclaw_service_stop(service) == H2_PAL_ERR_TASK);
      assert(!service->stopped);
      h2_pal_task_t *retained[] = {
          service->net_task, service->uplink_task, service->downlink_task,
          service->data_uplink_task, service->data_downlink_task};
      for (unsigned i = 0; i < 5; ++i) {
        assert(retained[i] == (i < failure ? NULL : test.tasks[i]));
        assert(test.joined[i] == (i < failure ? 1u : 0u));
      }
      assert(h2_gizclaw_service_deinit(service) == H2_PAL_ERR_INVALID_STATE);
      assert(test.callbacks == 0 && test.notifications == 0);
    }
    // Stop settles the request even while all notification slots are full.
    assert(h2_gizclaw_req_wait(request, 2000) == H2_PAL_ERR_CLOSED);
    h2_gizclaw_req_release(request);
    request = NULL;
    assert(h2_gizclaw_req_create_profile_get(service, 2, 1234, &request) ==
           H2_PAL_ERR_CLOSED);
    assert(request == NULL);
    assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
    assert(service->net_task == NULL && service->uplink_task == NULL &&
           service->downlink_task == NULL &&
           service->data_uplink_task == NULL &&
           service->data_downlink_task == NULL);
    assert(atomic_load(&env.rpc_destroy_count) == 1);
    assert(atomic_load(&env.cleanup_count) == 1);
    assert(atomic_load(&env.close_count) == 1);
    assert(atomic_load(&env.deinit_count) == 1);
    assert(h2_gizclaw_service_deinit(service) == H2_PAL_ERR_INVALID_STATE);
    for (unsigned i = 0; i < 5; ++i) {
      size_t dispatched = 0;
      assert(h2_gizclaw_service_poll(service, 1, &dispatched) == H2_PAL_OK);
      assert(dispatched == 1);
    }
    assert(test.callbacks == 1 && test.notifications == 4);
    assert(atomic_load(&env.terminal_count) == 0);
    size_t dispatched = 99;
    assert(h2_gizclaw_service_poll(service, 8, &dispatched) == H2_PAL_OK);
    assert(dispatched == 0);
    assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
    for (unsigned i = 0; i < 5; ++i) {
      assert(test.joined[i] == 1);
      assert(test.attempts[i] == (i == failure ? 3u : 1u));
    }
    assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
  }
}

static void test_service_partial_start_and_join_failures(void) {
  for (unsigned failed_start = 1; failed_start < 5; ++failed_start) {
    for (unsigned failed_join = 0; failed_join < failed_start; ++failed_join) {
      test_env_t env;
      h2_gizclaw_service_t *service = create_profile_service(&env);
      stop_join_test_t test = {.env = &env,
                               .fail_start = true,
                               .start_failure_index = failed_start,
                               .failure_index = failed_join,
                               .failures_remaining = 2};
      const h2_pal_task_vtable_t vt = {.start = stop_join_start,
                                       .join = stop_join_retry};
      const h2_pal_task_api_t tasks = {.user = &test, .vtable = &vt};
      service->config.task = &tasks;
      h2_gizclaw_req_t *request = NULL;
      assert(h2_gizclaw_req_create_profile_get(service, 1, 1234, &request) ==
             H2_PAL_OK);
      // Preserve the original start error, even if automatic stop also fails.
      assert(h2_gizclaw_service_start(service) == H2_PAL_ERR_NO_MEMORY);
      assert(test.starts == failed_start &&
             test.start_attempts == failed_start + 1);
      assert(h2_gizclaw_service_start(service) == H2_PAL_ERR_INVALID_STATE);
      assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) ==
             H2_PAL_ERR_CLOSED);
      assert(h2_gizclaw_service_deinit(service) == H2_PAL_ERR_INVALID_STATE);
      assert(h2_gizclaw_service_stop(service) == H2_PAL_ERR_TASK);
      assert(!service->stopped);
      h2_pal_task_t *retained[] = {
          service->net_task, service->uplink_task, service->downlink_task,
          service->data_uplink_task, service->data_downlink_task};
      for (unsigned i = 0; i < 5; ++i)
        assert(retained[i] ==
               (i >= failed_join && i < failed_start ? test.tasks[i] : NULL));
      assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
      assert(service->net_task == NULL && service->uplink_task == NULL &&
             service->downlink_task == NULL &&
             service->data_uplink_task == NULL &&
             service->data_downlink_task == NULL);
      for (unsigned i = 0; i < 5; ++i) {
        assert(test.joined[i] == (i < failed_start ? 1u : 0u));
        assert(test.attempts[i] == (i == failed_join   ? 3u
                                    : i < failed_start ? 1u
                                                       : 0u));
      }
      size_t dispatched = 99;
      assert(h2_gizclaw_service_poll(service, 1, &dispatched) == H2_PAL_OK);
      assert(dispatched == 0 && atomic_load(&env.terminal_count) == 0);
      assert(atomic_load(&env.rpc_start_count) == 0);
      // A rejected do leaves ownership with the caller, including after stop.
      assert(h2_gizclaw_service_deinit(service) == H2_PAL_ERR_INVALID_STATE);
      h2_gizclaw_req_release(request);
      assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
    }
  }
}

static void test_service_terminal_callback_obeys_poll_budget(void) {
  for (unsigned mode = 0; mode < 8; ++mode) {
    const unsigned with_request = mode % 2;
    const unsigned with_terminal = mode < 4;
    const size_t budget = mode % 4 < 2 ? 1 : 2;
    test_env_t env;
    h2_gizclaw_service_t *service = create_profile_service(&env);
    if (!with_terminal)
      service->config.terminal = NULL;
    atomic_store(&env.connect_gate, false);
    env.connect_result = H2_PAL_ERR_IO;
    assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
    if (with_request) {
      h2_gizclaw_req_t *request = NULL;
      assert(h2_gizclaw_req_create_profile_get(service, 1, 1234, &request) ==
             H2_PAL_OK);
      assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
      h2_gizclaw_req_release(request);
    }
    atomic_store(&env.connect_gate, true);
    wait_for_count(&env.cleanup_count, 1);
    assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
    assert(atomic_load(&env.terminal_count) == 0);
    const size_t expected = with_terminal;
    size_t total = 0;
    do {
      size_t dispatched = 99;
      const size_t before =
          atomic_load(&env.completion_count) + atomic_load(&env.terminal_count);
      assert(h2_gizclaw_service_poll(service, budget, &dispatched) ==
             H2_PAL_OK);
      const size_t after =
          atomic_load(&env.completion_count) + atomic_load(&env.terminal_count);
      const size_t remaining = expected - total;
      assert(dispatched == (remaining < budget ? remaining : budget));
      assert(after - before == dispatched);
      total += dispatched;
    } while (total < expected);
    assert(atomic_load(&env.completion_count) == 0u);
    assert(atomic_load(&env.terminal_count) == with_terminal);
    if (with_terminal)
      assert(env.terminal_result == H2_PAL_ERR_IO);
    // An absent terminal hook consumes no budget and needs no extra empty poll
    // to retire its terminal marker, even when the last request used the
    // budget.
    assert(service->terminal_dispatched);
    size_t dispatched = 99;
    assert(h2_gizclaw_service_poll(service, 1, &dispatched) == H2_PAL_OK);
    assert(dispatched == 0 &&
           atomic_load(&env.terminal_count) == with_terminal);
    assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
  }
}

static void test_req_profile_no_poll_and_copied_results(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_profile_service(&env);
  atomic_store(&env.run_gate, false);
  service->config.operation_capacity = 1u;
  h2_gizclaw_req_t *request = NULL;
  h2_gizclaw_profile_t profile;
  assert(h2_gizclaw_req_create_profile_get(service, 1u, 1234u, &request) ==
         H2_PAL_OK);
  assert(atomic_load(&env.rpc_start_count) == 0u);
  assert(h2_gizclaw_req_wait(request, 0u) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_gizclaw_resp_parse_profile_get(request, &profile) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  h2_pal_result_t submit_result =
      h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (submit_result != H2_PAL_OK)
    fprintf(stderr, "profile do returned %d after start\n", submit_result);
  assert(submit_result == H2_PAL_OK);
  assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) ==
         H2_PAL_ERR_INVALID_STATE);
  wait_for_count(&env.rpc_start_count, 1u);
  assert(h2_gizclaw_req_wait(request, 0u) == H2_PAL_ERR_TIMEOUT);
  /* The response is deliberately held: finishing a bodyless request must not
   * cancel or prematurely complete it, nor require application polling. */
  assert(h2_gizclaw_req_wait(request, 0u) == H2_PAL_ERR_TIMEOUT);
  atomic_store(&env.run_gate, true);
  assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK);
  assert(h2_gizclaw_req_wait(request, 0u) == H2_PAL_OK);
  assert(h2_gizclaw_resp_parse_profile_put_name(request, &profile) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_resp_parse_profile_get(request, &profile) == H2_PAL_OK);
  assert(profile.has_name && strcmp(profile.name, "A") == 0);
  h2_gizclaw_req_release(request);
  assert(strcmp(profile.name, "A") == 0);
  /* The synchronous wrapper must complete on this same app thread, without
   * any external service_poll task. */
  for (unsigned i = 0u; i < 100u; ++i)
    assert(h2_gizclaw_rpc_profile_get(service, 1234u, &profile) == H2_PAL_OK);
  assert(strcmp(profile.name, "A") == 0);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

static void test_req_callback_reference_and_independent_wait(void) {
  for (unsigned release_early = 0u; release_early < 2u; ++release_early) {
    test_env_t env;
    h2_gizclaw_service_t *service = create_profile_service(&env);
    env.release_in_completion = !release_early;
    assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
    h2_gizclaw_req_t *request = NULL;
    assert(h2_gizclaw_req_create_profile_get(service, 1u, 1234u, &request) ==
           H2_PAL_OK);
    assert(h2_gizclaw_req_do(request, &env, NULL, NULL,
                             record_req_completion) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK);
    assert(atomic_load(&env.completion_count) == 0u);
    if (release_early)
      h2_gizclaw_req_release(request);
    assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
    assert(h2_gizclaw_service_deinit(service) == H2_PAL_ERR_INVALID_STATE);
    size_t dispatched = 0u;
    assert(h2_gizclaw_service_poll(service, 8u, &dispatched) == H2_PAL_OK);
    assert(dispatched == 1u && atomic_load(&env.completion_count) == 1u);
    assert(h2_gizclaw_service_poll(service, 8u, &dispatched) == H2_PAL_OK);
    assert(dispatched == 0u);
    assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
  }
}

static void test_req_dispatch_queue_full_drops_hook_and_settles(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_profile_service(&env);
  atomic_store(&env.run_gate, false);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  h2_gizclaw_req_t *request = NULL;
  assert(h2_gizclaw_req_create_profile_get(service, 77u, 1234u, &request) ==
         H2_PAL_OK);
  env.release_in_completion = false;
  assert(h2_gizclaw_req_do(request, &env, NULL, NULL, record_req_completion) ==
         H2_PAL_OK);
  wait_for_count(&env.rpc_start_count, 1u);

  const size_t capacity = service->config.operation_capacity * 2u + 1u;
  const h2_gizclaw_dispatch_item_t item = {
      .kind = H2_GIZCLAW_DISPATCH_NOTIFICATION,
      .notify = ignore_notification,
  };
  assert(h2_pal_mutex_lock(service->config.sync, service->mutex) == H2_PAL_OK);
  for (size_t i = 0u; i < capacity; ++i) {
    assert(h2_pal_queue_send(service->config.queue, service->dispatch_queue,
                             &item, H2_PAL_QUEUE_NO_WAIT) == H2_PAL_OK);
    ++service->queued_event_count;
    ++service->dispatch_item_count;
  }
  assert(h2_pal_mutex_unlock(service->config.sync, service->mutex) ==
         H2_PAL_OK);

  atomic_store(&env.run_gate, true);
  assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_ERR_WOULD_BLOCK);
  assert(atomic_load(&env.completion_count) == 0u);
  h2_gizclaw_req_release(request);
  size_t dispatched = 0u;
  assert(h2_gizclaw_service_poll(service, capacity, &dispatched) == H2_PAL_OK);
  assert(dispatched == capacity && atomic_load(&env.completion_count) == 0u);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(service->active_count == 0u && service->request_reference_count == 0u);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

static void test_req_profile_put_copies_input_and_checks_parser(void) {
  for (unsigned emoji = 0u; emoji < 2u; ++emoji) {
    test_env_t env;
    h2_gizclaw_service_t *service = create_profile_service(&env);
    const uint8_t expected[] = {0x0a, 3, emoji ? 0x12 : 0x0a, 1, 'B'};
    env.expected_method = H2_GIZCLAW_RPC_SERVER_INFO_PUT;
    env.expected_payload = expected;
    env.expected_payload_len = sizeof(expected);
    char text[] = "B";
    h2_gizclaw_req_t *request = NULL;
    assert(
        (emoji ? h2_gizclaw_req_create_profile_put_emoji(
                     service, 1u, (h2_gizclaw_str_t){text, 1u}, 1234u, &request)
               : h2_gizclaw_req_create_profile_put_name(
                     service, 1u, (h2_gizclaw_str_t){text, 1u}, 1234u,
                     &request)) == H2_PAL_OK);
    text[0] = 'X';
    assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK);
    h2_gizclaw_profile_t profile;
    assert(h2_gizclaw_resp_parse_profile_get(request, &profile) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(h2_gizclaw_resp_parse_profile_put_name(request, &profile) ==
           (emoji ? H2_PAL_ERR_INVALID_ARG : H2_PAL_OK));
    assert(h2_gizclaw_resp_parse_profile_put_emoji(request, &profile) ==
           (emoji ? H2_PAL_OK : H2_PAL_ERR_INVALID_ARG));
    h2_gizclaw_req_release(request);
    text[0] = 'B';
    assert((emoji ? h2_gizclaw_rpc_profile_put_emoji(
                        service, (h2_gizclaw_str_t){text, 1u}, 1234u, &profile)
                  : h2_gizclaw_rpc_profile_put_name(
                        service, (h2_gizclaw_str_t){text, 1u}, 1234u,
                        &profile)) == H2_PAL_OK);
    assert(profile.has_name && strcmp(profile.name, "A") == 0);
    assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
    assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
  }
}

static void test_req_cancel_stop_and_created_lifetime(void) {
  for (unsigned mode = 0u; mode < 4u; ++mode) {
    test_env_t env;
    h2_gizclaw_service_t *service = create_profile_service(&env);
    atomic_store(&env.run_gate, false);
    h2_gizclaw_req_t *request = NULL;
    assert(h2_gizclaw_req_create_profile_get(service, 1u, 1234u, &request) ==
           H2_PAL_OK);
    assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
    wait_for_count(&env.init_count, 1u);
    if (mode != 0u) {
      assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
      wait_for_count(&env.rpc_start_count, 1u);
      assert(h2_gizclaw_req_wait(request, 1u) == H2_PAL_ERR_TIMEOUT);
    }
    if (mode < 2u) {
      assert(h2_gizclaw_req_cancel(request) == H2_PAL_OK);
      assert(env.installed_client_cancel != NULL);
      assert(!env.installed_client_cancel(env.installed_client_cancel_user));
      assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_ERR_CLOSED);
      assert(h2_gizclaw_req_cancel(request) == H2_PAL_OK);
    }
    if (mode == 0u) {
      /* Even on a running Service, canceling a newly created request prevents
       * submission. Error parsing clears the output and never starts the wire.
       */
      assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) ==
             H2_PAL_ERR_INVALID_STATE);
      h2_gizclaw_profile_t profile, empty;
      memset(&profile, 0xA5, sizeof(profile));
      memset(&empty, 0, sizeof(empty));
      assert(h2_gizclaw_resp_parse_profile_get(request, &profile) ==
             H2_PAL_ERR_CLOSED);
      assert(memcmp(&profile, &empty, sizeof(profile)) == 0);
      assert(atomic_load(&env.rpc_start_count) == 0u);
    }
    if (mode == 3u) {
      /* Releasing a running request must not cancel it or leak its results. */
      h2_gizclaw_req_release(request);
      atomic_store(&env.run_gate, true);
      wait_for_count(&env.rpc_destroy_count, 1u);
    }
    assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
    if (mode != 3u) {
      assert(h2_gizclaw_service_deinit(service) == H2_PAL_ERR_INVALID_STATE);
      assert(h2_gizclaw_req_wait(request, 0u) == H2_PAL_ERR_CLOSED);
      h2_gizclaw_req_release(request);
    }
    assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
  }
  test_env_t env;
  h2_gizclaw_service_t *service = create_profile_service(&env);
  h2_gizclaw_req_t *request = NULL;
  assert(h2_gizclaw_req_create_profile_get(service, 1u, 1234u, &request) ==
         H2_PAL_OK);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_ERR_INVALID_STATE);
  h2_gizclaw_req_release(request);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

static void test_req_response_errors(void) {
  static const uint8_t malformed[] = {0x0a, 3, 0x12, 1, 'A', 0};
  for (unsigned mode = 0u; mode < 3u; ++mode) {
    test_env_t env;
    h2_gizclaw_service_t *service = create_profile_service(&env);
    env.rpc_remote_error = mode == 0u;
    env.rpc_result = mode == 1u ? H2_PAL_ERR_TIMEOUT : H2_PAL_OK;
    if (mode == 2u) {
      env.response_payload = malformed;
      env.response_payload_len = sizeof(malformed);
    }
    const h2_pal_result_t execution =
        mode == 0u ? H2_GIZCLAW_ERR_REMOTE : env.rpc_result;
    assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
    h2_gizclaw_req_t *request = NULL;
    assert(h2_gizclaw_req_create_profile_get(service, 1u, 1234u, &request) ==
           H2_PAL_OK);
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == execution);
    h2_gizclaw_profile_t profile;
    assert(h2_gizclaw_resp_parse_profile_get(request, &profile) ==
           (mode == 2u ? H2_PAL_ERR_FORMAT : execution));
    assert(!profile.has_name && profile.name[0] == '\0');
    h2_gizclaw_req_release(request);
    assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
    assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
  }
}

typedef struct req_waiter {
  h2_gizclaw_req_t *request;
  h2_pal_result_t result;
} req_waiter_t;

static void *wait_request_thread(void *user) {
  req_waiter_t *waiter = user;
  waiter->result = h2_gizclaw_req_wait(waiter->request, 2000u);
  return NULL;
}

static void test_req_multiple_waiters_and_queued_close(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_profile_service(&env);
  atomic_store(&env.run_gate, false);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  h2_gizclaw_req_t *request = NULL;
  assert(h2_gizclaw_req_create_profile_get(service, 1u, 1234u, &request) ==
         H2_PAL_OK);
  assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
  pthread_t threads[4];
  req_waiter_t waiters[4];
  for (size_t i = 0u; i < 4u; ++i) {
    waiters[i] = (req_waiter_t){.request = request};
    assert(pthread_create(&threads[i], NULL, wait_request_thread,
                          &waiters[i]) == 0);
  }
  atomic_store(&env.run_gate, true);
  for (size_t i = 0u; i < 4u; ++i) {
    assert(pthread_join(threads[i], NULL) == 0);
    assert(waiters[i].result == H2_PAL_OK);
  }
  h2_gizclaw_req_release(request);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);

  /* Connection failure must settle even requests that never reach run(). */
  service = create_profile_service(&env);
  atomic_store(&env.connect_gate, false);
  env.connect_result = H2_PAL_ERR_IO;
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  assert(h2_gizclaw_req_create_profile_get(service, 2u, 1234u, &request) ==
         H2_PAL_OK);
  assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
  atomic_store(&env.connect_gate, true);
  assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_ERR_CLOSED);
  assert(atomic_load(&env.rpc_start_count) == 0u);
  h2_gizclaw_req_release(request);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  size_t dispatched = 0u;
  assert(h2_gizclaw_service_poll(service, 8u, &dispatched) == H2_PAL_OK);
  assert(atomic_load(&env.terminal_count) == 1u);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

static void test_req_register_and_peer_delete(void) {
  static const uint8_t token_payload[] = {0x0a, 3, 'a', 'b', 'c'};
  static const uint8_t register_payload[] = {0x0a, 7,   'd', 'e', 'f',
                                             'a',  'u', 'l', 't'};
  test_env_t env;
  h2_gizclaw_service_t *service = create_profile_service(&env);
  env.expected_method = H2_GIZCLAW_RPC_SERVER_REGISTER;
  env.expected_payload = token_payload;
  env.expected_payload_len = sizeof(token_payload);
  env.response_payload = register_payload;
  env.response_payload_len = sizeof(register_payload);
  char token[] = "abc";
  h2_gizclaw_req_t *request = NULL;
  assert(h2_gizclaw_req_create_register(service, 1u, token, 1234u, &request) ==
         H2_PAL_OK);
  token[0] = 'X';
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
  assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK);
  h2_gizclaw_registration_result_t registration;
  assert(h2_gizclaw_resp_parse_register(request, &registration) == H2_PAL_OK);
  assert(strcmp(registration.runtime_profile_name, "default") == 0);
  assert(h2_gizclaw_resp_parse_peer_delete(request) == H2_PAL_ERR_INVALID_ARG);
  h2_gizclaw_req_release(request);
  assert(strcmp(registration.runtime_profile_name, "default") == 0);
  assert(h2_gizclaw_rpc_register(service, "abc", 1234u, &registration) ==
         H2_PAL_OK);
  assert(strcmp(registration.runtime_profile_name, "default") == 0);

  env.expected_method = H2_GIZCLAW_RPC_SERVER_PEER_DELETE;
  env.expected_payload_len = 0u;
  env.response_payload_len = 0u;
  assert(h2_gizclaw_req_create_peer_delete(service, 2u, 1234u, &request) ==
         H2_PAL_OK);
  assert(h2_gizclaw_resp_parse_peer_delete(request) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
  assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK);
  assert(h2_gizclaw_resp_parse_peer_delete(request) == H2_PAL_OK);
  h2_gizclaw_req_release(request);
  assert(h2_gizclaw_rpc_peer_delete(service, 1234u) == H2_PAL_OK);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

static h2_pal_result_t fake_req_clock(void *user, uint64_t *out_ms) {
  test_env_t *env = user;
  *out_ms = atomic_load(&env->clock_ms);
  return H2_PAL_OK;
}

static void test_req_ping_execution_timing(void) {
  static const h2_pal_time_vtable_t vtable = {.get_monotonic_ms =
                                                  fake_req_clock};
  static const uint8_t response[] = {0x08, 123};
  test_env_t env;
  h2_gizclaw_service_t *service = create_profile_service(&env);
  const h2_pal_time_api_t time = {.user = &env, .vtable = &vtable};
  service->client_config.time = &time;
  env.expected_method = H2_GIZCLAW_RPC_ALL_PING;
  env.response_payload = response;
  env.response_payload_len = sizeof(response);
  atomic_store(&env.run_gate, false);
  atomic_store(&env.clock_ms, 100u);
  h2_gizclaw_req_t *request = NULL;
  assert(h2_gizclaw_req_create_ping(service, 1u, 1234u, &request) == H2_PAL_OK);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  /* Time between create and actual execution is not wire RTT. */
  atomic_store(&env.clock_ms, 500u);
  assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
  wait_for_count(&env.rpc_start_count, 1u);
  atomic_store(&env.clock_ms, 545u);
  atomic_store(&env.run_gate, true);
  assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK);
  atomic_store(&env.clock_ms, 9000u);
  h2_gizclaw_ping_result_t ping;
  assert(h2_gizclaw_resp_parse_ping(request, &ping) == H2_PAL_OK);
  assert(ping.round_trip_ms == 45u && ping.server_time_ms == 123);
  h2_gizclaw_req_release(request);
  assert(h2_gizclaw_rpc_ping(service, 1234u, &ping) == H2_PAL_OK);
  assert(ping.round_trip_ms == 0u && ping.server_time_ms == 123);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

typedef struct unary_context_test {
  test_env_t *env;
  const void *tag;
  atomic_uint destroys;
} unary_context_test_t;

static void unary_context_destroy(void *user) {
  unary_context_test_t *state = user;
  assert(atomic_fetch_add(&state->destroys, 1u) == 0u);
}

static void test_req_unary_context_lifetime(void) {
  static const char tag, wrong_tag;
  /* Successful response, failed response, running cancel, never submitted,
   * caller release before completion, and rejected construction. */
  for (unsigned mode = 0u; mode < 6u; ++mode) {
    test_env_t env;
    h2_gizclaw_service_t *service = create_profile_service(&env);
    unary_context_test_t state = {.env = &env, .tag = &tag};
    h2_gizclaw_req_t *request = NULL;
    const h2_pal_result_t created = h2_gizclaw_req_create_rpc_context_internal(
        service, 1u, env.expected_method, &tag, (h2_gizclaw_rpc_bytes_t){0},
        mode == 5u ? 0u : 1234u, unary_context_destroy, &state, &request);
    if (mode == 5u) {
      assert(created == H2_PAL_ERR_INVALID_ARG && request == NULL);
      assert(atomic_load(&state.destroys) == 0u);
      /* Rejected constructors do not consume the caller's context. */
      unary_context_destroy(&state);
    } else {
      assert(created == H2_PAL_OK);
      const void *context = &state;
      assert(h2_gizclaw_req_context_internal(request, &tag, &context) ==
             H2_PAL_ERR_INVALID_STATE);
      assert(context == NULL);
      assert(h2_gizclaw_req_context_internal(request, &wrong_tag, &context) ==
             H2_PAL_ERR_INVALID_ARG);
      if (mode != 3u) {
        atomic_store(&env.run_gate, false);
        env.rpc_result = mode == 1u ? H2_PAL_ERR_IO : H2_PAL_OK;
        assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
        assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
        wait_for_count(&env.rpc_start_count, 1u);
        if (mode == 4u) {
          h2_gizclaw_req_release(request);
          request = NULL;
        } else if (mode == 2u) {
          assert(h2_gizclaw_req_cancel(request) == H2_PAL_OK);
        }
        atomic_store(&env.run_gate, true);
        if (mode == 4u) {
          wait_for_count(&env.rpc_destroy_count, 1u);
          wait_for_count(&state.destroys, 1u);
        } else {
          h2_pal_result_t expected =
              mode == 2u ? H2_PAL_ERR_CLOSED : env.rpc_result;
          assert(h2_gizclaw_req_wait(request, 2000u) == expected);
          assert(h2_gizclaw_req_context_internal(request, &tag, &context) ==
                 expected);
          assert(context == (expected == H2_PAL_OK ? &state : NULL));
          const h2_gizclaw_rpc_response_t *response = NULL;
          assert(h2_gizclaw_req_response_internal(request, &tag, &response) ==
                 expected);
          if (expected == H2_PAL_OK) {
            assert(response->result_payload_len == env.response_payload_len);
            assert(memcmp(response->result_payload, env.response_payload,
                          response->result_payload_len) == 0);
          } else {
            assert(response == NULL);
          }
        }
      }
      h2_gizclaw_req_release(request);
    }
    assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
    assert(atomic_load(&state.destroys) == 1u);
    assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
  }
}

typedef struct probe_test {
  test_env_t *env;
  atomic_uint destroys;
} probe_test_t;

static h2_pal_result_t probe_send(void *user, h2_gizclaw_client_t *client,
                                  const h2_gizclaw_cancel_token_t *token) {
  (void)client;
  probe_test_t *state = user;
  atomic_fetch_add(&state->env->async_start_count, 1u);
  return h2_gizclaw_cancel_requested(token) ? H2_PAL_ERR_CLOSED
                                            : state->env->rpc_result;
}

static void probe_destroy(void *user) {
  probe_test_t *state = user;
  assert(atomic_fetch_add(&state->destroys, 1u) == 0u);
}

static void notification_filler(void *user) {
  test_env_t *env = user;
  assert(pthread_equal(pthread_self(), env->app_thread));
  atomic_fetch_add(&env->run_exit_count, 1u);
}

static void test_service_event_queue_backpressure(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_service(&env, 1u);
  atomic_store(&env.connect_gate, false);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_post_internal(service, notification_filler, &env) ==
         H2_PAL_OK);
  atomic_store(&env.connect_gate, true);
  wait_for_count(&env.event_dispatch_count, 2u);
  assert(!atomic_load(&env.event_emitted));
  static const char tag;
  probe_test_t state = {.env = &env};
  env.rpc_result = H2_PAL_OK;
  h2_gizclaw_req_t *request = NULL;
  assert(h2_gizclaw_req_create_send_internal(service, 1u, 1000u, &tag,
                                             probe_send, probe_destroy, &state,
                                             &request) == H2_PAL_OK);
  assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
  assert(h2_gizclaw_req_wait(request, 1000u) == H2_PAL_OK);
  h2_gizclaw_req_release(request);
  wait_for_count(&state.destroys, 1u);
  assert(atomic_load(&env.event_callback_count) == 0 &&
         atomic_load(&env.run_exit_count) == 0);
  for (unsigned i = 0; i < 2000 && !atomic_load(&env.event_callback_count);
       ++i) {
    size_t dispatched = 0;
    assert(h2_gizclaw_service_poll(service, 8, &dispatched) == H2_PAL_OK);
    h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 1);
  }
  assert(atomic_load(&env.event_callback_count) == 1 &&
         atomic_load(&env.run_exit_count) == 1);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

static int fake_req_telemetry_send(void *user,
                                   const gzc_telemetry_frame_t *frame) {
  test_env_t *env = user;
  assert(!pthread_equal(pthread_self(), env->app_thread));
  assert(frame->sequence == 7u && frame->observation_count == 1u);
  assert(frame->observations[0].kind == GZC_TELEMETRY_OBSERVATION_NETWORK);
  assert(frame->observations[0].network.rat.len == 4u);
  assert(memcmp(frame->observations[0].network.rat.data, "wifi", 4u) == 0);
  atomic_fetch_add(&env->rpc_start_count, 1u);
  atomic_fetch_add(&env->clock_ms, 10u);
  return env->rpc_result == H2_PAL_ERR_WOULD_BLOCK ? GZC_ERR_WOULD_BLOCK
         : env->rpc_result == H2_PAL_OK            ? GZC_OK
                                                   : GZC_ERR_WEBRTC;
}

static void test_req_telemetry_copy_and_backpressure(void) {
  static const h2_pal_time_vtable_t vtable = {.get_monotonic_ms =
                                                  fake_req_clock};
  for (unsigned mode = 0u; mode < 5u; ++mode) {
    test_env_t env;
    h2_gizclaw_service_t *service = create_profile_service(&env);
    const h2_pal_time_api_t time = {.user = &env, .vtable = &vtable};
    service->client_config.time = &time;
    env.rpc_result = mode == 0u   ? H2_PAL_OK
                     : mode == 1u ? H2_PAL_ERR_WOULD_BLOCK
                     : mode == 2u ? H2_PAL_ERR_IO
                                  : H2_PAL_ERR_CLOSED;
    h2_gizclaw_test_set_telemetry_send(fake_req_telemetry_send, &env);
    char rat[] = "wifi";
    h2_gizclaw_telemetry_observation_t observation = {
        .kind = H2_GIZCLAW_TELEMETRY_NETWORK,
        .value.network = {.has_rat = true, .rat = {rat, 4u}},
    };
    const h2_gizclaw_telemetry_frame_t frame = {
        .sequence = 7u,
        .observations = &observation,
        .observation_count = 1u,
    };
    h2_gizclaw_req_t *request = NULL;
    assert(h2_gizclaw_req_create_telemetry_send(service, 1u, &frame, 30u,
                                                &request) == H2_PAL_OK);
    rat[0] = 'X';
    if (mode == 4u) {
      /* Releasing a CREATED request never submits a packet. */
      h2_gizclaw_req_release(request);
      assert(atomic_load(&env.rpc_start_count) == 0u);
      assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
      assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
      h2_gizclaw_test_set_telemetry_send(NULL, NULL);
      continue;
    }
    if (mode == 3u)
      atomic_store(&env.connect_gate, false);
    assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    if (mode == 3u) {
      assert(h2_gizclaw_req_cancel(request) == H2_PAL_OK);
      atomic_store(&env.connect_gate, true);
    }
    assert(h2_gizclaw_req_wait(request, 2000u) == env.rpc_result);
    assert(h2_gizclaw_req_wait(request, 0u) == env.rpc_result);
    assert(h2_gizclaw_resp_parse_telemetry_send(request) == env.rpc_result);
    assert(atomic_load(&env.rpc_start_count) == (mode == 3u ? 0u : 1u));
    assert(atomic_load(&env.completion_count) == 0u);
    h2_gizclaw_req_release(request);
    rat[0] = 'w';
    if (mode != 3u) {
      assert(h2_gizclaw_rpc_telemetry_send(service, &frame, 30u) ==
             env.rpc_result);
      assert(atomic_load(&env.rpc_start_count) == 2u);
    }
    assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
    assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
    h2_gizclaw_test_set_telemetry_send(NULL, NULL);
  }
}

static void test_req_point_storage_and_limits(void) {
  static const uint8_t account_payload[] = {0x0a, 5, 0x08, 42, 0x2a, 1, 'T'};
  static const uint8_t list_payload[] = {
      0x0a, 15, 0x08, 1, 0x12, 3,    0x2a, 1,   'a',
      0x12, 3,  0x2a, 1, 'b',  0x1a, 1,    'c',
  };
  uint8_t bytes[4097];
  h2_gizclaw_resp_storage_t storage = {.data = bytes + 1,
                                       .capacity = sizeof(bytes) - 1};
  test_env_t env;
  h2_gizclaw_service_t *service = create_profile_service(&env);
  env.expected_method = H2_GIZCLAW_RPC_SERVER_POINTS_GET;
  env.response_payload = account_payload;
  env.response_payload_len = sizeof(account_payload);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  h2_gizclaw_req_t *request = NULL;
  assert(h2_gizclaw_req_create_point_get(service, 1u, 1234u, &request) ==
         H2_PAL_OK);
  assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
  assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK);
  h2_gizclaw_points_account_t account;
  h2_gizclaw_resp_storage_t small = {.data = bytes, .capacity = 1u};
  assert(h2_gizclaw_resp_parse_point_get(request, &small, &account) ==
         H2_PAL_ERR_NO_SPACE);
  assert(small.used == 0u && account.updated_at.data == NULL);
  assert(h2_gizclaw_resp_parse_point_get(request, &storage, &account) ==
         H2_PAL_OK);
  h2_gizclaw_req_release(request);
  assert(account.balance == 42 && strcmp(account.updated_at.data, "T") == 0);
  h2_gizclaw_points_account_t sync_account;
  assert(h2_gizclaw_rpc_point_get(service, 1234u, &storage, &sync_account) ==
         H2_PAL_OK);
  assert(sync_account.balance == 42 &&
         strcmp(sync_account.updated_at.data, "T") == 0);

  uint8_t expected[] = {0x0a, 5, 0x0a, 1, 'q', 0x10, 2};
  env.expected_method = H2_GIZCLAW_RPC_SERVER_POINTS_TRANSACTIONS_LIST;
  env.expected_payload = expected;
  env.expected_payload_len = sizeof(expected);
  env.response_payload = list_payload;
  env.response_payload_len = sizeof(list_payload);
  char cursor[] = "q";
  assert(h2_gizclaw_req_create_point_transaction_list(
             service, 2u, (h2_gizclaw_str_t){cursor, 1u}, 2u, 1234u,
             &request) == H2_PAL_OK);
  cursor[0] = 'x';
  assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
  assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK);
  h2_gizclaw_points_transaction_page_t page;
  assert(h2_gizclaw_resp_parse_point_transaction_list(request, &small, &page) ==
         H2_PAL_ERR_NO_SPACE);
  assert(page.items == NULL && page.count == 0u && small.used == 0u);
  assert(h2_gizclaw_resp_parse_point_get(request, &storage, &sync_account) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_resp_parse_point_transaction_list(request, &storage,
                                                      &page) == H2_PAL_OK);
  h2_gizclaw_req_release(request);
  assert(page.count == 2u && page.has_next &&
         strcmp(page.next_cursor.data, "c") == 0);
  assert(strcmp(page.items[0].id.data, "a") == 0 &&
         strcmp(page.items[1].id.data, "b") == 0);
  h2_gizclaw_points_transaction_page_t sync_page;
  cursor[0] = 'q';
  assert(h2_gizclaw_rpc_point_transaction_list(
             service, (h2_gizclaw_str_t){cursor, 1u}, 2u, 1234u, &storage,
             &sync_page) == H2_PAL_OK);
  assert(sync_page.count == 2u && strcmp(sync_page.items[1].id.data, "b") == 0);
  const size_t checkpoint = storage.used;
  expected[6] = 1;
  assert(h2_gizclaw_req_create_point_transaction_list(
             service, 3u, (h2_gizclaw_str_t){cursor, 1u}, 1u, 1234u,
             &request) == H2_PAL_OK);
  assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
  assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK);
  h2_gizclaw_points_transaction_page_t overflow;
  assert(h2_gizclaw_resp_parse_point_transaction_list(
             request, &storage, &overflow) == H2_PAL_ERR_FORMAT);
  assert(storage.used == checkpoint && overflow.items == NULL &&
         overflow.count == 0u);
  h2_gizclaw_req_release(request);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
  assert(strcmp(account.updated_at.data, "T") == 0);
  assert(strcmp(page.items[1].id.data, "b") == 0);
  assert(strcmp(sync_page.items[0].id.data, "a") == 0);
}

static void test_req_workflow_public_paths(void) {
  static const uint8_t get_params[] = {0x0a, 11,  's', 't', 'o', 'r', 'y',
                                       '.',  'a', 'e', 's', 'o', 'p'};
  static const uint8_t list_params[] = {0x0a, 1, 'q', 0x10, 1, 0x1a, 1, 'a'};
  uint8_t response[] = {0x0a, 16,  0x0a, 11,  's', 't',  'o', 'r',
                        'y',  '.', 'a',  'e', 's', 'o',  'p', 0x1a,
                        1,    'a', 0x12, 1,   'p', 0x1a, 1,   'r'};
  for (unsigned list = 0u; list < 2u; ++list) {
    test_env_t env;
    h2_gizclaw_service_t *service = create_profile_service(&env);
    env.expected_method = list ? H2_GIZCLAW_RPC_SERVER_WORKFLOW_LIST
                               : H2_GIZCLAW_RPC_SERVER_WORKFLOW_GET;
    env.expected_payload = list ? list_params : get_params;
    env.expected_payload_len = list ? sizeof(list_params) : sizeof(get_params);
    response[0] = list ? 0x12 : 0x0a;
    response[18] = list ? 0x22 : 0x12;
    response[21] = list ? 0x2a : 0x1a;
    env.response_payload = response;
    env.response_payload_len = sizeof(response);
    uint8_t bytes[2048];
    h2_gizclaw_resp_storage_t storage = {.data = bytes,
                                         .capacity = sizeof(bytes)};
    h2_gizclaw_req_t *request = NULL;
    assert(h2_gizclaw_req_create_workflow_get(
               service, 1u, (h2_gizclaw_str_t){"story..esop", 11u}, 1234u,
               &request) == H2_PAL_ERR_INVALID_ARG);
    assert(request == NULL && atomic_load(&env.rpc_start_count) == 0u);
    char name[] = "story.aesop";
    const h2_gizclaw_str_t collection = {"a", 1u};
    const h2_gizclaw_str_t cursor = {"q", 1u};
    assert((list ? h2_gizclaw_req_create_workflow_list(
                       service, 1u, collection, cursor, 1u, 1234u, &request)
                 : h2_gizclaw_req_create_workflow_get(
                       service, 1u, (h2_gizclaw_str_t){name, 11u}, 1234u,
                       &request)) == H2_PAL_OK);
    name[0] = 'X';
    assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK);
    h2_gizclaw_workflow_get_result_t result;
    h2_gizclaw_workflow_page_t page;
    assert(h2_gizclaw_resp_parse_workflow_get(request, &storage, &result) ==
           (list ? H2_PAL_ERR_INVALID_ARG : H2_PAL_OK));
    assert(h2_gizclaw_resp_parse_workflow_list(request, &storage, &page) ==
           (list ? H2_PAL_OK : H2_PAL_ERR_INVALID_ARG));
    h2_gizclaw_req_release(request);
    if (list) {
      assert(page.count == 1u &&
             strcmp(page.items[0].name, "story.aesop") == 0);
      assert(strcmp(page.runtime_profile_name, "p") == 0 &&
             strcmp(page.runtime_profile_revision, "r") == 0);
      assert(h2_gizclaw_rpc_workflow_list(service, collection, cursor, 1u,
                                          1234u, &storage, &page) == H2_PAL_OK);
    } else {
      assert(strcmp(result.workflow.name, "story.aesop") == 0);
      assert(strcmp(result.runtime_profile_name, "p") == 0 &&
             strcmp(result.runtime_profile_revision, "r") == 0);
      assert(h2_gizclaw_rpc_workflow_get(
                 service, (h2_gizclaw_str_t){"story.aesop", 11u}, 1234u,
                 &storage, &result) == H2_PAL_OK);
    }
    assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
    assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
    assert(strcmp(list ? page.items[0].name : result.workflow.name,
                  "story.aesop") == 0);
  }
}

typedef struct test_contact_text {
  const char *data;
  size_t len;
} test_contact_text_t;

typedef struct test_contact_rpc {
  h2_gizclaw_rpc_method_t expected_method;
  const uint8_t *expected_request;
  size_t expected_request_len;
  const uint8_t *response;
  size_t response_len;
  bool has_error;
  int error_code;
  const char *error_message;
  size_t error_message_len;
  h2_pal_result_t transport_result;
  bool request_matches;
  int calls;
} test_contact_rpc_t;

typedef struct test_rpc_sequence {
  test_contact_rpc_t *steps;
  size_t step_count;
  size_t next_step;
} test_rpc_sequence_t;

static bool test_encode_contact_text(pb_ostream_t *stream,
                                     const pb_field_t *field,
                                     void *const *arg) {
  const test_contact_text_t *text = *arg;
  return text != NULL && pb_encode_tag_for_field(stream, field) &&
         pb_encode_string(stream, (const pb_byte_t *)text->data, text->len);
}

static bool test_encode_workspace_delete_response(uint8_t *buffer,
                                                  size_t capacity,
                                                  size_t *out_len) {
  gizclaw_rpc_v1_WorkspaceDeleteResponse response =
      gizclaw_rpc_v1_WorkspaceDeleteResponse_init_zero;
  test_contact_text_t text[] = {
      {.data = "workspace-1", .len = 11u},
      {.data = "chat", .len = 4u},
  };
  response.has_value = true;
  response.value.name.funcs.encode = test_encode_contact_text;
  response.value.name.arg = &text[0];
  response.value.workflow_name.funcs.encode = test_encode_contact_text;
  response.value.workflow_name.arg = &text[1];
  response.value.available = true;
  pb_ostream_t stream = pb_ostream_from_buffer(buffer, capacity);
  if (!pb_encode(&stream, gizclaw_rpc_v1_WorkspaceDeleteResponse_fields,
                 &response)) {
    return false;
  }
  *out_len = stream.bytes_written;
  return true;
}

static bool test_encode_workspace_input_put_response(uint8_t *buffer,
                                                     size_t capacity,
                                                     size_t *out_len) {
  gizclaw_rpc_v1_WorkspaceInputPutResponse response =
      gizclaw_rpc_v1_WorkspaceInputPutResponse_init_zero;
  test_contact_text_t text[] = {
      {.data = "workspace-1", .len = 11u},
      {.data = "chat", .len = 4u},
  };
  response.has_value = true;
  response.value.name.funcs.encode = test_encode_contact_text;
  response.value.name.arg = &text[0];
  response.value.workflow_name.funcs.encode = test_encode_contact_text;
  response.value.workflow_name.arg = &text[1];
  response.value.available = true;
  pb_ostream_t stream = pb_ostream_from_buffer(buffer, capacity);
  if (!pb_encode(&stream, gizclaw_rpc_v1_WorkspaceInputPutResponse_fields,
                 &response)) {
    return false;
  }
  *out_len = stream.bytes_written;
  return true;
}

static test_rpc_sequence_t *workspace_test_sequence;
static test_contact_rpc_t *workspace_test_single;
static test_contact_rpc_t *workspace_test_current;
static void workspace_test_use_single(test_contact_rpc_t *mock) {
  workspace_test_sequence = NULL;
  workspace_test_single = mock;
}
static void workspace_test_use_sequence(test_rpc_sequence_t *sequence) {
  workspace_test_sequence = sequence;
  workspace_test_single = NULL;
}
static int workspace_test_start(h2_gizclaw_client_t *client,
                                h2_gizclaw_rpc_method_t method,
                                h2_gizclaw_rpc_bytes_t payload,
                                uint32_t timeout_ms,
                                h2_gizclaw_rpc_request_t **out_request) {
  assert(client == (h2_gizclaw_client_t *)s_env && timeout_ms <= 1234u &&
         timeout_ms > 0u);
  test_contact_rpc_t *mock = workspace_test_single;
  if (workspace_test_sequence != NULL) {
    assert(workspace_test_sequence->next_step <
           workspace_test_sequence->step_count);
    mock =
        &workspace_test_sequence->steps[workspace_test_sequence->next_step++];
  }
  assert(mock != NULL);
  ++mock->calls;
  mock->request_matches =
      method == mock->expected_method &&
      payload.len == mock->expected_request_len &&
      (payload.len == 0u ||
       memcmp(payload.data, mock->expected_request, payload.len) == 0);
  workspace_test_current = mock;
  *out_request = (h2_gizclaw_rpc_request_t *)mock;
  return H2_PAL_OK;
}
static int workspace_test_result(h2_gizclaw_rpc_request_t *request,
                                 h2_gizclaw_rpc_response_t *out_response) {
  test_contact_rpc_t *mock = (test_contact_rpc_t *)request;
  assert(mock == workspace_test_current);
  memset(out_response, 0, sizeof(*out_response));
  out_response->has_error = mock->has_error;
  out_response->error_code = mock->error_code;
  if (mock->transport_result != H2_PAL_OK)
    return mock->transport_result;
  if (mock->error_message_len != 0u) {
    out_response->error_message = h2_pal_mem_alloc(
        s_env->service->client_config.allocator, mock->error_message_len);
    assert(out_response->error_message != NULL);
    memcpy(out_response->error_message, mock->error_message,
           mock->error_message_len);
    out_response->error_message_len = mock->error_message_len;
  }
  if (mock->response_len != 0u) {
    out_response->result_payload = h2_pal_mem_alloc(
        s_env->service->client_config.allocator, mock->response_len);
    assert(out_response->result_payload != NULL);
    memcpy(out_response->result_payload, mock->response, mock->response_len);
    out_response->result_payload_len = mock->response_len;
  }
  return H2_PAL_OK;
}
static void workspace_test_destroy(h2_gizclaw_rpc_request_t *request) {
  (void)request;
}
static const h2_gizclaw_async_rpc_ops_t workspace_test_ops = {
    .start = workspace_test_start,
    .result = workspace_test_result,
    .cancel = workspace_test_destroy,
    .destroy = workspace_test_destroy,
};

typedef struct remote_error_hook {
  h2_gizclaw_req_t *request;
  h2_pal_result_t result;
  int error_code;
  const char *error_message;
  size_t error_message_len;
  bool has_error;
  unsigned calls;
} remote_error_hook_t;

static void test_req_remote_error_mapping(void) {
  static const struct {
    bool has_error;
    int code;
    h2_pal_result_t transport;
    h2_pal_result_t expected;
  } cases[] = {
      {true, H2_GIZCLAW_RPC_ERROR_NOT_FOUND, H2_PAL_OK, H2_PAL_ERR_NOT_FOUND},
      {true, H2_GIZCLAW_RPC_ERROR_METHOD_NOT_FOUND, H2_PAL_OK,
       H2_GIZCLAW_ERR_REMOTE},
      {true, H2_GIZCLAW_RPC_ERROR_BAD_REQUEST, H2_PAL_OK,
       H2_GIZCLAW_ERR_REMOTE},
      {true, H2_GIZCLAW_RPC_ERROR_FORBIDDEN, H2_PAL_OK, H2_GIZCLAW_ERR_REMOTE},
      {true, H2_GIZCLAW_RPC_ERROR_CONFLICT, H2_PAL_OK, H2_GIZCLAW_ERR_REMOTE},
      {true, 0, H2_PAL_OK, H2_GIZCLAW_ERR_REMOTE},
      {true, INT_MIN, H2_PAL_OK, H2_GIZCLAW_ERR_REMOTE},
      {true, INT_MAX, H2_PAL_OK, H2_GIZCLAW_ERR_REMOTE},
      {true, 404, H2_PAL_ERR_TIMEOUT, H2_PAL_ERR_TIMEOUT},
      {true, 404, H2_PAL_ERR_FORMAT, H2_PAL_ERR_FORMAT},
      {true, 404, H2_PAL_ERR_IO, H2_PAL_ERR_IO},
      {false, 404, H2_PAL_OK, H2_PAL_OK},
  };
  /* A valid empty DeviceInfo, not an error response. */
  static const uint8_t payload[] = {0x0a, 0x00};
  static const char remote_message[] = "remote detail";
  for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    test_env_t env;
    h2_gizclaw_service_t *service = create_profile_service(&env);
    test_contact_rpc_t mock = {
        .expected_method = H2_GIZCLAW_RPC_SERVER_INFO_GET,
        .has_error = cases[i].has_error,
        .error_code = cases[i].code,
        .error_message = cases[i].has_error && cases[i].transport == H2_PAL_OK
                             ? remote_message
                             : NULL,
        .error_message_len =
            cases[i].has_error && cases[i].transport == H2_PAL_OK
                ? sizeof(remote_message) - 1u
                : 0u,
        .transport_result = cases[i].transport,
        .response = payload,
        .response_len = sizeof(payload),
    };
    workspace_test_use_single(&mock);
    h2_gizclaw_async_rpc_test_set_ops(&workspace_test_ops);
    assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
    h2_gizclaw_req_t *request = NULL;
    assert(h2_gizclaw_req_create_profile_get(service, 1u, 1234u, &request) ==
           H2_PAL_OK);
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == cases[i].expected);
    assert(h2_gizclaw_req_wait(request, 0u) == cases[i].expected);
    h2_gizclaw_profile_t profile;
    memset(&profile, 0xa5, sizeof(profile));
    assert(h2_gizclaw_resp_parse_profile_get(request, &profile) ==
           cases[i].expected);
    assert(!profile.has_name && profile.name[0] == '\0');
    h2_gizclaw_req_release(request);
    memset(&profile, 0xa5, sizeof(profile));
    assert(h2_gizclaw_rpc_profile_get(service, 1234u, &profile) ==
           cases[i].expected);
    assert(!profile.has_name && profile.name[0] == '\0');
    assert(mock.request_matches && mock.calls == 2);
    assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
    assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
    h2_gizclaw_async_rpc_test_set_ops(NULL);
  }
}

static int workspace_expect(int condition, const char *message) {
  if (!condition)
    fprintf(stderr, "FAIL workspace: %s\n", message);
  return !condition;
}
static void test_workspace_direct_input_update(void) {
  int fails = 0;
  test_env_t env;
  h2_gizclaw_service_t *service = create_profile_service(&env);
  static const h2_pal_time_vtable_t time_vtable = {.get_monotonic_ms =
                                                       fake_req_clock};
  const h2_pal_time_api_t time = {.user = &env, .vtable = &time_vtable};
  service->client_config.time = &time;
  h2_gizclaw_async_rpc_test_set_ops(&workspace_test_ops);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  uint8_t buffer[4096];
  h2_gizclaw_resp_storage_t storage = {.data = buffer,
                                       .capacity = sizeof(buffer)};
  const uint8_t workspace_get_request[] = {
      0x0a, 0x0b, 'w', 'o', 'r', 'k', 's', 'p', 'a', 'c', 'e', '-', '1',
  };
  const uint8_t workspace_input_put_request[] = {
      0x0a, 0x0b, 'w', 'o', 'r', 'k',  's',  'p',
      'a',  'c',  'e', '-', '1', 0x10, 0x02,
  };
  uint8_t workspace_input_put_response[128];
  size_t workspace_input_put_response_len = 0u;
  fails += workspace_expect(test_encode_workspace_input_put_response(
                                workspace_input_put_response,
                                sizeof(workspace_input_put_response),
                                &workspace_input_put_response_len),
                            "workspace input put response fixture encodes");
  test_contact_rpc_t workspace_input_mock = {
      .expected_method = H2_GIZCLAW_RPC_SERVER_WORKSPACE_INPUT_PUT,
      .expected_request = workspace_input_put_request,
      .expected_request_len = sizeof(workspace_input_put_request),
      .response = workspace_input_put_response,
      .response_len = workspace_input_put_response_len,
  };
  workspace_test_use_single(&workspace_input_mock);
  h2_gizclaw_workspace_t workspace = {0};
  fails += workspace_expect(
      h2_gizclaw_rpc_workspace_set_input(
          service, (h2_gizclaw_str_t){.data = "workspace-1", .len = 11u},
          H2_GIZCLAW_WORKSPACE_INPUT_REALTIME, 1234u, &storage,
          &workspace) == H2_PAL_OK,
      "workspace input update uses the direct input PUT RPC");
  fails += workspace_expect(
      workspace_input_mock.calls == 1 && workspace_input_mock.request_matches &&
          strcmp(workspace.name, "workspace-1") == 0 &&
          strcmp(workspace.workflow_name, "chat") == 0 && workspace.available,
      "workspace input update is one request and owns its response");
  storage.used = 0u;

  const uint8_t *workspace_request = workspace_get_request;
  uint8_t workspace_response[128];
  size_t workspace_response_len = 0u;
  fails += workspace_expect(test_encode_workspace_delete_response(
                                workspace_response, sizeof(workspace_response),
                                &workspace_response_len),
                            "workspace delete response fixture encodes");
  test_contact_rpc_t mock = {
      .expected_method = H2_GIZCLAW_RPC_SERVER_WORKSPACE_DELETE,
      .expected_request = workspace_request,
      .expected_request_len = sizeof(workspace_get_request),
      .response = workspace_response,
      .response_len = workspace_response_len,
  };
  workspace_test_use_single(&mock);
  fails += workspace_expect(
      h2_gizclaw_rpc_workspace_delete(
          service, (h2_gizclaw_str_t){.data = "workspace-1", .len = 11u}, 1234u,
          &storage, &workspace) == H2_PAL_OK,
      "workspace delete accepts the typed mocked RPC response");
  fails += workspace_expect(
      mock.calls == 1 && mock.request_matches &&
          strcmp(workspace.name, "workspace-1") == 0 &&
          strcmp(workspace.workflow_name, "chat") == 0 && workspace.available,
      "workspace delete encodes method 28 and owns its snapshot");
  storage.used = 0u;

  mock = (test_contact_rpc_t){
      .expected_method = H2_GIZCLAW_RPC_SERVER_WORKSPACE_DELETE,
      .expected_request = workspace_request,
      .expected_request_len = sizeof(workspace_get_request),
      .has_error = true,
      .error_code = H2_GIZCLAW_RPC_ERROR_NOT_FOUND,
  };
  workspace.name = (char *)0x1;
  fails += workspace_expect(
      h2_gizclaw_rpc_workspace_delete(
          service, (h2_gizclaw_str_t){.data = "workspace-1", .len = 11u}, 1234u,
          &storage, &workspace) == H2_PAL_ERR_NOT_FOUND &&
          workspace.name == NULL,
      "workspace delete maps RPC errors and clears output");

  const char invalid_utf8[] = {(char)0xc0, (char)0x80};
  fails += workspace_expect(
      h2_gizclaw_rpc_workspace_delete(
          service, (h2_gizclaw_str_t){.data = invalid_utf8, .len = 2u}, 1234u,
          &storage, &workspace) == H2_PAL_ERR_INVALID_ARG &&
          workspace.name == NULL,
      "workspace delete rejects invalid UTF-8 before RPC dispatch");

  assert(fails == 0);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

static void test_workspace_request_and_response_paths(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_profile_service(&env);
  static const h2_pal_time_vtable_t tv = {.get_monotonic_ms = fake_req_clock};
  const h2_pal_time_api_t time = {.user = &env, .vtable = &tv};
  service->client_config.time = &time;
  h2_gizclaw_async_rpc_test_set_ops(&workspace_test_ops);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  uint8_t buffer[8192];
  h2_gizclaw_resp_storage_t storage = {.data = buffer,
                                       .capacity = sizeof(buffer)};
  h2_gizclaw_req_t *request = NULL;
  char name[] = "ws", collection[] = "a", workflow[] = "story.aesop";
  const h2_gizclaw_str_t ws = {name, 2u}, col = {collection, 1u},
                         flow = {workflow, 11u};
  static const uint8_t get_request[] = {0x0a, 2, 'w', 's'};
  static const uint8_t get_response[] = {0x0a, 10, 0x1a, 2,    'w', 's',
                                         0x32, 4,  'c',  'h',  'a', 't',
                                         0x12, 1,  'p',  0x1a, 1,   'r'};
  test_contact_rpc_t mock = {.expected_method =
                                 H2_GIZCLAW_RPC_SERVER_WORKSPACE_GET,
                             .expected_request = get_request,
                             .expected_request_len = sizeof(get_request),
                             .response = get_response,
                             .response_len = sizeof(get_response)};
  workspace_test_use_single(&mock);
  assert(h2_gizclaw_req_create_workspace_get(service, 1u, ws, 1234u,
                                             &request) == H2_PAL_OK);
  h2_gizclaw_workspace_get_result_t get;
  assert(h2_gizclaw_resp_parse_workspace_get(request, &storage, &get) ==
         H2_PAL_ERR_INVALID_STATE);
  name[0] = 'x';
  assert(mock.calls == 0);
  assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
  assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK &&
         mock.request_matches);
  h2_gizclaw_workspace_t value;
  assert(h2_gizclaw_resp_parse_workspace_delete(request, &storage, &value) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_resp_parse_workspace_get(request, &storage, &get) ==
         H2_PAL_OK);
  h2_gizclaw_req_release(request);
  name[0] = 'w';
  assert(strcmp(get.workspace.name, "ws") == 0 &&
         strcmp(get.runtime_profile_name, "p") == 0);
  assert(h2_gizclaw_rpc_workspace_get(service, ws, 1234u, &storage, &get) ==
         H2_PAL_OK);
  static const uint8_t create_request[] = {
      0x0a, 20,  0x0a, 2,   'w', 's', 0x1a, 11,  's',  't', 'o',
      'r',  'y', '.',  'a', 'e', 's', 'o',  'p', 0x2a, 1,   'a'};
  static const uint8_t value_response[] = {0x0a, 10, 0x1a, 2,   'w', 's',
                                           0x32, 4,  'c',  'h', 'a', 't'};
  mock = (test_contact_rpc_t){.expected_method =
                                  H2_GIZCLAW_RPC_SERVER_WORKSPACE_CREATE,
                              .expected_request = create_request,
                              .expected_request_len = sizeof(create_request),
                              .response = value_response,
                              .response_len = sizeof(value_response)};
  workspace_test_use_single(&mock);
  assert(h2_gizclaw_req_create_workspace_create(service, 1u, col, flow, ws,
                                                1234u, &request) == H2_PAL_OK);
  memset(workflow, 'x', 11u);
  collection[0] = 'b';
  assert(mock.calls == 0);
  assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
  assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK &&
         mock.request_matches);
  assert(h2_gizclaw_resp_parse_workspace_create(request, &storage, &value) ==
         H2_PAL_OK);
  assert(strcmp(value.collection, "a") == 0);
  h2_gizclaw_req_release(request);
  memcpy(workflow, "story.aesop", 11u);
  collection[0] = 'a';
  assert(h2_gizclaw_rpc_workspace_create(service, col, flow, ws, 1234u,
                                         &storage, &value) == H2_PAL_OK);
  assert(h2_gizclaw_req_create_workspace_create(
             service, 1u, col, (h2_gizclaw_str_t){"story..esop", 11u}, ws,
             1234u, &request) == H2_PAL_ERR_INVALID_ARG &&
         request == NULL);

  static const uint8_t list_request[] = {0x10, 1, 0x22, 1, 'a'};
  static const uint8_t list_response[] = {0x12, 10, 0x1a, 2,    'w', 's',
                                          0x32, 4,  'c',  'h',  'a', 't',
                                          0x22, 1,  'p',  0x2a, 1,   'r'};
  mock = (test_contact_rpc_t){.expected_method =
                                  H2_GIZCLAW_RPC_SERVER_WORKSPACE_LIST,
                              .expected_request = list_request,
                              .expected_request_len = sizeof(list_request),
                              .response = list_response,
                              .response_len = sizeof(list_response)};
  h2_gizclaw_workspace_page_t page;
  assert(h2_gizclaw_req_create_workspace_list(service, 1u, col,
                                              (h2_gizclaw_str_t){0}, 1u, 1234u,
                                              &request) == H2_PAL_OK);
  assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
  assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK &&
         mock.request_matches);
  h2_gizclaw_resp_storage_t tiny = {.data = buffer, .capacity = 1u};
  assert(h2_gizclaw_resp_parse_workspace_list(request, &tiny, &page) ==
             H2_PAL_ERR_NO_SPACE &&
         tiny.used == 0u);
  assert(h2_gizclaw_resp_parse_workspace_list(request, &storage, &page) ==
         H2_PAL_OK);
  assert(page.count == 1u && strcmp(page.items[0].collection, "a") == 0);
  h2_gizclaw_req_release(request);
  assert(h2_gizclaw_rpc_workspace_list(service, col, (h2_gizclaw_str_t){0}, 1u,
                                       1234u, &storage, &page) == H2_PAL_OK);

  static const uint8_t history_request[] = {0x10, 1, 0x18, 1,
                                            0x22, 2, 'w',  's'};
  static const uint8_t history_response[] = {0x0a, 2, 0x08, 1};
  mock = (test_contact_rpc_t){.expected_method =
                                  H2_GIZCLAW_RPC_SERVER_WORKSPACE_HISTORY_LIST,
                              .expected_request = history_request,
                              .expected_request_len = sizeof(history_request),
                              .response = history_response,
                              .response_len = sizeof(history_response)};
  h2_gizclaw_workspace_history_page_t history;
  assert(h2_gizclaw_req_create_workspace_history_list(
             service, 1u, ws, (h2_gizclaw_str_t){0}, 1u,
             H2_GIZCLAW_WORKSPACE_HISTORY_ORDER_ASC, 1234u,
             &request) == H2_PAL_OK);
  assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
  assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK &&
         mock.request_matches);
  assert(h2_gizclaw_resp_parse_workspace_history_list(request, &storage,
                                                      &history) == H2_PAL_OK &&
         history.available);
  h2_gizclaw_req_release(request);
  assert(h2_gizclaw_rpc_workspace_history_list(
             service, ws, (h2_gizclaw_str_t){0}, 1u,
             H2_GIZCLAW_WORKSPACE_HISTORY_ORDER_ASC, 1234u, &storage,
             &history) == H2_PAL_OK);

  static const uint8_t activate_request[] = {0x0a, 4, 0x0a, 2, 'w', 's'};
  static const uint8_t activate_response[] = {0x0a, 10, 0x0a, 2, 'w', 's',
                                              0x40, 3,  0x6a, 2, 'w', 's'};
  test_contact_rpc_t steps[] = {
      {.expected_method = H2_GIZCLAW_RPC_SERVER_RUN_WORKSPACE_SET,
       .expected_request = activate_request,
       .expected_request_len = sizeof(activate_request),
       .response = activate_response,
       .response_len = sizeof(activate_response)},
      {.expected_method = H2_GIZCLAW_RPC_SERVER_RUN_WORKSPACE_RELOAD,
       .response = activate_response,
       .response_len = sizeof(activate_response)}};
  test_rpc_sequence_t sequence = {.steps = steps, .step_count = 2u};
  workspace_test_use_sequence(&sequence);
  h2_gizclaw_workspace_activation_t activation;
  assert(h2_gizclaw_req_create_workspace_activate(service, 1u, ws, 1234u,
                                                  &request) == H2_PAL_OK);
  assert(sequence.next_step == 0u);
  assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
  assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK);
  assert(sequence.next_step == 1u && steps[0].request_matches);
  assert(h2_gizclaw_resp_parse_workspace_activate(request, &storage,
                                                  &activation) == H2_PAL_OK);
  assert(activation.runtime_state == H2_GIZCLAW_WORKSPACE_RUNTIME_RUNNING);
  assert(h2_gizclaw_resp_parse_workspace_reload(
             request, &storage, &activation) == H2_PAL_ERR_INVALID_ARG);
  h2_gizclaw_req_release(request);
  assert(h2_gizclaw_req_create_workspace_reload(service, 2u, 1234u, &request) ==
         H2_PAL_OK);
  assert(sequence.next_step == 1u);
  assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
  assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK);
  assert(sequence.next_step == 2u && steps[1].request_matches);
  assert(h2_gizclaw_resp_parse_workspace_activate(
             request, &storage, &activation) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_resp_parse_workspace_reload(request, &storage,
                                                &activation) == H2_PAL_OK);
  h2_gizclaw_req_release(request);
  sequence.next_step = 0u;
  assert(h2_gizclaw_rpc_workspace_activate(service, ws, 1234u, &storage,
                                           &activation) == H2_PAL_OK);
  assert(sequence.next_step == 1u);
  assert(h2_gizclaw_rpc_workspace_reload(service, 1234u, &storage,
                                         &activation) == H2_PAL_OK);
  assert(sequence.next_step == 2u);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
  assert(strcmp(get.workspace.name, "ws") == 0 &&
         strcmp(value.collection, "a") == 0 &&
         strcmp(page.items[0].name, "ws") == 0 &&
         strcmp(activation.workspace_name, "ws") == 0);
}

static void test_workspace_selection_boundaries(void) {
  static const uint8_t payload[] = {0x0a, 4, 0x0a, 2, 'w', 's'};
  for (unsigned reload = 0u; reload < 2u; ++reload) {
    for (unsigned mode = 0u; mode < 8u; ++mode) {
      test_env_t env;
      h2_gizclaw_service_t *service = create_profile_service(&env);
      /* PeerRunWorkspaceState.workspace_name is field 13, not active field 1.
       */
      uint8_t response[] = {0x0a, 6, 0x6a, 2, 'w', 's', 0x40, 2};
      if (mode == 2u)
        response[0] = 0xff;
      if (mode == 3u)
        response[5] = 'x';
      test_contact_rpc_t mock = {
          .expected_method = reload ? H2_GIZCLAW_RPC_SERVER_RUN_WORKSPACE_RELOAD
                                    : H2_GIZCLAW_RPC_SERVER_RUN_WORKSPACE_SET,
          .expected_request = reload ? NULL : payload,
          .expected_request_len = reload ? 0u : sizeof(payload),
          .response = response,
          .response_len = sizeof(response),
          .has_error = mode == 4u || mode == 5u,
          .error_code = mode == 4u ? H2_GIZCLAW_RPC_ERROR_NOT_FOUND
                                   : H2_GIZCLAW_RPC_ERROR_METHOD_NOT_FOUND,
      };
      workspace_test_use_single(&mock);
      h2_gizclaw_async_rpc_test_set_ops(&workspace_test_ops);
      char name[] = "ws";
      h2_gizclaw_req_t *request = NULL;
      assert((reload ? h2_gizclaw_req_create_workspace_reload(service, 1u,
                                                              1234u, &request)
                     : h2_gizclaw_req_create_workspace_activate(
                           service, 1u, (h2_gizclaw_str_t){name, 2u}, 1234u,
                           &request)) == H2_PAL_OK);
      name[0] = 'X'; /* CREATE must already own the selected name. */
      assert(mock.calls == 0);
      if (mode != 7u) {
        if (mode == 6u)
          atomic_store(&env.connect_gate, false);
        assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
        assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
        if (mode == 6u) {
          assert(h2_gizclaw_req_cancel(request) == H2_PAL_OK);
          atomic_store(&env.connect_gate, true);
        }
        const int result = mode == 4u   ? H2_PAL_ERR_NOT_FOUND
                           : mode == 5u ? H2_GIZCLAW_ERR_REMOTE
                           : mode == 6u ? H2_PAL_ERR_CLOSED
                                        : H2_PAL_OK;
        assert(h2_gizclaw_req_wait(request, 2000u) == result);
        assert(h2_gizclaw_req_wait(request, 0u) == result);
        uint8_t bytes[256] = {0x51, 0x52, 0x53};
        h2_gizclaw_resp_storage_t storage = {
            .data = bytes,
            .capacity = mode == 1u ? 4u : sizeof(bytes),
            .used = 3u};
        h2_gizclaw_workspace_activation_t value;
        memset(&value, 0xa5, sizeof(value));
        const int parsed = mode == 1u              ? H2_PAL_ERR_NO_SPACE
                           : mode == 2u            ? H2_PAL_ERR_FORMAT
                           : mode == 3u && !reload ? H2_PAL_ERR_INVALID_STATE
                                                   : result;
        const int actual = reload ? h2_gizclaw_resp_parse_workspace_reload(
                                        request, &storage, &value)
                                  : h2_gizclaw_resp_parse_workspace_activate(
                                        request, &storage, &value);
        if (actual != parsed)
          fprintf(stderr, "selection reload=%u mode=%u got=%d expected=%d\n",
                  reload, mode, actual, parsed);
        assert(actual == parsed);
        if (parsed != H2_PAL_OK)
          assert(storage.used == 3u && value.workspace_name == NULL);
        else
          assert(value.runtime_state == H2_GIZCLAW_WORKSPACE_RUNTIME_STARTING);
        assert(bytes[0] == 0x51 && bytes[1] == 0x52 && bytes[2] == 0x53);
        assert(mode == 6u ? mock.calls == 0
                          : mock.calls == 1 && mock.request_matches);
      }
      h2_gizclaw_req_release(request);
      assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
      assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
      h2_gizclaw_async_rpc_test_set_ops(NULL);
    }
  }
}

static bool test_encode_contact_response_with_name(uint8_t *buffer,
                                                   size_t capacity,
                                                   const char *display_name,
                                                   size_t display_name_len,
                                                   size_t *out_len) {
  gizclaw_rpc_v1_ContactPutResponse response =
      gizclaw_rpc_v1_ContactPutResponse_init_zero;
  test_contact_text_t text[] = {
      {.data = "2026-07-27T00:00:00Z", .len = 20u},
      {.data = display_name, .len = display_name_len},
      {.data = "contact-1", .len = 9u},
      {.data = "+8613900000000", .len = 14u},
      {.data = "2026-07-27T00:01:00Z", .len = 20u},
  };
  response.has_value = true;
  response.value.created_at.funcs.encode = test_encode_contact_text;
  response.value.created_at.arg = &text[0];
  response.value.display_name.funcs.encode = test_encode_contact_text;
  response.value.display_name.arg = &text[1];
  response.value.name.funcs.encode = test_encode_contact_text;
  response.value.name.arg = &text[2];
  response.value.phone_number.funcs.encode = test_encode_contact_text;
  response.value.phone_number.arg = &text[3];
  response.value.updated_at.funcs.encode = test_encode_contact_text;
  response.value.updated_at.arg = &text[4];
  pb_ostream_t stream = pb_ostream_from_buffer(buffer, capacity);
  if (!pb_encode(&stream, gizclaw_rpc_v1_ContactPutResponse_fields,
                 &response)) {
    return false;
  }
  *out_len = stream.bytes_written;
  return true;
}

static bool test_encode_contact_response(uint8_t *buffer, size_t capacity,
                                         size_t *out_len) {
  return test_encode_contact_response_with_name(buffer, capacity, "Alice", 5u,
                                                out_len);
}

static void test_contact_mutation_requests(void) {
  int fails = 0;
  test_env_t env;
  h2_gizclaw_service_t *service = create_profile_service(&env);
  h2_gizclaw_async_rpc_test_set_ops(&workspace_test_ops);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  uint8_t buffer[8192];
  h2_gizclaw_resp_storage_t storage = {.data = buffer,
                                       .capacity = sizeof(buffer)};
  uint8_t response[128];
  size_t response_len = 0u;
  fails += workspace_expect(
      test_encode_contact_response(response, sizeof(response), &response_len),
      "contact mutation response fixture encodes");
  const uint8_t put_request[] = {
      0x0a, 0x05, 'A', 'l', 'i', 'c', 'e',  0x12, 0x09, 'c', 'o', 'n',
      't',  'a',  'c', 't', '-', '1', 0x1a, 0x0e, '+',  '8', '6', '1',
      '3',  '9',  '0', '0', '0', '0', '0',  '0',  '0',  '0',
  };
  test_contact_rpc_t mock = {
      .expected_method = H2_GIZCLAW_RPC_SERVER_CONTACT_PUT,
      .expected_request = put_request,
      .expected_request_len = sizeof(put_request),
      .response = response,
      .response_len = response_len,
  };
  workspace_test_use_single(&mock);
  h2_gizclaw_contact_t contact = {0};
  fails += workspace_expect(
      h2_gizclaw_rpc_contact_put(
          service, (h2_gizclaw_str_t){.data = "contact-1", .len = 9u},
          (h2_gizclaw_str_t){.data = "Alice", .len = 5u},
          (h2_gizclaw_str_t){.data = "+8613900000000", .len = 14u}, 1234u,
          &storage, &contact) == H2_PAL_OK,
      "contact put accepts mocked RPC response");
  fails += workspace_expect(
      mock.calls == 1 && mock.request_matches &&
          strcmp(contact.name, "contact-1") == 0 &&
          strcmp(contact.display_name, "Alice") == 0 &&
          strcmp(contact.phone_number, "+8613900000000") == 0 &&
          strcmp(contact.created_at, "2026-07-27T00:00:00Z") == 0 &&
          strcmp(contact.updated_at, "2026-07-27T00:01:00Z") == 0,
      "contact put maps request and owns decoded response");
  storage.used = 0u;
  memset(&contact, 0, sizeof(contact));
  fails += workspace_expect(
      contact.name == NULL && contact.display_name == NULL &&
          contact.phone_number == NULL && contact.created_at == NULL &&
          contact.updated_at == NULL,
      "contact put response releases owned strings");

  const uint8_t delete_request[] = {
      0x0a, 0x09, 'c', 'o', 'n', 't', 'a', 'c', 't', '-', '1',
  };
  mock = (test_contact_rpc_t){
      .expected_method = H2_GIZCLAW_RPC_SERVER_CONTACT_GET,
      .expected_request = delete_request,
      .expected_request_len = sizeof(delete_request),
      .response = response,
      .response_len = response_len,
  };
  fails += workspace_expect(
      h2_gizclaw_rpc_contact_get(
          service, (h2_gizclaw_str_t){.data = "contact-1", .len = 9u}, 1234u,
          &storage, &contact) == H2_PAL_OK,
      "contact get accepts mocked RPC response");
  fails +=
      workspace_expect(mock.calls == 1 && mock.request_matches &&
                           strcmp(contact.name, "contact-1") == 0,
                       "contact get maps stable name and decodes response");
  storage.used = 0u;
  memset(&contact, 0, sizeof(contact));

  mock = (test_contact_rpc_t){
      .expected_method = H2_GIZCLAW_RPC_SERVER_CONTACT_GET,
      .expected_request = delete_request,
      .expected_request_len = sizeof(delete_request),
      .has_error = true,
      .error_code = H2_GIZCLAW_RPC_ERROR_NOT_FOUND,
  };
  contact.name = (char *)0x1;
  fails += workspace_expect(
      h2_gizclaw_rpc_contact_get(
          service, (h2_gizclaw_str_t){.data = "contact-1", .len = 9u}, 1234u,
          &storage, &contact) == H2_PAL_ERR_NOT_FOUND &&
          contact.name == NULL,
      "contact get maps RPC not found and clears output");

  mock = (test_contact_rpc_t){
      .expected_method = H2_GIZCLAW_RPC_SERVER_CONTACT_DELETE,
      .expected_request = delete_request,
      .expected_request_len = sizeof(delete_request),
      .response = response,
      .response_len = response_len,
  };
  fails += workspace_expect(
      h2_gizclaw_rpc_contact_delete(
          service, (h2_gizclaw_str_t){.data = "contact-1", .len = 9u}, 1234u,
          &storage, &contact) == H2_PAL_OK,
      "contact delete accepts mocked RPC response");
  fails += workspace_expect(mock.calls == 1 && mock.request_matches &&
                                strcmp(contact.name, "contact-1") == 0,
                            "contact delete maps request and decodes response");
  storage.used = 0u;
  memset(&contact, 0, sizeof(contact));

  const uint8_t malformed_response[] = {0x0a, 0x01, 0xff};
  mock = (test_contact_rpc_t){
      .expected_method = H2_GIZCLAW_RPC_SERVER_CONTACT_DELETE,
      .expected_request = delete_request,
      .expected_request_len = sizeof(delete_request),
      .response = malformed_response,
      .response_len = sizeof(malformed_response),
  };
  fails += workspace_expect(
      h2_gizclaw_rpc_contact_delete(
          service, (h2_gizclaw_str_t){.data = "contact-1", .len = 9u}, 1234u,
          &storage, &contact) == H2_PAL_ERR_FORMAT,
      "contact delete rejects malformed mocked response");
  fails += workspace_expect(
      mock.calls == 1 && mock.request_matches && contact.name == NULL &&
          contact.display_name == NULL && contact.phone_number == NULL &&
          contact.created_at == NULL && contact.updated_at == NULL,
      "malformed contact response releases partial ownership");

  char oversized_name[H2_GIZCLAW_CONTACT_DISPLAY_NAME_MAX_BYTES + 1u];
  memset(oversized_name, 'A', sizeof(oversized_name));
  uint8_t oversized_response[512];
  size_t oversized_response_len = 0u;
  fails += workspace_expect(test_encode_contact_response_with_name(
                                oversized_response, sizeof(oversized_response),
                                oversized_name, sizeof(oversized_name),
                                &oversized_response_len),
                            "oversized contact response fixture encodes");
  mock = (test_contact_rpc_t){
      .expected_method = H2_GIZCLAW_RPC_SERVER_CONTACT_GET,
      .expected_request = delete_request,
      .expected_request_len = sizeof(delete_request),
      .response = oversized_response,
      .response_len = oversized_response_len,
  };
  fails += workspace_expect(
      h2_gizclaw_rpc_contact_get(
          service, (h2_gizclaw_str_t){.data = "contact-1", .len = 9u}, 1234u,
          &storage, &contact) == H2_PAL_ERR_FORMAT,
      "contact get rejects an oversized decoded field");
  fails += workspace_expect(
      mock.calls == 1 && contact.name == NULL && contact.display_name == NULL &&
          contact.phone_number == NULL && contact.created_at == NULL &&
          contact.updated_at == NULL,
      "oversized contact response releases partial ownership");
  assert(fails == 0);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

static void test_contact_and_group_public_paths(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_profile_service(&env);
  h2_gizclaw_async_rpc_test_set_ops(&workspace_test_ops);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  uint8_t buffer[8192];
  h2_gizclaw_resp_storage_t storage = {.data = buffer,
                                       .capacity = sizeof(buffer)};
  h2_gizclaw_req_t *request = NULL;
  h2_gizclaw_contact_t snapshot = {0};
  test_contact_rpc_t mock;
  workspace_test_use_single(&mock);
  {
    char text[] = "x";
    static const uint8_t input[] = {0x0a, 1, 'x', 0x10, 1};
    static const uint8_t response[] = {0x12, 3, 0x1a, 1, 'x'};
    mock = (test_contact_rpc_t){.expected_method =
                                    H2_GIZCLAW_RPC_SERVER_CONTACT_LIST,
                                .expected_request = input,
                                .expected_request_len = sizeof(input),
                                .response = response,
                                .response_len = sizeof(response)};
    h2_gizclaw_contact_page_t value;
    assert(h2_gizclaw_req_create_contact_list(service, 1u,
                                              (h2_gizclaw_str_t){text, 1u}, 1u,
                                              1234u, &request) == H2_PAL_OK);
    assert(mock.calls == 0);
    assert(h2_gizclaw_resp_parse_contact_list(request, &storage, &value) ==
           H2_PAL_ERR_INVALID_STATE);
    text[0] = 'y';
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK &&
           mock.request_matches);
    h2_gizclaw_resp_storage_t tiny = {.data = buffer, .capacity = 1u};
    assert(h2_gizclaw_resp_parse_contact_list(request, &tiny, &value) ==
               H2_PAL_ERR_NO_SPACE &&
           tiny.used == 0u);
    assert(h2_gizclaw_resp_parse_contact_list(request, &storage, &value) ==
           H2_PAL_OK);
    assert(strcmp(value.items[0].name, "x") == 0);

    h2_gizclaw_req_release(request);
    text[0] = 'x';
    assert(h2_gizclaw_rpc_contact_list(service, (h2_gizclaw_str_t){text, 1u},
                                       1u, 1234u, &storage,
                                       &value) == H2_PAL_OK);
    /* A page exceeding the requested limit must not escape or consume storage.
     */
    static const uint8_t too_many[] = {0x12, 3, 0x1a, 1, 'x',
                                       0x12, 3, 0x1a, 1, 'x'};
    mock.response = too_many;
    mock.response_len = sizeof(too_many);
    size_t checkpoint = storage.used;
    assert(h2_gizclaw_rpc_contact_list(service, (h2_gizclaw_str_t){text, 1u},
                                       1u, 1234u, &storage,
                                       &value) == H2_PAL_ERR_FORMAT);
    assert(value.count == 0u && storage.used == checkpoint);
    static const uint8_t bad_cursor[] = {0x08, 1, 0x1a, 2, 0xc0, 0x80};
    mock.response = bad_cursor;
    mock.response_len = sizeof(bad_cursor);
    assert(h2_gizclaw_rpc_contact_list(service, (h2_gizclaw_str_t){text, 1u},
                                       1u, 1234u, &storage,
                                       &value) == H2_PAL_ERR_FORMAT);
    assert(storage.used == checkpoint);
  }
  {
    char text[] = "x";
    static const uint8_t input[] = {0x0a, 1, 'x'};
    static const uint8_t response[] = {0x0a, 3, 0x1a, 1, 'x'};
    mock = (test_contact_rpc_t){.expected_method =
                                    H2_GIZCLAW_RPC_SERVER_CONTACT_GET,
                                .expected_request = input,
                                .expected_request_len = sizeof(input),
                                .response = response,
                                .response_len = sizeof(response)};
    h2_gizclaw_contact_t value;
    assert(h2_gizclaw_req_create_contact_get(service, 1u,
                                             (h2_gizclaw_str_t){text, 1u},
                                             1234u, &request) == H2_PAL_OK);
    assert(mock.calls == 0);
    assert(h2_gizclaw_resp_parse_contact_get(request, &storage, &value) ==
           H2_PAL_ERR_INVALID_STATE);
    text[0] = 'y';
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK &&
           mock.request_matches);
    h2_gizclaw_resp_storage_t tiny = {.data = buffer, .capacity = 1u};
    assert(h2_gizclaw_resp_parse_contact_get(request, &tiny, &value) ==
               H2_PAL_ERR_NO_SPACE &&
           tiny.used == 0u);
    assert(h2_gizclaw_resp_parse_contact_get(request, &storage, &value) ==
           H2_PAL_OK);
    assert(strcmp(value.name, "x") == 0);
    snapshot = value;
    h2_gizclaw_req_release(request);
    text[0] = 'x';
    assert(h2_gizclaw_rpc_contact_get(service, (h2_gizclaw_str_t){text, 1u},
                                      1234u, &storage, &value) == H2_PAL_OK);
  }
  {
    char text[] = "x";
    static const uint8_t input[] = {0x0a, 1, 'x', 0x12, 1, 'D', 0x1a, 1, 'P'};
    static const uint8_t response[] = {0x0a, 3, 0x1a, 1, 'x'};
    mock = (test_contact_rpc_t){.expected_method =
                                    H2_GIZCLAW_RPC_SERVER_CONTACT_CREATE,
                                .expected_request = input,
                                .expected_request_len = sizeof(input),
                                .response = response,
                                .response_len = sizeof(response)};
    h2_gizclaw_contact_t value;
    assert(h2_gizclaw_req_create_contact_create(
               service, 1u, (h2_gizclaw_str_t){text, 1u},
               (h2_gizclaw_str_t){"D", 1u}, (h2_gizclaw_str_t){"P", 1u}, 1234u,
               &request) == H2_PAL_OK);
    assert(mock.calls == 0);
    assert(h2_gizclaw_resp_parse_contact_create(request, &storage, &value) ==
           H2_PAL_ERR_INVALID_STATE);
    text[0] = 'y';
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK &&
           mock.request_matches);
    h2_gizclaw_resp_storage_t tiny = {.data = buffer, .capacity = 1u};
    assert(h2_gizclaw_resp_parse_contact_create(request, &tiny, &value) ==
               H2_PAL_ERR_NO_SPACE &&
           tiny.used == 0u);
    assert(h2_gizclaw_resp_parse_contact_create(request, &storage, &value) ==
           H2_PAL_OK);
    assert(strcmp(value.name, "x") == 0);
    snapshot = value;
    h2_gizclaw_req_release(request);
    text[0] = 'x';
    assert(h2_gizclaw_rpc_contact_create(service, (h2_gizclaw_str_t){text, 1u},
                                         (h2_gizclaw_str_t){"D", 1u},
                                         (h2_gizclaw_str_t){"P", 1u}, 1234u,
                                         &storage, &value) == H2_PAL_OK);
  }
  {
    char text[] = "x";
    static const uint8_t input[] = {0x0a, 1, 'D', 0x12, 1, 'x', 0x1a, 1, 'P'};
    static const uint8_t response[] = {0x0a, 3, 0x1a, 1, 'x'};
    mock = (test_contact_rpc_t){.expected_method =
                                    H2_GIZCLAW_RPC_SERVER_CONTACT_PUT,
                                .expected_request = input,
                                .expected_request_len = sizeof(input),
                                .response = response,
                                .response_len = sizeof(response)};
    h2_gizclaw_contact_t value;
    assert(h2_gizclaw_req_create_contact_put(
               service, 1u, (h2_gizclaw_str_t){text, 1u},
               (h2_gizclaw_str_t){"D", 1u}, (h2_gizclaw_str_t){"P", 1u}, 1234u,
               &request) == H2_PAL_OK);
    assert(mock.calls == 0);
    assert(h2_gizclaw_resp_parse_contact_put(request, &storage, &value) ==
           H2_PAL_ERR_INVALID_STATE);
    text[0] = 'y';
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK &&
           mock.request_matches);
    h2_gizclaw_resp_storage_t tiny = {.data = buffer, .capacity = 1u};
    assert(h2_gizclaw_resp_parse_contact_put(request, &tiny, &value) ==
               H2_PAL_ERR_NO_SPACE &&
           tiny.used == 0u);
    assert(h2_gizclaw_resp_parse_contact_put(request, &storage, &value) ==
           H2_PAL_OK);
    assert(strcmp(value.name, "x") == 0);
    snapshot = value;
    h2_gizclaw_req_release(request);
    text[0] = 'x';
    assert(h2_gizclaw_rpc_contact_put(service, (h2_gizclaw_str_t){text, 1u},
                                      (h2_gizclaw_str_t){"D", 1u},
                                      (h2_gizclaw_str_t){"P", 1u}, 1234u,
                                      &storage, &value) == H2_PAL_OK);
  }
  {
    char text[] = "x";
    static const uint8_t input[] = {0x0a, 1, 'x'};
    static const uint8_t response[] = {0x0a, 3, 0x1a, 1, 'x'};
    mock = (test_contact_rpc_t){.expected_method =
                                    H2_GIZCLAW_RPC_SERVER_CONTACT_DELETE,
                                .expected_request = input,
                                .expected_request_len = sizeof(input),
                                .response = response,
                                .response_len = sizeof(response)};
    h2_gizclaw_contact_t value;
    assert(h2_gizclaw_req_create_contact_delete(service, 1u,
                                                (h2_gizclaw_str_t){text, 1u},
                                                1234u, &request) == H2_PAL_OK);
    assert(mock.calls == 0);
    assert(h2_gizclaw_resp_parse_contact_delete(request, &storage, &value) ==
           H2_PAL_ERR_INVALID_STATE);
    text[0] = 'y';
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK &&
           mock.request_matches);
    h2_gizclaw_resp_storage_t tiny = {.data = buffer, .capacity = 1u};
    assert(h2_gizclaw_resp_parse_contact_delete(request, &tiny, &value) ==
               H2_PAL_ERR_NO_SPACE &&
           tiny.used == 0u);
    assert(h2_gizclaw_resp_parse_contact_delete(request, &storage, &value) ==
           H2_PAL_OK);
    assert(strcmp(value.name, "x") == 0);
    snapshot = value;
    h2_gizclaw_req_release(request);
    text[0] = 'x';
    assert(h2_gizclaw_rpc_contact_delete(service, (h2_gizclaw_str_t){text, 1u},
                                         1234u, &storage, &value) == H2_PAL_OK);
  }
  {
    char text[] = "x";
    static const uint8_t input[] = {0x0a, 1, 'x', 0x10, 1};
    static const uint8_t response[] = {0x12, 3, 0x32, 1, 'x'};
    mock = (test_contact_rpc_t){.expected_method =
                                    H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_LIST,
                                .expected_request = input,
                                .expected_request_len = sizeof(input),
                                .response = response,
                                .response_len = sizeof(response)};
    h2_gizclaw_friend_group_page_t value;
    assert(h2_gizclaw_req_create_friend_group_list(
               service, 1u, (h2_gizclaw_str_t){text, 1u}, 1u, 1234u,
               &request) == H2_PAL_OK);
    assert(mock.calls == 0);
    assert(h2_gizclaw_resp_parse_friend_group_list(request, &storage, &value) ==
           H2_PAL_ERR_INVALID_STATE);
    text[0] = 'y';
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK &&
           mock.request_matches);
    h2_gizclaw_resp_storage_t tiny = {.data = buffer, .capacity = 1u};
    assert(h2_gizclaw_resp_parse_friend_group_list(request, &tiny, &value) ==
               H2_PAL_ERR_NO_SPACE &&
           tiny.used == 0u);
    assert(h2_gizclaw_resp_parse_friend_group_list(request, &storage, &value) ==
           H2_PAL_OK);
    assert(strcmp(value.items[0].name, "x") == 0);

    h2_gizclaw_req_release(request);
    text[0] = 'x';
    assert(h2_gizclaw_rpc_friend_group_list(
               service, (h2_gizclaw_str_t){text, 1u}, 1u, 1234u, &storage,
               &value) == H2_PAL_OK);
    /* A page exceeding the requested limit must not escape or consume storage.
     */
    static const uint8_t too_many[] = {0x12, 3, 0x32, 1, 'x',
                                       0x12, 3, 0x32, 1, 'x'};
    mock.response = too_many;
    mock.response_len = sizeof(too_many);
    size_t checkpoint = storage.used;
    assert(h2_gizclaw_rpc_friend_group_list(
               service, (h2_gizclaw_str_t){text, 1u}, 1u, 1234u, &storage,
               &value) == H2_PAL_ERR_FORMAT);
    assert(value.count == 0u && storage.used == checkpoint);
    static const uint8_t bad_cursor[] = {0x08, 1, 0x1a, 2, 0xc0, 0x80};
    mock.response = bad_cursor;
    mock.response_len = sizeof(bad_cursor);
    assert(h2_gizclaw_rpc_friend_group_list(
               service, (h2_gizclaw_str_t){text, 1u}, 1u, 1234u, &storage,
               &value) == H2_PAL_ERR_FORMAT);
    assert(storage.used == checkpoint);
  }
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
  assert(strcmp(snapshot.name, "x") == 0);
}

static int retry_rpc_start(h2_gizclaw_client_t *client,
                           h2_gizclaw_rpc_method_t method,
                           h2_gizclaw_rpc_bytes_t payload, uint32_t timeout_ms,
                           h2_gizclaw_rpc_request_t **out_request) {
  assert(client == (h2_gizclaw_client_t *)s_env);
  assert(method == H2_GIZCLAW_RPC_SERVER_INFO_GET && payload.len == 0u);
  assert(timeout_ms == 1234u - atomic_load(&s_env->clock_ms));
  unsigned attempt = atomic_fetch_add(&s_env->rpc_start_count, 1u);
  *out_request = NULL;
  if (s_env->retry_mode != 0u || attempt < 2u) {
    if (s_env->retry_mode != 2u)
      atomic_fetch_add(&s_env->clock_ms, 100u);
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  *out_request = (h2_gizclaw_rpc_request_t *)s_env;
  return H2_PAL_OK;
}

static void test_req_start_backpressure_deadline_and_cancel(void) {
  static const h2_gizclaw_async_rpc_ops_t ops = {.start = retry_rpc_start,
                                                 .result = fake_profile_result,
                                                 .cancel = fake_rpc_cancel,
                                                 .destroy = fake_rpc_destroy};
  static const h2_pal_time_vtable_t tv = {.get_monotonic_ms = fake_req_clock};
  for (unsigned mode = 0; mode < 4u; ++mode) {
    test_env_t env;
    h2_gizclaw_service_t *service = create_profile_service(&env);
    const h2_pal_time_api_t time = {.user = &env, .vtable = &tv};
    service->client_config.time = mode == 3u ? NULL : &time;
    env.retry_mode = mode;
    h2_gizclaw_async_rpc_test_set_ops(&ops);
    assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
    h2_gizclaw_req_t *request = NULL;
    assert(h2_gizclaw_req_create_profile_get(service, 1u, 1234u, &request) ==
           H2_PAL_OK);
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    if (mode == 2u) {
      wait_for_count(&env.rpc_start_count, 2u);
      assert(h2_gizclaw_req_cancel(request) == H2_PAL_OK);
    }
    const h2_pal_result_t rc = h2_gizclaw_req_wait(request, 2000u);
    if (mode == 0u) {
      assert(rc == H2_PAL_OK && atomic_load(&env.rpc_start_count) == 3u);
      h2_gizclaw_profile_t profile;
      assert(h2_gizclaw_resp_parse_profile_get(request, &profile) == H2_PAL_OK);
    } else if (mode == 1u) {
      assert(rc == H2_PAL_ERR_TIMEOUT &&
             atomic_load(&env.rpc_start_count) == 13u);
    } else if (mode == 2u) {
      assert(rc == H2_PAL_ERR_CLOSED);
    } else {
      assert(rc != H2_PAL_OK && rc != H2_PAL_ERR_WOULD_BLOCK &&
             rc != H2_PAL_ERR_TIMEOUT);
      assert(atomic_load(&env.rpc_start_count) == 1u);
    }
    assert(atomic_load(&env.rpc_destroy_count) == (mode == 0u ? 1u : 0u));
    h2_gizclaw_req_release(request);
    assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
    assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
  }
}

static void test_friend_public_request_paths(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_profile_service(&env);
  h2_gizclaw_async_rpc_test_set_ops(&workspace_test_ops);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  uint8_t buffer[8192];
  h2_gizclaw_resp_storage_t storage = {.data = buffer,
                                       .capacity = sizeof(buffer)};
  h2_gizclaw_req_t *request = NULL;
  test_contact_rpc_t mock;
  workspace_test_use_single(&mock);
  h2_gizclaw_invite_token_t saved_token = {0};
  {
    char text[] = "x";
    static const uint8_t input[] = {0x0a, 1, 'x', 0x10, 1};
    static const uint8_t response[] = {0x12, 3, 0x12, 1, 'x'};
    mock = (test_contact_rpc_t){.expected_method =
                                    H2_GIZCLAW_RPC_SERVER_FRIEND_LIST,
                                .expected_request = input,
                                .expected_request_len = sizeof(input),
                                .response = response,
                                .response_len = sizeof(response)};
    h2_gizclaw_friend_page_t value;
    assert(h2_gizclaw_req_create_friend_list(service, 1u,
                                             (h2_gizclaw_str_t){text, 1u}, 1u,
                                             1234u, &request) == H2_PAL_OK);
    assert(mock.calls == 0);
    assert(h2_gizclaw_resp_parse_friend_list(request, &storage, &value) ==
           H2_PAL_ERR_INVALID_STATE);
    text[0] = 'y';
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK &&
           mock.request_matches);
    assert(h2_gizclaw_resp_parse_friend_list(request, &storage, &value) ==
           H2_PAL_OK);
    assert(strcmp(value.items[0].id, "x") == 0);

    h2_gizclaw_resp_storage_t tiny = {.data = buffer, .capacity = 1u};
    assert(h2_gizclaw_resp_parse_friend_list(request, &tiny, &value) ==
               H2_PAL_ERR_NO_SPACE &&
           tiny.used == 0u);
    h2_gizclaw_req_release(request);
    text[0] = 'x';
    assert(h2_gizclaw_rpc_friend_list(service, (h2_gizclaw_str_t){text, 1u}, 1u,
                                      1234u, &storage, &value) == H2_PAL_OK);
    size_t checkpoint = storage.used;
    static const uint8_t bad_response[] = {0x0a, 1, 0xff};
    mock.response = bad_response;
    mock.response_len = sizeof(bad_response);
    assert(h2_gizclaw_rpc_friend_list(service, (h2_gizclaw_str_t){text, 1u}, 1u,
                                      1234u, &storage,
                                      &value) == H2_PAL_ERR_FORMAT);
    assert(storage.used == checkpoint);
    mock.has_error = true;
    mock.error_code = 404;
    assert(h2_gizclaw_rpc_friend_list(service, (h2_gizclaw_str_t){text, 1u}, 1u,
                                      1234u, &storage,
                                      &value) == H2_PAL_ERR_NOT_FOUND);
  }
  {
    char text[] = "x";
    static const uint8_t input[] = {0x0a, 1, 'x'};
    static const uint8_t response[] = {0x0a, 1, 'k', 0x12, 3, 0x0a, 1, 'D'};
    mock = (test_contact_rpc_t){.expected_method =
                                    H2_GIZCLAW_RPC_SERVER_FRIEND_INFO_GET,
                                .expected_request = input,
                                .expected_request_len = sizeof(input),
                                .response = response,
                                .response_len = sizeof(response)};
    h2_gizclaw_friend_t value;
    assert(h2_gizclaw_req_create_friend_info_get(service, 1u,
                                                 (h2_gizclaw_str_t){text, 1u},
                                                 1234u, &request) == H2_PAL_OK);
    assert(mock.calls == 0);
    assert(h2_gizclaw_resp_parse_friend_info_get(request, &storage, &value) ==
           H2_PAL_ERR_INVALID_STATE);
    text[0] = 'y';
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK &&
           mock.request_matches);
    assert(h2_gizclaw_resp_parse_friend_info_get(request, &storage, &value) ==
           H2_PAL_OK);
    assert(strcmp(value.id, "x") == 0);
    assert(strcmp(value.peer_public_key, "k") == 0 &&
           strcmp(value.name, "D") == 0);

    h2_gizclaw_resp_storage_t tiny = {.data = buffer, .capacity = 1u};
    assert(h2_gizclaw_resp_parse_friend_info_get(request, &tiny, &value) ==
               H2_PAL_ERR_NO_SPACE &&
           tiny.used == 0u);
    h2_gizclaw_req_release(request);
    text[0] = 'x';
    assert(h2_gizclaw_rpc_friend_info_get(service, (h2_gizclaw_str_t){text, 1u},
                                          1234u, &storage,
                                          &value) == H2_PAL_OK);
    size_t checkpoint = storage.used;
    static const uint8_t bad_response[] = {0x0a, 1, 0xff};
    mock.response = bad_response;
    mock.response_len = sizeof(bad_response);
    assert(h2_gizclaw_rpc_friend_info_get(service, (h2_gizclaw_str_t){text, 1u},
                                          1234u, &storage,
                                          &value) == H2_PAL_ERR_FORMAT);
    assert(storage.used == checkpoint);
    mock.has_error = true;
    mock.error_code = 404;
    assert(h2_gizclaw_rpc_friend_info_get(service, (h2_gizclaw_str_t){text, 1u},
                                          1234u, &storage,
                                          &value) == H2_PAL_ERR_NOT_FOUND);
  }
  {
    char text[] = "x";
    static const uint8_t input[] = {0x0a, 1, 'x'};
    static const uint8_t response[] = {0x0a, 3, 0x12, 1, 'x'};
    mock = (test_contact_rpc_t){.expected_method =
                                    H2_GIZCLAW_RPC_SERVER_FRIEND_ADD,
                                .expected_request = input,
                                .expected_request_len = sizeof(input),
                                .response = response,
                                .response_len = sizeof(response)};
    h2_gizclaw_friend_t value;
    assert(h2_gizclaw_req_create_friend_add(service, 1u,
                                            (h2_gizclaw_str_t){text, 1u}, 1234u,
                                            &request) == H2_PAL_OK);
    assert(mock.calls == 0);
    assert(h2_gizclaw_resp_parse_friend_add(request, &storage, &value) ==
           H2_PAL_ERR_INVALID_STATE);
    text[0] = 'y';
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK &&
           mock.request_matches);
    assert(h2_gizclaw_resp_parse_friend_add(request, &storage, &value) ==
           H2_PAL_OK);
    assert(strcmp(value.id, "x") == 0);

    h2_gizclaw_resp_storage_t tiny = {.data = buffer, .capacity = 1u};
    assert(h2_gizclaw_resp_parse_friend_add(request, &tiny, &value) ==
               H2_PAL_ERR_NO_SPACE &&
           tiny.used == 0u);
    h2_gizclaw_req_release(request);
    text[0] = 'x';
    assert(h2_gizclaw_rpc_friend_add(service, (h2_gizclaw_str_t){text, 1u},
                                     1234u, &storage, &value) == H2_PAL_OK);
    size_t checkpoint = storage.used;
    static const uint8_t bad_response[] = {0x0a, 1, 0xff};
    mock.response = bad_response;
    mock.response_len = sizeof(bad_response);
    assert(h2_gizclaw_rpc_friend_add(service, (h2_gizclaw_str_t){text, 1u},
                                     1234u, &storage,
                                     &value) == H2_PAL_ERR_FORMAT);
    assert(storage.used == checkpoint);
    mock.has_error = true;
    mock.error_code = 404;
    assert(h2_gizclaw_rpc_friend_add(service, (h2_gizclaw_str_t){text, 1u},
                                     1234u, &storage,
                                     &value) == H2_PAL_ERR_NOT_FOUND);
  }
  {
    char text[] = "x";
    static const uint8_t input[] = {0x0a, 1, 'x'};
    static const uint8_t response[] = {0x0a, 3, 0x12, 1, 'x'};
    mock = (test_contact_rpc_t){.expected_method =
                                    H2_GIZCLAW_RPC_SERVER_FRIEND_DELETE,
                                .expected_request = input,
                                .expected_request_len = sizeof(input),
                                .response = response,
                                .response_len = sizeof(response)};
    h2_gizclaw_friend_t value;
    assert(h2_gizclaw_req_create_friend_delete(service, 1u,
                                               (h2_gizclaw_str_t){text, 1u},
                                               1234u, &request) == H2_PAL_OK);
    assert(mock.calls == 0);
    assert(h2_gizclaw_resp_parse_friend_delete(request, &storage, &value) ==
           H2_PAL_ERR_INVALID_STATE);
    text[0] = 'y';
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK &&
           mock.request_matches);
    assert(h2_gizclaw_resp_parse_friend_delete(request, &storage, &value) ==
           H2_PAL_OK);
    assert(strcmp(value.id, "x") == 0);

    h2_gizclaw_resp_storage_t tiny = {.data = buffer, .capacity = 1u};
    assert(h2_gizclaw_resp_parse_friend_delete(request, &tiny, &value) ==
               H2_PAL_ERR_NO_SPACE &&
           tiny.used == 0u);
    h2_gizclaw_req_release(request);
    text[0] = 'x';
    assert(h2_gizclaw_rpc_friend_delete(service, (h2_gizclaw_str_t){text, 1u},
                                        1234u, &storage, &value) == H2_PAL_OK);
    size_t checkpoint = storage.used;
    static const uint8_t bad_response[] = {0x0a, 1, 0xff};
    mock.response = bad_response;
    mock.response_len = sizeof(bad_response);
    assert(h2_gizclaw_rpc_friend_delete(service, (h2_gizclaw_str_t){text, 1u},
                                        1234u, &storage,
                                        &value) == H2_PAL_ERR_FORMAT);
    assert(storage.used == checkpoint);
    mock.has_error = true;
    mock.error_code = 404;
    assert(h2_gizclaw_rpc_friend_delete(service, (h2_gizclaw_str_t){text, 1u},
                                        1234u, &storage,
                                        &value) == H2_PAL_ERR_NOT_FOUND);
  }
  {

    static const uint8_t input[] = {0};
    static const uint8_t response[] = {0x0a, 1, 'e', 0x12, 1, 'x'};
    mock = (test_contact_rpc_t){
        .expected_method = H2_GIZCLAW_RPC_SERVER_FRIEND_INVITE_TOKEN_GET,
        .expected_request = input,
        .expected_request_len = 0u,
        .response = response,
        .response_len = sizeof(response)};
    h2_gizclaw_invite_token_t value;
    assert(h2_gizclaw_req_create_friend_invite_token_get(
               service, 1u, 1234u, &request) == H2_PAL_OK);
    assert(mock.calls == 0);
    assert(h2_gizclaw_resp_parse_friend_invite_token_get(
               request, &storage, &value) == H2_PAL_ERR_INVALID_STATE);

    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK &&
           mock.request_matches);
    assert(h2_gizclaw_resp_parse_friend_invite_token_get(request, &storage,
                                                         &value) == H2_PAL_OK);
    assert(strcmp(value.value, "x") == 0);

    saved_token = value;
    h2_gizclaw_resp_storage_t tiny = {.data = buffer, .capacity = 1u};
    assert(h2_gizclaw_resp_parse_friend_invite_token_get(
               request, &tiny, &value) == H2_PAL_ERR_NO_SPACE &&
           tiny.used == 0u);
    h2_gizclaw_req_release(request);

    assert(h2_gizclaw_rpc_friend_invite_token_get(service, 1234u, &storage,
                                                  &value) == H2_PAL_OK);
    size_t checkpoint = storage.used;
    mock.response = NULL;
    mock.response_len = 0u;
    assert(h2_gizclaw_rpc_friend_invite_token_get(service, 1234u, &storage,
                                                  &value) == H2_PAL_OK);
    assert(value.value == NULL && value.expires_at == NULL &&
           storage.used == checkpoint);
    static const uint8_t bad_response[] = {0x0a, 1, 0xff};
    mock.response = bad_response;
    mock.response_len = sizeof(bad_response);
    assert(h2_gizclaw_rpc_friend_invite_token_get(service, 1234u, &storage,
                                                  &value) == H2_PAL_ERR_FORMAT);
    assert(storage.used == checkpoint);
    mock.has_error = true;
    mock.error_code = 404;
    assert(h2_gizclaw_rpc_friend_invite_token_get(
               service, 1234u, &storage, &value) == H2_PAL_ERR_NOT_FOUND);
  }
  {

    static const uint8_t input[] = {0};
    static const uint8_t response[] = {0x0a, 1, 'e', 0x12, 1, 'x'};
    mock = (test_contact_rpc_t){
        .expected_method = H2_GIZCLAW_RPC_SERVER_FRIEND_INVITE_TOKEN_CREATE,
        .expected_request = input,
        .expected_request_len = 0u,
        .response = response,
        .response_len = sizeof(response)};
    h2_gizclaw_invite_token_t value;
    assert(h2_gizclaw_req_create_friend_invite_token_create(
               service, 1u, 1234u, &request) == H2_PAL_OK);
    assert(mock.calls == 0);
    assert(h2_gizclaw_resp_parse_friend_invite_token_create(
               request, &storage, &value) == H2_PAL_ERR_INVALID_STATE);

    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK &&
           mock.request_matches);
    assert(h2_gizclaw_resp_parse_friend_invite_token_create(
               request, &storage, &value) == H2_PAL_OK);
    assert(strcmp(value.value, "x") == 0);

    saved_token = value;
    h2_gizclaw_resp_storage_t tiny = {.data = buffer, .capacity = 1u};
    assert(h2_gizclaw_resp_parse_friend_invite_token_create(
               request, &tiny, &value) == H2_PAL_ERR_NO_SPACE &&
           tiny.used == 0u);
    h2_gizclaw_req_release(request);

    assert(h2_gizclaw_rpc_friend_invite_token_create(service, 1234u, &storage,
                                                     &value) == H2_PAL_OK);
    size_t checkpoint = storage.used;
    mock.response = NULL;
    mock.response_len = 0u;
    assert(h2_gizclaw_rpc_friend_invite_token_create(
               service, 1234u, &storage, &value) == H2_PAL_ERR_FORMAT);
    assert(value.value == NULL && storage.used == checkpoint);
    static const uint8_t bad_response[] = {0x0a, 1, 0xff};
    mock.response = bad_response;
    mock.response_len = sizeof(bad_response);
    assert(h2_gizclaw_rpc_friend_invite_token_create(
               service, 1234u, &storage, &value) == H2_PAL_ERR_FORMAT);
    assert(storage.used == checkpoint);
    mock.has_error = true;
    mock.error_code = 404;
    assert(h2_gizclaw_rpc_friend_invite_token_create(
               service, 1234u, &storage, &value) == H2_PAL_ERR_NOT_FOUND);
  }
  {

    static const uint8_t input[] = {0};
    static const uint8_t response[] = {0};
    mock = (test_contact_rpc_t){
        .expected_method = H2_GIZCLAW_RPC_SERVER_FRIEND_INVITE_TOKEN_CLEAR,
        .expected_request = input,
        .expected_request_len = 0u,
        .response = response,
        .response_len = 0u};

    assert(h2_gizclaw_req_create_friend_invite_token_clear(
               service, 1u, 1234u, &request) == H2_PAL_OK);
    assert(mock.calls == 0);
    assert(h2_gizclaw_resp_parse_friend_invite_token_clear(request) ==
           H2_PAL_ERR_INVALID_STATE);

    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK &&
           mock.request_matches);
    assert(h2_gizclaw_resp_parse_friend_invite_token_clear(request) ==
           H2_PAL_OK);

    h2_gizclaw_req_release(request);

    assert(h2_gizclaw_rpc_friend_invite_token_clear(service, 1234u) ==
           H2_PAL_OK);
    size_t checkpoint = storage.used;
    static const uint8_t bad_response[] = {0x0a, 1, 0xff};
    mock.response = bad_response;
    mock.response_len = sizeof(bad_response);
    assert(h2_gizclaw_rpc_friend_invite_token_clear(service, 1234u) ==
           H2_PAL_OK);
    assert(storage.used == checkpoint);
    mock.response_len = 1u;
    assert(h2_gizclaw_rpc_friend_invite_token_clear(service, 1234u) ==
           H2_PAL_ERR_FORMAT);
    mock.has_error = true;
    mock.error_code = 404;
    assert(h2_gizclaw_rpc_friend_invite_token_clear(service, 1234u) ==
           H2_PAL_ERR_NOT_FOUND);
  }

  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
  assert(strcmp(saved_token.value, "x") == 0);
}

static void test_friend_group_public_request_paths(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_profile_service(&env);
  h2_gizclaw_async_rpc_test_set_ops(&workspace_test_ops);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  uint8_t buffer[16384];
  h2_gizclaw_resp_storage_t storage = {.data = buffer,
                                       .capacity = sizeof(buffer)};
  h2_gizclaw_req_t *request = NULL;
  test_contact_rpc_t mock;
  workspace_test_use_single(&mock);
  h2_gizclaw_friend_group_t saved = {0};
  {
    char text[] = "x";
    h2_gizclaw_str_t text_arg = {text, 1u};
    static const uint8_t input[] = {0x0a, 1, 'x'};
    static const uint8_t response[] = {0x0a, 6, 0x22, 1, 'D', 0x32, 1, 'x'};
    mock = (test_contact_rpc_t){.expected_method =
                                    H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_GET,
                                .expected_request = input,
                                .expected_request_len = sizeof(input),
                                .response = response,
                                .response_len = sizeof(response)};
    h2_gizclaw_friend_group_t value;
    assert(h2_gizclaw_req_create_friend_group_get(service, 1u, text_arg, 1234u,
                                                  &request) == H2_PAL_OK);
    assert(mock.calls == 0);
    assert(h2_gizclaw_resp_parse_friend_group_get(request, &storage, &value) ==
           H2_PAL_ERR_INVALID_STATE);
    text[0] = 'y';
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK &&
           mock.request_matches);
    assert(h2_gizclaw_resp_parse_friend_group_get(request, &storage, &value) ==
           H2_PAL_OK);
    assert(strcmp(value.name, "x") == 0);
    assert(strcmp(value.display_name, "D") == 0);
    saved = value;
    h2_gizclaw_resp_storage_t tiny = {.data = buffer, .capacity = 1u};
    assert(h2_gizclaw_resp_parse_friend_group_get(request, &tiny, &value) ==
               H2_PAL_ERR_NO_SPACE &&
           tiny.used == 0u);
    assert(value.name == NULL);
    /* Another method's parser must not accept this response. */
    h2_gizclaw_profile_t wrong;
    assert(h2_gizclaw_resp_parse_profile_get(request, &wrong) ==
           H2_PAL_ERR_INVALID_ARG);
    h2_gizclaw_req_release(request);
    text[0] = 'x';
    assert(h2_gizclaw_rpc_friend_group_get(service, text_arg, 1234u, &storage,
                                           &value) == H2_PAL_OK);
    size_t checkpoint = storage.used;

    static const uint8_t truncated[] = {0x0a};
    mock.response = truncated;
    mock.response_len = sizeof(truncated);
    assert(h2_gizclaw_rpc_friend_group_get(service, text_arg, 1234u, &storage,
                                           &value) == H2_PAL_ERR_FORMAT);
    assert(storage.used == checkpoint);
    mock.has_error = true;
    mock.error_code = 404;
    assert(h2_gizclaw_rpc_friend_group_get(service, text_arg, 1234u, &storage,
                                           &value) == H2_PAL_ERR_NOT_FOUND);
  }
  {
    char text[] = "x";
    h2_gizclaw_str_t text_arg = {text, 1u};
    static const uint8_t input[] = {0x0a, 1, 'x', 0x12, 1, 'x', 0x1a, 1, 'x'};
    static const uint8_t response[] = {0x0a, 6, 0x22, 1, 'D', 0x32, 1, 'x'};
    mock = (test_contact_rpc_t){.expected_method =
                                    H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_CREATE,
                                .expected_request = input,
                                .expected_request_len = sizeof(input),
                                .response = response,
                                .response_len = sizeof(response)};
    h2_gizclaw_friend_group_t value;
    assert(h2_gizclaw_req_create_friend_group_create(service, 1u, text_arg,
                                                     text_arg, text_arg, 1234u,
                                                     &request) == H2_PAL_OK);
    assert(mock.calls == 0);
    assert(h2_gizclaw_resp_parse_friend_group_create(
               request, &storage, &value) == H2_PAL_ERR_INVALID_STATE);
    text[0] = 'y';
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK &&
           mock.request_matches);
    assert(h2_gizclaw_resp_parse_friend_group_create(request, &storage,
                                                     &value) == H2_PAL_OK);
    assert(strcmp(value.name, "x") == 0);
    assert(strcmp(value.display_name, "D") == 0);
    saved = value;
    h2_gizclaw_resp_storage_t tiny = {.data = buffer, .capacity = 1u};
    assert(h2_gizclaw_resp_parse_friend_group_create(request, &tiny, &value) ==
               H2_PAL_ERR_NO_SPACE &&
           tiny.used == 0u);
    assert(value.name == NULL);
    /* Another method's parser must not accept this response. */
    h2_gizclaw_profile_t wrong;
    assert(h2_gizclaw_resp_parse_profile_get(request, &wrong) ==
           H2_PAL_ERR_INVALID_ARG);
    h2_gizclaw_req_release(request);
    text[0] = 'x';
    assert(h2_gizclaw_rpc_friend_group_create(service, text_arg, text_arg,
                                              text_arg, 1234u, &storage,
                                              &value) == H2_PAL_OK);
    size_t checkpoint = storage.used;

    static const uint8_t truncated[] = {0x0a};
    mock.response = truncated;
    mock.response_len = sizeof(truncated);
    assert(h2_gizclaw_rpc_friend_group_create(service, text_arg, text_arg,
                                              text_arg, 1234u, &storage,
                                              &value) == H2_PAL_ERR_FORMAT);
    assert(storage.used == checkpoint);
    mock.has_error = true;
    mock.error_code = 404;
    assert(h2_gizclaw_rpc_friend_group_create(service, text_arg, text_arg,
                                              text_arg, 1234u, &storage,
                                              &value) == H2_PAL_ERR_NOT_FOUND);
  }
  {
    char text[] = "x";
    h2_gizclaw_str_t text_arg = {text, 1u};
    static const uint8_t input[] = {0x0a, 1, 'x', 0x12, 1, 'x', 0x1a, 1, 'x'};
    static const uint8_t response[] = {0x0a, 6, 0x22, 1, 'D', 0x32, 1, 'x'};
    mock = (test_contact_rpc_t){.expected_method =
                                    H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_PUT,
                                .expected_request = input,
                                .expected_request_len = sizeof(input),
                                .response = response,
                                .response_len = sizeof(response)};
    h2_gizclaw_friend_group_t value;
    assert(h2_gizclaw_req_create_friend_group_put(service, 1u, text_arg,
                                                  text_arg, text_arg, 1234u,
                                                  &request) == H2_PAL_OK);
    assert(mock.calls == 0);
    assert(h2_gizclaw_resp_parse_friend_group_put(request, &storage, &value) ==
           H2_PAL_ERR_INVALID_STATE);
    text[0] = 'y';
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK &&
           mock.request_matches);
    assert(h2_gizclaw_resp_parse_friend_group_put(request, &storage, &value) ==
           H2_PAL_OK);
    assert(strcmp(value.name, "x") == 0);
    assert(strcmp(value.display_name, "D") == 0);
    saved = value;
    h2_gizclaw_resp_storage_t tiny = {.data = buffer, .capacity = 1u};
    assert(h2_gizclaw_resp_parse_friend_group_put(request, &tiny, &value) ==
               H2_PAL_ERR_NO_SPACE &&
           tiny.used == 0u);
    assert(value.name == NULL);
    /* Another method's parser must not accept this response. */
    h2_gizclaw_profile_t wrong;
    assert(h2_gizclaw_resp_parse_profile_get(request, &wrong) ==
           H2_PAL_ERR_INVALID_ARG);
    h2_gizclaw_req_release(request);
    text[0] = 'x';
    assert(h2_gizclaw_rpc_friend_group_put(service, text_arg, text_arg,
                                           text_arg, 1234u, &storage,
                                           &value) == H2_PAL_OK);
    size_t checkpoint = storage.used;

    static const uint8_t truncated[] = {0x0a};
    mock.response = truncated;
    mock.response_len = sizeof(truncated);
    assert(h2_gizclaw_rpc_friend_group_put(service, text_arg, text_arg,
                                           text_arg, 1234u, &storage,
                                           &value) == H2_PAL_ERR_FORMAT);
    assert(storage.used == checkpoint);
    mock.has_error = true;
    mock.error_code = 404;
    assert(h2_gizclaw_rpc_friend_group_put(service, text_arg, text_arg,
                                           text_arg, 1234u, &storage,
                                           &value) == H2_PAL_ERR_NOT_FOUND);
  }
  {
    char text[] = "x";
    h2_gizclaw_str_t text_arg = {text, 1u};
    static const uint8_t input[] = {0x0a, 1, 'x'};
    static const uint8_t response[] = {0x0a, 6, 0x22, 1, 'D', 0x32, 1, 'x'};
    mock = (test_contact_rpc_t){.expected_method =
                                    H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_DELETE,
                                .expected_request = input,
                                .expected_request_len = sizeof(input),
                                .response = response,
                                .response_len = sizeof(response)};
    h2_gizclaw_friend_group_t value;
    assert(h2_gizclaw_req_create_friend_group_delete(
               service, 1u, text_arg, 1234u, &request) == H2_PAL_OK);
    assert(mock.calls == 0);
    assert(h2_gizclaw_resp_parse_friend_group_delete(
               request, &storage, &value) == H2_PAL_ERR_INVALID_STATE);
    text[0] = 'y';
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK &&
           mock.request_matches);
    assert(h2_gizclaw_resp_parse_friend_group_delete(request, &storage,
                                                     &value) == H2_PAL_OK);
    assert(strcmp(value.name, "x") == 0);
    assert(strcmp(value.display_name, "D") == 0);
    saved = value;
    h2_gizclaw_resp_storage_t tiny = {.data = buffer, .capacity = 1u};
    assert(h2_gizclaw_resp_parse_friend_group_delete(request, &tiny, &value) ==
               H2_PAL_ERR_NO_SPACE &&
           tiny.used == 0u);
    assert(value.name == NULL);
    /* Another method's parser must not accept this response. */
    h2_gizclaw_profile_t wrong;
    assert(h2_gizclaw_resp_parse_profile_get(request, &wrong) ==
           H2_PAL_ERR_INVALID_ARG);
    h2_gizclaw_req_release(request);
    text[0] = 'x';
    assert(h2_gizclaw_rpc_friend_group_delete(service, text_arg, 1234u,
                                              &storage, &value) == H2_PAL_OK);
    size_t checkpoint = storage.used;

    static const uint8_t truncated[] = {0x0a};
    mock.response = truncated;
    mock.response_len = sizeof(truncated);
    assert(h2_gizclaw_rpc_friend_group_delete(service, text_arg, 1234u,
                                              &storage,
                                              &value) == H2_PAL_ERR_FORMAT);
    assert(storage.used == checkpoint);
    mock.has_error = true;
    mock.error_code = 404;
    assert(h2_gizclaw_rpc_friend_group_delete(service, text_arg, 1234u,
                                              &storage,
                                              &value) == H2_PAL_ERR_NOT_FOUND);
  }
  {
    char text[] = "x";
    h2_gizclaw_str_t text_arg = {text, 1u};
    static const uint8_t input[] = {0x0a, 1, 'x', 0x12, 1, 'x'};
    static const uint8_t response[] = {0x0a, 6, 0x22, 1, 'D', 0x32, 1, 'x'};
    mock = (test_contact_rpc_t){.expected_method =
                                    H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_JOIN,
                                .expected_request = input,
                                .expected_request_len = sizeof(input),
                                .response = response,
                                .response_len = sizeof(response)};
    h2_gizclaw_friend_group_t value;
    assert(h2_gizclaw_req_create_friend_group_join(
               service, 1u, text_arg, text_arg, 1234u, &request) == H2_PAL_OK);
    assert(mock.calls == 0);
    assert(h2_gizclaw_resp_parse_friend_group_join(request, &storage, &value) ==
           H2_PAL_ERR_INVALID_STATE);
    text[0] = 'y';
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK &&
           mock.request_matches);
    assert(h2_gizclaw_resp_parse_friend_group_join(request, &storage, &value) ==
           H2_PAL_OK);
    assert(strcmp(value.name, "x") == 0);
    assert(strcmp(value.display_name, "D") == 0);
    saved = value;
    h2_gizclaw_resp_storage_t tiny = {.data = buffer, .capacity = 1u};
    assert(h2_gizclaw_resp_parse_friend_group_join(request, &tiny, &value) ==
               H2_PAL_ERR_NO_SPACE &&
           tiny.used == 0u);
    assert(value.name == NULL);
    /* Another method's parser must not accept this response. */
    h2_gizclaw_profile_t wrong;
    assert(h2_gizclaw_resp_parse_profile_get(request, &wrong) ==
           H2_PAL_ERR_INVALID_ARG);
    h2_gizclaw_req_release(request);
    text[0] = 'x';
    assert(h2_gizclaw_rpc_friend_group_join(service, text_arg, text_arg, 1234u,
                                            &storage, &value) == H2_PAL_OK);
    size_t checkpoint = storage.used;

    static const uint8_t truncated[] = {0x0a};
    mock.response = truncated;
    mock.response_len = sizeof(truncated);
    assert(h2_gizclaw_rpc_friend_group_join(service, text_arg, text_arg, 1234u,
                                            &storage,
                                            &value) == H2_PAL_ERR_FORMAT);
    assert(storage.used == checkpoint);
    mock.has_error = true;
    mock.error_code = 404;
    assert(h2_gizclaw_rpc_friend_group_join(service, text_arg, text_arg, 1234u,
                                            &storage,
                                            &value) == H2_PAL_ERR_NOT_FOUND);
  }
  {
    char text[] = "x";
    h2_gizclaw_str_t text_arg = {text, 1u};
    static const uint8_t input[] = {0x0a, 1, 'x'};
    static const uint8_t response[] = {0x0a, 1, 'e', 0x12, 1, 'x'};
    mock = (test_contact_rpc_t){
        .expected_method = H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_INVITE_TOKEN_GET,
        .expected_request = input,
        .expected_request_len = sizeof(input),
        .response = response,
        .response_len = sizeof(response)};
    h2_gizclaw_invite_token_t value;
    assert(h2_gizclaw_req_create_friend_group_invite_token_get(
               service, 1u, text_arg, 1234u, &request) == H2_PAL_OK);
    assert(mock.calls == 0);
    assert(h2_gizclaw_resp_parse_friend_group_invite_token_get(
               request, &storage, &value) == H2_PAL_ERR_INVALID_STATE);
    text[0] = 'y';
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK &&
           mock.request_matches);
    assert(h2_gizclaw_resp_parse_friend_group_invite_token_get(
               request, &storage, &value) == H2_PAL_OK);
    assert(strcmp(value.value, "x") == 0);

    h2_gizclaw_resp_storage_t tiny = {.data = buffer, .capacity = 1u};
    assert(h2_gizclaw_resp_parse_friend_group_invite_token_get(
               request, &tiny, &value) == H2_PAL_ERR_NO_SPACE &&
           tiny.used == 0u);
    assert(value.value == NULL);
    /* Another method's parser must not accept this response. */
    h2_gizclaw_profile_t wrong;
    assert(h2_gizclaw_resp_parse_profile_get(request, &wrong) ==
           H2_PAL_ERR_INVALID_ARG);
    h2_gizclaw_req_release(request);
    text[0] = 'x';
    assert(h2_gizclaw_rpc_friend_group_invite_token_get(
               service, text_arg, 1234u, &storage, &value) == H2_PAL_OK);
    size_t checkpoint = storage.used;
    mock.response = NULL;
    mock.response_len = 0u;
    assert(h2_gizclaw_rpc_friend_group_invite_token_get(
               service, text_arg, 1234u, &storage, &value) == H2_PAL_OK);
    assert(value.value == NULL && storage.used == checkpoint);
    static const uint8_t truncated[] = {0x0a};
    mock.response = truncated;
    mock.response_len = sizeof(truncated);
    assert(h2_gizclaw_rpc_friend_group_invite_token_get(
               service, text_arg, 1234u, &storage, &value) ==
           H2_PAL_ERR_FORMAT);
    assert(storage.used == checkpoint);
    mock.has_error = true;
    mock.error_code = 404;
    assert(h2_gizclaw_rpc_friend_group_invite_token_get(
               service, text_arg, 1234u, &storage, &value) ==
           H2_PAL_ERR_NOT_FOUND);
  }
  {
    char text[] = "x";
    h2_gizclaw_str_t text_arg = {text, 1u};
    static const uint8_t input[] = {0x0a, 1, 'x'};
    static const uint8_t response[] = {0x0a, 1, 'e', 0x12, 1, 'x'};
    mock = (test_contact_rpc_t){
        .expected_method =
            H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_INVITE_TOKEN_CREATE,
        .expected_request = input,
        .expected_request_len = sizeof(input),
        .response = response,
        .response_len = sizeof(response)};
    h2_gizclaw_invite_token_t value;
    assert(h2_gizclaw_req_create_friend_group_invite_token_create(
               service, 1u, text_arg, 1234u, &request) == H2_PAL_OK);
    assert(mock.calls == 0);
    assert(h2_gizclaw_resp_parse_friend_group_invite_token_create(
               request, &storage, &value) == H2_PAL_ERR_INVALID_STATE);
    text[0] = 'y';
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK &&
           mock.request_matches);
    assert(h2_gizclaw_resp_parse_friend_group_invite_token_create(
               request, &storage, &value) == H2_PAL_OK);
    assert(strcmp(value.value, "x") == 0);

    h2_gizclaw_resp_storage_t tiny = {.data = buffer, .capacity = 1u};
    assert(h2_gizclaw_resp_parse_friend_group_invite_token_create(
               request, &tiny, &value) == H2_PAL_ERR_NO_SPACE &&
           tiny.used == 0u);
    assert(value.value == NULL);
    /* Another method's parser must not accept this response. */
    h2_gizclaw_profile_t wrong;
    assert(h2_gizclaw_resp_parse_profile_get(request, &wrong) ==
           H2_PAL_ERR_INVALID_ARG);
    h2_gizclaw_req_release(request);
    text[0] = 'x';
    assert(h2_gizclaw_rpc_friend_group_invite_token_create(
               service, text_arg, 1234u, &storage, &value) == H2_PAL_OK);
    size_t checkpoint = storage.used;
    mock.response = NULL;
    mock.response_len = 0u;
    assert(h2_gizclaw_rpc_friend_group_invite_token_create(
               service, text_arg, 1234u, &storage, &value) ==
           H2_PAL_ERR_FORMAT);
    assert(value.value == NULL && storage.used == checkpoint);
    static const uint8_t truncated[] = {0x0a};
    mock.response = truncated;
    mock.response_len = sizeof(truncated);
    assert(h2_gizclaw_rpc_friend_group_invite_token_create(
               service, text_arg, 1234u, &storage, &value) ==
           H2_PAL_ERR_FORMAT);
    assert(storage.used == checkpoint);
    mock.has_error = true;
    mock.error_code = 404;
    assert(h2_gizclaw_rpc_friend_group_invite_token_create(
               service, text_arg, 1234u, &storage, &value) ==
           H2_PAL_ERR_NOT_FOUND);
  }
  {
    char text[] = "x";
    h2_gizclaw_str_t text_arg = {text, 1u};
    static const uint8_t input[] = {0x0a, 1, 'x'};
    static const uint8_t response[] = {0};
    mock = (test_contact_rpc_t){
        .expected_method =
            H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_INVITE_TOKEN_CLEAR,
        .expected_request = input,
        .expected_request_len = sizeof(input),
        .response = response,
        .response_len = 0u};

    assert(h2_gizclaw_req_create_friend_group_invite_token_clear(
               service, 1u, text_arg, 1234u, &request) == H2_PAL_OK);
    assert(mock.calls == 0);
    assert(h2_gizclaw_resp_parse_friend_group_invite_token_clear(request) ==
           H2_PAL_ERR_INVALID_STATE);
    text[0] = 'y';
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK &&
           mock.request_matches);
    assert(h2_gizclaw_resp_parse_friend_group_invite_token_clear(request) ==
           H2_PAL_OK);

    /* Another method's parser must not accept this response. */
    h2_gizclaw_profile_t wrong;
    assert(h2_gizclaw_resp_parse_profile_get(request, &wrong) ==
           H2_PAL_ERR_INVALID_ARG);
    h2_gizclaw_req_release(request);
    text[0] = 'x';
    assert(h2_gizclaw_rpc_friend_group_invite_token_clear(service, text_arg,
                                                          1234u) == H2_PAL_OK);
    size_t checkpoint = storage.used;
    static const uint8_t truncated[] = {0x0a};
    mock.response = truncated;
    mock.response_len = sizeof(truncated);
    assert(h2_gizclaw_rpc_friend_group_invite_token_clear(
               service, text_arg, 1234u) == H2_PAL_ERR_FORMAT);
    assert(storage.used == checkpoint);
    mock.has_error = true;
    mock.error_code = 404;
    assert(h2_gizclaw_rpc_friend_group_invite_token_clear(
               service, text_arg, 1234u) == H2_PAL_ERR_NOT_FOUND);
  }
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
  assert(strcmp(saved.name, "x") == 0 && strcmp(saved.display_name, "D") == 0);
}

static bool test_encode_friend_group_message(pb_ostream_t *stream,
                                             const pb_field_t *field,
                                             void *const *arg) {
  const gizclaw_rpc_v1_FriendGroupMessageObject *message = *arg;
  return message != NULL && pb_encode_tag_for_field(stream, field) &&
         pb_encode_submessage(
             stream, gizclaw_rpc_v1_FriendGroupMessageObject_fields, message);
}

static void test_friend_group_message_fixture(
    gizclaw_rpc_v1_FriendGroupMessageObject *message) {
  *message = (gizclaw_rpc_v1_FriendGroupMessageObject)
      gizclaw_rpc_v1_FriendGroupMessageObject_init_zero;
  (void)snprintf(message->created_at, sizeof(message->created_at), "%s",
                 "2026-08-05T00:00:00Z");
  (void)snprintf(message->friend_group_name, sizeof(message->friend_group_name),
                 "%s", "group-1");
  (void)snprintf(message->sender_peer_public_key,
                 sizeof(message->sender_peer_public_key), "%s", "peer-1");
  message->has_sender_peer_public_key = true;
  (void)snprintf(message->name, sizeof(message->name), "%s", "history-1");
  (void)snprintf(message->actor_name, sizeof(message->actor_name), "%s",
                 "Nova");
  (void)snprintf(message->text, sizeof(message->text), "%s", "hello");
  message->type =
      gizclaw_rpc_v1_PeerRunHistoryEntryType_PEER_RUN_HISTORY_ENTRY_TYPE_AGENT;
  message->audio_available = true;
}

static bool test_encode_friend_group_message_list_response(uint8_t *buffer,
                                                           size_t capacity,
                                                           bool invalid_utf8,
                                                           size_t *out_len) {
  gizclaw_rpc_v1_FriendGroupMessageObject message;
  test_friend_group_message_fixture(&message);
  if (invalid_utf8) {
    message.text[0] = (char)0xc0;
    message.text[1] = (char)0x80;
    message.text[2] = '\0';
  }
  gizclaw_rpc_v1_FriendGroupMessageListResponse response =
      gizclaw_rpc_v1_FriendGroupMessageListResponse_init_zero;
  response.items.funcs.encode = test_encode_friend_group_message;
  response.items.arg = &message;
  pb_ostream_t stream = pb_ostream_from_buffer(buffer, capacity);
  if (!pb_encode(&stream, gizclaw_rpc_v1_FriendGroupMessageListResponse_fields,
                 &response)) {
    return false;
  }
  *out_len = stream.bytes_written;
  return true;
}

static bool test_encode_friend_group_message_get_response(uint8_t *buffer,
                                                          size_t capacity,
                                                          size_t *out_len) {
  gizclaw_rpc_v1_FriendGroupMessageGetResponse response =
      gizclaw_rpc_v1_FriendGroupMessageGetResponse_init_zero;
  response.has_value = true;
  test_friend_group_message_fixture(&response.value);
  pb_ostream_t stream = pb_ostream_from_buffer(buffer, capacity);
  if (!pb_encode(&stream, gizclaw_rpc_v1_FriendGroupMessageGetResponse_fields,
                 &response)) {
    return false;
  }
  *out_len = stream.bytes_written;
  return true;
}

static int test_friend_group_message_projection_rpcs(void) {
  int fails = 0;
  test_env_t env;
  h2_gizclaw_service_t *service = create_profile_service(&env);
  h2_gizclaw_async_rpc_test_set_ops(&workspace_test_ops);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  uint8_t storage_buffer[4096];
  h2_gizclaw_resp_storage_t storage = {.data = storage_buffer,
                                       .capacity = sizeof(storage_buffer)};
  const h2_gizclaw_str_t group_name = {.data = "group-1", .len = 7u};
  const h2_gizclaw_str_t history_name = {.data = "history-1", .len = 9u};
  const uint8_t list_request[] = {
      0x12, 0x07, 'g', 'r', 'o', 'u', 'p', '-', '1', 0x18, 0x08,
  };
  uint8_t response[512];
  size_t response_len = 0u;
  fails +=
      workspace_expect(test_encode_friend_group_message_list_response(
                           response, sizeof(response), false, &response_len),
                       "friend group message list response fixture encodes");
  test_contact_rpc_t mock = {
      .expected_method = H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_MESSAGES_LIST,
      .expected_request = list_request,
      .expected_request_len = sizeof(list_request),
      .response = response,
      .response_len = response_len,
  };
  workspace_test_use_single(&mock);
  h2_gizclaw_friend_group_message_page_t page = {0};
  fails += workspace_expect(
      h2_gizclaw_rpc_friend_group_message_list(service, group_name,
                                               (h2_gizclaw_str_t){0}, 8u, 1234u,
                                               &storage, &page) == H2_PAL_OK,
      "friend group message list accepts a history projection");
  fails += workspace_expect(
      mock.calls == 1 && mock.request_matches && page.count == 1u &&
          strcmp(page.items[0].friend_group_name, "group-1") == 0 &&
          strcmp(page.items[0].history_id, "history-1") == 0 &&
          strcmp(page.items[0].sender_peer_public_key, "peer-1") == 0 &&
          strcmp(page.items[0].name, "Nova") == 0 &&
          strcmp(page.items[0].text, "hello") == 0 &&
          page.items[0].type == H2_GIZCLAW_FRIEND_GROUP_MESSAGE_TYPE_AGENT &&
          page.items[0].audio_available,
      "friend group message list maps group and Workspace History identity");
  /* Caller storage owns this snapshot. */

  response_len = 0u;
  fails +=
      workspace_expect(test_encode_friend_group_message_list_response(
                           response, sizeof(response), true, &response_len),
                       "invalid UTF-8 message response fixture encodes");
  mock = (test_contact_rpc_t){
      .expected_method = H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_MESSAGES_LIST,
      .expected_request = list_request,
      .expected_request_len = sizeof(list_request),
      .response = response,
      .response_len = response_len,
  };
  fails +=
      workspace_expect(h2_gizclaw_rpc_friend_group_message_list(
                           service, group_name, (h2_gizclaw_str_t){0}, 8u,
                           1234u, &storage, &page) == H2_PAL_ERR_FORMAT,
                       "friend group message list rejects invalid UTF-8 text");
  fails +=
      workspace_expect(page.items == NULL && page.count == 0u,
                       "invalid message projection releases partial ownership");

  const uint8_t get_request[] = {
      0x0a, 0x07, 'g', 'r', 'o', 'u', 'p', '-', '1', 0x12,
      0x09, 'h',  'i', 's', 't', 'o', 'r', 'y', '-', '1',
  };
  response_len = 0u;
  fails +=
      workspace_expect(test_encode_friend_group_message_get_response(
                           response, sizeof(response), &response_len),
                       "friend group message get response fixture encodes");
  mock = (test_contact_rpc_t){
      .expected_method = H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_MESSAGES_GET,
      .expected_request = get_request,
      .expected_request_len = sizeof(get_request),
      .response = response,
      .response_len = response_len,
  };
  h2_gizclaw_friend_group_message_t message = {0};
  fails +=
      workspace_expect(h2_gizclaw_rpc_friend_group_message_get(
                           service, group_name, history_name, 1234u, &storage,
                           &message) == H2_PAL_OK,
                       "friend group message get accepts a history projection");
  fails += workspace_expect(
      mock.calls == 1 && mock.request_matches &&
          strcmp(message.friend_group_name, "group-1") == 0 &&
          strcmp(message.history_id, "history-1") == 0 &&
          message.audio_available,
      "friend group message get preserves the requested history identity");

  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
  assert(strcmp(message.history_id, "history-1") == 0);
  return fails;
}

static void test_group_member_and_message_request_paths(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_profile_service(&env);
  h2_gizclaw_async_rpc_test_set_ops(&workspace_test_ops);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  uint8_t arena_buffer[16384];
  h2_gizclaw_resp_storage_t storage = {.data = arena_buffer,
                                       .capacity = sizeof(arena_buffer)};
  h2_gizclaw_req_t *request = NULL;
  test_contact_rpc_t mock;
  workspace_test_use_single(&mock);
  {
    char text[] = "x";
    h2_gizclaw_str_t text_arg = {text, 1u};
    static const uint8_t input[] = {0x0a, 1, 'x', 0x12, 1, 'x', 0x18, 1};
    static const uint8_t response[] = {0x12, 8, 0x12, 1,    'x',
                                       0x1a, 1, 'm',  0x28, 3};
    size_t response_len = sizeof(response);
    mock = (test_contact_rpc_t){
        .expected_method = H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_MEMBERS_LIST,
        .expected_request = input,
        .expected_request_len = sizeof(input),
        .response = response,
        .response_len = response_len};
    h2_gizclaw_friend_group_member_page_t value;
    assert(h2_gizclaw_req_create_friend_group_member_list(
               service, 1u, text_arg, text_arg, 1u, 1234u, &request) ==
           H2_PAL_OK);
    assert(mock.calls == 0);
    assert(h2_gizclaw_resp_parse_friend_group_member_list(
               request, &storage, &value) == H2_PAL_ERR_INVALID_STATE);
    text[0] = 'y';
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK &&
           mock.request_matches);
    assert(h2_gizclaw_resp_parse_friend_group_member_list(request, &storage,
                                                          &value) == H2_PAL_OK);
    assert(value.count == 1u);
    assert(strcmp(value.items[0].id, "m") == 0);
    assert(value.items[0].role == H2_GIZCLAW_FRIEND_GROUP_ROLE_MEMBER);
    h2_gizclaw_resp_storage_t tiny = {.data = arena_buffer, .capacity = 1u};
    assert(h2_gizclaw_resp_parse_friend_group_member_list(
               request, &tiny, &value) == H2_PAL_ERR_NO_SPACE &&
           tiny.used == 0u);
    h2_gizclaw_req_release(request);
    text[0] = 'x';
    assert(h2_gizclaw_rpc_friend_group_member_list(service, text_arg, text_arg,
                                                   1u, 1234u, &storage,
                                                   &value) == H2_PAL_OK);
    const size_t checkpoint = storage.used;
    uint8_t too_many[1024];
    assert(response_len * 2u <= sizeof(too_many));
    memcpy(too_many, response, response_len);
    memcpy(too_many + response_len, response, response_len);
    mock.response = too_many;
    mock.response_len = response_len * 2u;
    assert(h2_gizclaw_rpc_friend_group_member_list(
               service, text_arg, text_arg, 1u, 1234u, &storage, &value) ==
           H2_PAL_ERR_FORMAT);
    assert(value.count == 0u && value.items == NULL &&
           storage.used == checkpoint);
    static const uint8_t bad[] = {0x0a};
    mock.response = bad;
    mock.response_len = sizeof(bad);
    assert(h2_gizclaw_rpc_friend_group_member_list(
               service, text_arg, text_arg, 1u, 1234u, &storage, &value) ==
           H2_PAL_ERR_FORMAT);
    assert(storage.used == checkpoint);
    mock.has_error = true;
    mock.error_code = 403;
    assert(h2_gizclaw_rpc_friend_group_member_list(
               service, text_arg, text_arg, 1u, 1234u, &storage, &value) ==
           H2_GIZCLAW_ERR_REMOTE);
  }
  {
    char text[] = "x";
    h2_gizclaw_str_t text_arg = {text, 1u};
    static const uint8_t input[] = {0x0a, 1, 'x', 0x12, 1, 'x', 0x18, 2};
    static const uint8_t response[] = {0x0a, 8, 0x12, 1,    'x',
                                       0x1a, 1, 'm',  0x28, 3};
    size_t response_len = sizeof(response);
    mock = (test_contact_rpc_t){
        .expected_method = H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_MEMBERS_PUT,
        .expected_request = input,
        .expected_request_len = sizeof(input),
        .response = response,
        .response_len = response_len};
    h2_gizclaw_friend_group_member_t value;
    assert(h2_gizclaw_req_create_friend_group_member_put(
               service, 1u, text_arg, text_arg,
               H2_GIZCLAW_FRIEND_GROUP_ROLE_MEMBER, 1234u,
               &request) == H2_PAL_OK);
    assert(mock.calls == 0);
    assert(h2_gizclaw_resp_parse_friend_group_member_put(
               request, &storage, &value) == H2_PAL_ERR_INVALID_STATE);
    text[0] = 'y';
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK &&
           mock.request_matches);
    assert(h2_gizclaw_resp_parse_friend_group_member_put(request, &storage,
                                                         &value) == H2_PAL_OK);

    assert(strcmp(value.id, "m") == 0);
    assert(value.role == H2_GIZCLAW_FRIEND_GROUP_ROLE_MEMBER);
    h2_gizclaw_resp_storage_t tiny = {.data = arena_buffer, .capacity = 1u};
    assert(h2_gizclaw_resp_parse_friend_group_member_put(
               request, &tiny, &value) == H2_PAL_ERR_NO_SPACE &&
           tiny.used == 0u);
    h2_gizclaw_req_release(request);
    text[0] = 'x';
    assert(h2_gizclaw_rpc_friend_group_member_put(
               service, text_arg, text_arg, H2_GIZCLAW_FRIEND_GROUP_ROLE_MEMBER,
               1234u, &storage, &value) == H2_PAL_OK);
    const size_t checkpoint = storage.used;

    static const uint8_t bad[] = {0x0a};
    mock.response = bad;
    mock.response_len = sizeof(bad);
    assert(h2_gizclaw_rpc_friend_group_member_put(
               service, text_arg, text_arg, H2_GIZCLAW_FRIEND_GROUP_ROLE_MEMBER,
               1234u, &storage, &value) == H2_PAL_ERR_FORMAT);
    assert(storage.used == checkpoint);
    mock.has_error = true;
    mock.error_code = 403;
    assert(h2_gizclaw_rpc_friend_group_member_put(
               service, text_arg, text_arg, H2_GIZCLAW_FRIEND_GROUP_ROLE_MEMBER,
               1234u, &storage, &value) == H2_GIZCLAW_ERR_REMOTE);
  }
  {
    char text[] = "x";
    h2_gizclaw_str_t text_arg = {text, 1u};
    static const uint8_t input[] = {0x0a, 1, 'x', 0x12, 1, 'x'};
    static const uint8_t response[] = {0x0a, 8, 0x12, 1,    'x',
                                       0x1a, 1, 'm',  0x28, 3};
    size_t response_len = sizeof(response);
    mock = (test_contact_rpc_t){
        .expected_method = H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_MEMBERS_DELETE,
        .expected_request = input,
        .expected_request_len = sizeof(input),
        .response = response,
        .response_len = response_len};
    h2_gizclaw_friend_group_member_t value;
    assert(h2_gizclaw_req_create_friend_group_member_delete(
               service, 1u, text_arg, text_arg, 1234u, &request) == H2_PAL_OK);
    assert(mock.calls == 0);
    assert(h2_gizclaw_resp_parse_friend_group_member_delete(
               request, &storage, &value) == H2_PAL_ERR_INVALID_STATE);
    text[0] = 'y';
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK &&
           mock.request_matches);
    assert(h2_gizclaw_resp_parse_friend_group_member_delete(
               request, &storage, &value) == H2_PAL_OK);

    assert(strcmp(value.id, "m") == 0);
    assert(value.role == H2_GIZCLAW_FRIEND_GROUP_ROLE_MEMBER);
    h2_gizclaw_resp_storage_t tiny = {.data = arena_buffer, .capacity = 1u};
    assert(h2_gizclaw_resp_parse_friend_group_member_delete(
               request, &tiny, &value) == H2_PAL_ERR_NO_SPACE &&
           tiny.used == 0u);
    h2_gizclaw_req_release(request);
    text[0] = 'x';
    assert(h2_gizclaw_rpc_friend_group_member_delete(service, text_arg,
                                                     text_arg, 1234u, &storage,
                                                     &value) == H2_PAL_OK);
    const size_t checkpoint = storage.used;

    static const uint8_t bad[] = {0x0a};
    mock.response = bad;
    mock.response_len = sizeof(bad);
    assert(h2_gizclaw_rpc_friend_group_member_delete(
               service, text_arg, text_arg, 1234u, &storage, &value) ==
           H2_PAL_ERR_FORMAT);
    assert(storage.used == checkpoint);
    mock.has_error = true;
    mock.error_code = 403;
    assert(h2_gizclaw_rpc_friend_group_member_delete(
               service, text_arg, text_arg, 1234u, &storage, &value) ==
           H2_GIZCLAW_ERR_REMOTE);
  }
  {
    char text[] = "x";
    h2_gizclaw_str_t text_arg = {text, 1u};
    static const uint8_t input[] = {0x0a, 1, 'x', 0x12, 1, 'x', 0x18, 1};
    uint8_t response[512];
    size_t response_len;
    assert(test_encode_friend_group_message_list_response(
        response, sizeof(response), false, &response_len));
    mock = (test_contact_rpc_t){
        .expected_method = H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_MESSAGES_LIST,
        .expected_request = input,
        .expected_request_len = sizeof(input),
        .response = response,
        .response_len = response_len};
    h2_gizclaw_friend_group_message_page_t value;
    assert(h2_gizclaw_req_create_friend_group_message_list(
               service, 1u, text_arg, text_arg, 1u, 1234u, &request) ==
           H2_PAL_OK);
    assert(mock.calls == 0);
    assert(h2_gizclaw_resp_parse_friend_group_message_list(
               request, &storage, &value) == H2_PAL_ERR_INVALID_STATE);
    text[0] = 'y';
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK &&
           mock.request_matches);
    assert(h2_gizclaw_resp_parse_friend_group_message_list(
               request, &storage, &value) == H2_PAL_OK);
    assert(value.count == 1u);
    assert(strcmp(value.items[0].history_id, "history-1") == 0);
    assert(strcmp(value.items[0].text, "hello") == 0 &&
           value.items[0].audio_available);
    h2_gizclaw_resp_storage_t tiny = {.data = arena_buffer, .capacity = 1u};
    assert(h2_gizclaw_resp_parse_friend_group_message_list(
               request, &tiny, &value) == H2_PAL_ERR_NO_SPACE &&
           tiny.used == 0u);
    h2_gizclaw_req_release(request);
    text[0] = 'x';
    assert(h2_gizclaw_rpc_friend_group_message_list(service, text_arg, text_arg,
                                                    1u, 1234u, &storage,
                                                    &value) == H2_PAL_OK);
    const size_t checkpoint = storage.used;
    uint8_t too_many[1024];
    assert(response_len * 2u <= sizeof(too_many));
    memcpy(too_many, response, response_len);
    memcpy(too_many + response_len, response, response_len);
    mock.response = too_many;
    mock.response_len = response_len * 2u;
    assert(h2_gizclaw_rpc_friend_group_message_list(
               service, text_arg, text_arg, 1u, 1234u, &storage, &value) ==
           H2_PAL_ERR_FORMAT);
    assert(value.count == 0u && value.items == NULL &&
           storage.used == checkpoint);
    static const uint8_t bad[] = {0x0a};
    mock.response = bad;
    mock.response_len = sizeof(bad);
    assert(h2_gizclaw_rpc_friend_group_message_list(
               service, text_arg, text_arg, 1u, 1234u, &storage, &value) ==
           H2_PAL_ERR_FORMAT);
    assert(storage.used == checkpoint);
    mock.has_error = true;
    mock.error_code = 403;
    assert(h2_gizclaw_rpc_friend_group_message_list(
               service, text_arg, text_arg, 1u, 1234u, &storage, &value) ==
           H2_GIZCLAW_ERR_REMOTE);
  }
  {
    char text[] = "x";
    h2_gizclaw_str_t text_arg = {text, 1u};
    static const uint8_t input[] = {0x0a, 1, 'x', 0x12, 1, 'x'};
    uint8_t response[512];
    size_t response_len;
    assert(test_encode_friend_group_message_get_response(
        response, sizeof(response), &response_len));
    mock = (test_contact_rpc_t){
        .expected_method = H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_MESSAGES_GET,
        .expected_request = input,
        .expected_request_len = sizeof(input),
        .response = response,
        .response_len = response_len};
    h2_gizclaw_friend_group_message_t value;
    assert(h2_gizclaw_req_create_friend_group_message_get(
               service, 1u, text_arg, text_arg, 1234u, &request) == H2_PAL_OK);
    assert(mock.calls == 0);
    assert(h2_gizclaw_resp_parse_friend_group_message_get(
               request, &storage, &value) == H2_PAL_ERR_INVALID_STATE);
    text[0] = 'y';
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK &&
           mock.request_matches);
    assert(h2_gizclaw_resp_parse_friend_group_message_get(request, &storage,
                                                          &value) == H2_PAL_OK);

    assert(strcmp(value.history_id, "history-1") == 0);
    assert(strcmp(value.text, "hello") == 0 && value.audio_available);
    h2_gizclaw_resp_storage_t tiny = {.data = arena_buffer, .capacity = 1u};
    assert(h2_gizclaw_resp_parse_friend_group_message_get(
               request, &tiny, &value) == H2_PAL_ERR_NO_SPACE &&
           tiny.used == 0u);
    h2_gizclaw_req_release(request);
    text[0] = 'x';
    assert(h2_gizclaw_rpc_friend_group_message_get(service, text_arg, text_arg,
                                                   1234u, &storage,
                                                   &value) == H2_PAL_OK);
    const size_t checkpoint = storage.used;

    static const uint8_t bad[] = {0x0a};
    mock.response = bad;
    mock.response_len = sizeof(bad);
    assert(h2_gizclaw_rpc_friend_group_message_get(service, text_arg, text_arg,
                                                   1234u, &storage, &value) ==
           H2_PAL_ERR_FORMAT);
    assert(storage.used == checkpoint);
    mock.has_error = true;
    mock.error_code = 403;
    assert(h2_gizclaw_rpc_friend_group_message_get(service, text_arg, text_arg,
                                                   1234u, &storage, &value) ==
           H2_GIZCLAW_ERR_REMOTE);
  }
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

static void test_social_full_page_arena_growth(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_profile_service(&env);
  h2_gizclaw_async_rpc_test_set_ops(&workspace_test_ops);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  test_contact_rpc_t mock;
  workspace_test_use_single(&mock);
  uint8_t storage_buffer[16384], payload[65536];
  h2_gizclaw_resp_storage_t storage = {.data = storage_buffer,
                                       .capacity = sizeof(storage_buffer)};
  {
    static const uint8_t item[] = {0x12, 3, 0x12, 1, 'x'};
    size_t item_len = sizeof(item);
    assert(item_len * 64u <= sizeof(payload));
    for (size_t i = 0; i < 64u; ++i)
      memcpy(payload + i * item_len, item, item_len);
    static const uint8_t input[] = {0x10, 64};
    mock = (test_contact_rpc_t){.expected_method =
                                    H2_GIZCLAW_RPC_SERVER_FRIEND_LIST,
                                .expected_request = input,
                                .expected_request_len = sizeof(input),
                                .response = payload,
                                .response_len = item_len * 64u};
    storage.used = 0u;
    h2_gizclaw_friend_page_t page;
    assert(h2_gizclaw_rpc_friend_list(service, (h2_gizclaw_str_t){0}, 64u,
                                      1234u, &storage, &page) == H2_PAL_OK);
    assert(mock.request_matches && page.count == 64u);
    assert(strcmp(page.items[63].id, "x") == 0);
  }
  {
    static const uint8_t item[] = {0x12, 6, 0x12, 1, 'x', 0x1a, 1, 'm'};
    size_t item_len = sizeof(item);
    assert(item_len * 64u <= sizeof(payload));
    for (size_t i = 0; i < 64u; ++i)
      memcpy(payload + i * item_len, item, item_len);
    static const uint8_t input[] = {0x12, 1, 'x', 0x18, 64};
    mock = (test_contact_rpc_t){
        .expected_method = H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_MEMBERS_LIST,
        .expected_request = input,
        .expected_request_len = sizeof(input),
        .response = payload,
        .response_len = item_len * 64u};
    storage.used = 0u;
    h2_gizclaw_friend_group_member_page_t page;
    assert(h2_gizclaw_rpc_friend_group_member_list(
               service, (h2_gizclaw_str_t){"x", 1u}, (h2_gizclaw_str_t){0}, 64u,
               1234u, &storage, &page) == H2_PAL_OK);
    assert(mock.request_matches && page.count == 64u);
    assert(strcmp(page.items[63].id, "m") == 0);
  }
  {
    uint8_t item[512];
    size_t item_len;
    assert(test_encode_friend_group_message_list_response(item, sizeof(item),
                                                          false, &item_len));
    assert(item_len * 64u <= sizeof(payload));
    for (size_t i = 0; i < 64u; ++i)
      memcpy(payload + i * item_len, item, item_len);
    static const uint8_t input[] = {0x12, 1, 'x', 0x18, 64};
    mock = (test_contact_rpc_t){
        .expected_method = H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_MESSAGES_LIST,
        .expected_request = input,
        .expected_request_len = sizeof(input),
        .response = payload,
        .response_len = item_len * 64u};
    storage.used = 0u;
    h2_gizclaw_friend_group_message_page_t page;
    assert(h2_gizclaw_rpc_friend_group_message_list(
               service, (h2_gizclaw_str_t){"x", 1u}, (h2_gizclaw_str_t){0}, 64u,
               1234u, &storage, &page) == H2_PAL_OK);
    assert(mock.request_matches && page.count == 64u);
    assert(strcmp(page.items[63].history_id, "history-1") == 0);
  }
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

static void test_pet_public_request_paths(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_profile_service(&env);
  h2_gizclaw_async_rpc_test_set_ops(&workspace_test_ops);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  test_contact_rpc_t mock;
  workspace_test_use_single(&mock);
  uint8_t buffer[16384];
  h2_gizclaw_resp_storage_t storage = {.data = buffer,
                                       .capacity = sizeof(buffer)};
  h2_gizclaw_req_t *request = NULL;
  const char *saved_name = NULL;
  {
    char text[] = "x";
    h2_gizclaw_str_t text_arg = {text, 1u};
    static const uint8_t input[] = {0x0a, 3, 0x0a, 1, 'x'},
                         response[] = {0x0a, 3, 0x0a, 1, 'x'};
    mock =
        (test_contact_rpc_t){.expected_method = H2_GIZCLAW_RPC_SERVER_PET_GET,
                             .expected_request = input,
                             .expected_request_len = sizeof(input),
                             .response = response,
                             .response_len = sizeof(response)};
    h2_gizclaw_pet_t value;
    assert(h2_gizclaw_req_create_pet_get(service, 1u, text_arg, 1234u,
                                         &request) == H2_PAL_OK &&
           mock.calls == 0);
    assert(h2_gizclaw_resp_parse_pet_get(request, &storage, &value) ==
           H2_PAL_ERR_INVALID_STATE);
    text[0] = 'y';
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK &&
           mock.request_matches);
    assert(h2_gizclaw_resp_parse_pet_get(request, &storage, &value) ==
           H2_PAL_OK);

    assert(strcmp(value.name, "x") == 0);
    saved_name = value.name;

    h2_gizclaw_resp_storage_t tiny = {.data = buffer, .capacity = 1u};
    assert(h2_gizclaw_resp_parse_pet_get(request, &tiny, &value) ==
               H2_PAL_ERR_NO_SPACE &&
           tiny.used == 0u);
    h2_gizclaw_profile_t wrong;
    assert(h2_gizclaw_resp_parse_profile_get(request, &wrong) ==
           H2_PAL_ERR_INVALID_ARG);
    h2_gizclaw_req_release(request);
    text[0] = 'x';
    assert(h2_gizclaw_rpc_pet_get(service, text_arg, 1234u, &storage, &value) ==
           H2_PAL_OK);
    const size_t checkpoint = storage.used;
    uint8_t bad[sizeof(response)];
    memcpy(bad, response, sizeof(bad));
    bad[4] = 0xff;
    mock.response = bad;
    assert(h2_gizclaw_rpc_pet_get(service, text_arg, 1234u, &storage, &value) ==
               H2_PAL_ERR_FORMAT &&
           storage.used == checkpoint);
    mock.response_len = 1u;
    assert(h2_gizclaw_rpc_pet_get(service, text_arg, 1234u, &storage, &value) ==
               H2_PAL_ERR_FORMAT &&
           storage.used == checkpoint);
    mock.has_error = true;
    mock.error_code = 404;
    assert(h2_gizclaw_rpc_pet_get(service, text_arg, 1234u, &storage, &value) ==
           H2_PAL_ERR_NOT_FOUND);
  }
  {
    char text[] = "x";
    h2_gizclaw_str_t text_arg = {text, 1u};
    static const uint8_t input[] = {0x0a, 3, 0x0a, 1, 'x'},
                         response[] = {0x0a, 3, 0x0a, 1, 'x'};
    mock = (test_contact_rpc_t){.expected_method =
                                    H2_GIZCLAW_RPC_SERVER_PET_DELETE,
                                .expected_request = input,
                                .expected_request_len = sizeof(input),
                                .response = response,
                                .response_len = sizeof(response)};
    h2_gizclaw_pet_t value;
    assert(h2_gizclaw_req_create_pet_delete(service, 1u, text_arg, 1234u,
                                            &request) == H2_PAL_OK &&
           mock.calls == 0);
    assert(h2_gizclaw_resp_parse_pet_delete(request, &storage, &value) ==
           H2_PAL_ERR_INVALID_STATE);
    text[0] = 'y';
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK &&
           mock.request_matches);
    assert(h2_gizclaw_resp_parse_pet_delete(request, &storage, &value) ==
           H2_PAL_OK);

    assert(strcmp(value.name, "x") == 0);
    saved_name = value.name;

    h2_gizclaw_resp_storage_t tiny = {.data = buffer, .capacity = 1u};
    assert(h2_gizclaw_resp_parse_pet_delete(request, &tiny, &value) ==
               H2_PAL_ERR_NO_SPACE &&
           tiny.used == 0u);
    h2_gizclaw_profile_t wrong;
    assert(h2_gizclaw_resp_parse_profile_get(request, &wrong) ==
           H2_PAL_ERR_INVALID_ARG);
    h2_gizclaw_req_release(request);
    text[0] = 'x';
    assert(h2_gizclaw_rpc_pet_delete(service, text_arg, 1234u, &storage,
                                     &value) == H2_PAL_OK);
    const size_t checkpoint = storage.used;
    uint8_t bad[sizeof(response)];
    memcpy(bad, response, sizeof(bad));
    bad[4] = 0xff;
    mock.response = bad;
    assert(h2_gizclaw_rpc_pet_delete(service, text_arg, 1234u, &storage,
                                     &value) == H2_PAL_ERR_FORMAT &&
           storage.used == checkpoint);
    mock.response_len = 1u;
    assert(h2_gizclaw_rpc_pet_delete(service, text_arg, 1234u, &storage,
                                     &value) == H2_PAL_ERR_FORMAT &&
           storage.used == checkpoint);
    mock.has_error = true;
    mock.error_code = 404;
    assert(h2_gizclaw_rpc_pet_delete(service, text_arg, 1234u, &storage,
                                     &value) == H2_PAL_ERR_NOT_FOUND);
  }
  {
    char text[] = "x";
    h2_gizclaw_str_t text_arg = {text, 1u};
    h2_gizclaw_pet_adopt_options_t options = {.name = text_arg,
                                              .display_name = text_arg};
    static const uint8_t input[] = {0x0a, 6, 0x0a, 1, 'x', 0x12, 1, 'x'},
                         response[] = {0x0a, 5, 0x0a, 3, 0x0a, 1, 'x'};
    mock = (test_contact_rpc_t){.expected_method = H2_GIZCLAW_RPC_RUNTIME_ADOPT,
                                .expected_request = input,
                                .expected_request_len = sizeof(input),
                                .response = response,
                                .response_len = sizeof(response)};
    h2_gizclaw_pet_t value;
    assert(h2_gizclaw_req_create_pet_adopt(service, 1u, &options, 1234u,
                                           &request) == H2_PAL_OK &&
           mock.calls == 0);
    assert(h2_gizclaw_resp_parse_pet_adopt(request, &storage, &value) ==
           H2_PAL_ERR_INVALID_STATE);
    text[0] = 'y';
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK &&
           mock.request_matches);
    assert(h2_gizclaw_resp_parse_pet_adopt(request, &storage, &value) ==
           H2_PAL_OK);

    assert(strcmp(value.name, "x") == 0);
    saved_name = value.name;

    h2_gizclaw_resp_storage_t tiny = {.data = buffer, .capacity = 1u};
    assert(h2_gizclaw_resp_parse_pet_adopt(request, &tiny, &value) ==
               H2_PAL_ERR_NO_SPACE &&
           tiny.used == 0u);
    h2_gizclaw_profile_t wrong;
    assert(h2_gizclaw_resp_parse_profile_get(request, &wrong) ==
           H2_PAL_ERR_INVALID_ARG);
    h2_gizclaw_req_release(request);
    text[0] = 'x';
    assert(h2_gizclaw_rpc_pet_adopt(service, &options, 1234u, &storage,
                                    &value) == H2_PAL_OK);
    const size_t checkpoint = storage.used;
    uint8_t bad[sizeof(response)];
    memcpy(bad, response, sizeof(bad));
    bad[6] = 0xff;
    mock.response = bad;
    assert(h2_gizclaw_rpc_pet_adopt(service, &options, 1234u, &storage,
                                    &value) == H2_PAL_ERR_FORMAT &&
           storage.used == checkpoint);
    mock.response_len = 1u;
    assert(h2_gizclaw_rpc_pet_adopt(service, &options, 1234u, &storage,
                                    &value) == H2_PAL_ERR_FORMAT &&
           storage.used == checkpoint);
    mock.has_error = true;
    mock.error_code = 404;
    assert(h2_gizclaw_rpc_pet_adopt(service, &options, 1234u, &storage,
                                    &value) == H2_PAL_ERR_NOT_FOUND);
  }
  {
    char text[] = "x";
    h2_gizclaw_str_t text_arg = {text, 1u};
    h2_gizclaw_pet_drive_options_t options = {.pet_name = text_arg,
                                              .idempotency_key = text_arg,
                                              .behavior =
                                                  H2_GIZCLAW_PET_BEHAVIOR_FEED};
    static const uint8_t input[] = {0x0a, 8,   0x08, 1, 0x1a,
                                    1,    'x', 0x22, 1, 'x'},
                         response[] = {0x0a, 5, 0x1a, 3, 0x0a, 1, 'x'};
    mock =
        (test_contact_rpc_t){.expected_method = H2_GIZCLAW_RPC_SERVER_PET_DRIVE,
                             .expected_request = input,
                             .expected_request_len = sizeof(input),
                             .response = response,
                             .response_len = sizeof(response)};
    h2_gizclaw_pet_t value;
    assert(h2_gizclaw_req_create_pet_drive(service, 1u, &options, 1234u,
                                           &request) == H2_PAL_OK &&
           mock.calls == 0);
    assert(h2_gizclaw_resp_parse_pet_drive(request, &storage, &value) ==
           H2_PAL_ERR_INVALID_STATE);
    text[0] = 'y';
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK &&
           mock.request_matches);
    assert(h2_gizclaw_resp_parse_pet_drive(request, &storage, &value) ==
           H2_PAL_OK);

    assert(strcmp(value.name, "x") == 0);
    saved_name = value.name;

    h2_gizclaw_resp_storage_t tiny = {.data = buffer, .capacity = 1u};
    assert(h2_gizclaw_resp_parse_pet_drive(request, &tiny, &value) ==
               H2_PAL_ERR_NO_SPACE &&
           tiny.used == 0u);
    h2_gizclaw_profile_t wrong;
    assert(h2_gizclaw_resp_parse_profile_get(request, &wrong) ==
           H2_PAL_ERR_INVALID_ARG);
    h2_gizclaw_req_release(request);
    text[0] = 'x';
    assert(h2_gizclaw_rpc_pet_drive(service, &options, 1234u, &storage,
                                    &value) == H2_PAL_OK);
    const size_t checkpoint = storage.used;
    uint8_t bad[sizeof(response)];
    memcpy(bad, response, sizeof(bad));
    bad[6] = 0xff;
    mock.response = bad;
    assert(h2_gizclaw_rpc_pet_drive(service, &options, 1234u, &storage,
                                    &value) == H2_PAL_ERR_FORMAT &&
           storage.used == checkpoint);
    mock.response_len = 1u;
    assert(h2_gizclaw_rpc_pet_drive(service, &options, 1234u, &storage,
                                    &value) == H2_PAL_ERR_FORMAT &&
           storage.used == checkpoint);
    mock.has_error = true;
    mock.error_code = 404;
    assert(h2_gizclaw_rpc_pet_drive(service, &options, 1234u, &storage,
                                    &value) == H2_PAL_ERR_NOT_FOUND);
  }
  {
    char text[] = "x";
    h2_gizclaw_str_t text_arg = {text, 1u};
    static const uint8_t input[] = {0x0a, 5, 0x0a, 1, 'x', 0x10, 1},
                         response[] = {0x0a, 5, 0x12, 3, 0x0a, 1, 'x'};
    mock =
        (test_contact_rpc_t){.expected_method = H2_GIZCLAW_RPC_SERVER_PET_LIST,
                             .expected_request = input,
                             .expected_request_len = sizeof(input),
                             .response = response,
                             .response_len = sizeof(response)};
    h2_gizclaw_pet_page_t value;
    assert(h2_gizclaw_req_create_pet_list(service, 1u, text_arg, 1u, 1234u,
                                          &request) == H2_PAL_OK &&
           mock.calls == 0);
    assert(h2_gizclaw_resp_parse_pet_list(request, &storage, &value) ==
           H2_PAL_ERR_INVALID_STATE);
    text[0] = 'y';
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK &&
           mock.request_matches);
    assert(h2_gizclaw_resp_parse_pet_list(request, &storage, &value) ==
           H2_PAL_OK);
    assert(value.count == 1u);
    assert(strcmp(value.items[0].name, "x") == 0);
    saved_name = value.items[0].name;

    h2_gizclaw_resp_storage_t tiny = {.data = buffer, .capacity = 1u};
    assert(h2_gizclaw_resp_parse_pet_list(request, &tiny, &value) ==
               H2_PAL_ERR_NO_SPACE &&
           tiny.used == 0u);
    h2_gizclaw_profile_t wrong;
    assert(h2_gizclaw_resp_parse_profile_get(request, &wrong) ==
           H2_PAL_ERR_INVALID_ARG);
    h2_gizclaw_req_release(request);
    text[0] = 'x';
    assert(h2_gizclaw_rpc_pet_list(service, text_arg, 1u, 1234u, &storage,
                                   &value) == H2_PAL_OK);
    const size_t checkpoint = storage.used;
    uint8_t bad[sizeof(response)];
    memcpy(bad, response, sizeof(bad));
    bad[6] = 0xff;
    mock.response = bad;
    assert(h2_gizclaw_rpc_pet_list(service, text_arg, 1u, 1234u, &storage,
                                   &value) == H2_PAL_ERR_FORMAT &&
           storage.used == checkpoint);
    mock.response_len = 1u;
    assert(h2_gizclaw_rpc_pet_list(service, text_arg, 1u, 1234u, &storage,
                                   &value) == H2_PAL_ERR_FORMAT &&
           storage.used == checkpoint);
    mock.has_error = true;
    mock.error_code = 404;
    assert(h2_gizclaw_rpc_pet_list(service, text_arg, 1u, 1234u, &storage,
                                   &value) == H2_PAL_ERR_NOT_FOUND);
  }
  {
    char text[] = "x";
    h2_gizclaw_str_t text_arg = {text, 1u};
    static const uint8_t input[] = {0x0a, 3, 0x0a, 1, 'x'},
                         response[] = {0x0a, 11, 0x0a, 1,    'x', 0x2a, 6,
                                       0x0a, 1,  'i',  0x12, 1,   'c'};
    mock = (test_contact_rpc_t){.expected_method =
                                    H2_GIZCLAW_RPC_SERVER_PET_ACTIONS_GET,
                                .expected_request = input,
                                .expected_request_len = sizeof(input),
                                .response = response,
                                .response_len = sizeof(response)};
    h2_gizclaw_pet_actions_t value;
    assert(h2_gizclaw_req_create_pet_action_get(service, 1u, text_arg, 1234u,
                                                &request) == H2_PAL_OK &&
           mock.calls == 0);
    assert(h2_gizclaw_resp_parse_pet_action_get(request, &storage, &value) ==
           H2_PAL_ERR_INVALID_STATE);
    text[0] = 'y';
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_req_wait(request, 2000u) == H2_PAL_OK &&
           mock.request_matches);
    assert(h2_gizclaw_resp_parse_pet_action_get(request, &storage, &value) ==
           H2_PAL_OK);

    assert(strcmp(value.pet_name, "x") == 0);
    saved_name = value.pet_name;
    assert(value.clip_name_count == 1u &&
           strcmp(value.clip_names[0].id, "i") == 0 &&
           strcmp(value.clip_names[0].pixa_clip_name, "c") == 0);
    h2_gizclaw_resp_storage_t tiny = {.data = buffer, .capacity = 1u};
    assert(h2_gizclaw_resp_parse_pet_action_get(request, &tiny, &value) ==
               H2_PAL_ERR_NO_SPACE &&
           tiny.used == 0u);
    h2_gizclaw_profile_t wrong;
    assert(h2_gizclaw_resp_parse_profile_get(request, &wrong) ==
           H2_PAL_ERR_INVALID_ARG);
    h2_gizclaw_req_release(request);
    text[0] = 'x';
    assert(h2_gizclaw_rpc_pet_action_get(service, text_arg, 1234u, &storage,
                                         &value) == H2_PAL_OK);
    const size_t checkpoint = storage.used;
    uint8_t bad[sizeof(response)];
    memcpy(bad, response, sizeof(bad));
    bad[4] = 0xff;
    mock.response = bad;
    assert(h2_gizclaw_rpc_pet_action_get(service, text_arg, 1234u, &storage,
                                         &value) == H2_PAL_ERR_FORMAT &&
           storage.used == checkpoint);
    mock.response_len = 1u;
    assert(h2_gizclaw_rpc_pet_action_get(service, text_arg, 1234u, &storage,
                                         &value) == H2_PAL_ERR_FORMAT &&
           storage.used == checkpoint);
    mock.has_error = true;
    mock.error_code = 404;
    assert(h2_gizclaw_rpc_pet_action_get(service, text_arg, 1234u, &storage,
                                         &value) == H2_PAL_ERR_NOT_FOUND);
  }
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
  assert(strcmp(saved_name, "x") == 0);
}

static bool test_encode_pet_delete_response(uint8_t *buffer, size_t capacity,
                                            size_t *out_len) {
  gizclaw_rpc_v1_ServerPetDeleteResponse response =
      gizclaw_rpc_v1_ServerPetDeleteResponse_init_zero;
  test_contact_text_t text[] = {
      {.data = "pet-1", .len = 5u},
      {.data = "petdef-1", .len = 8u},
      {.data = "E2E Pet", .len = 7u},
  };
  response.has_value = true;
  response.value.name.funcs.encode = test_encode_contact_text;
  response.value.name.arg = &text[0];
  response.value.pet_def_name.funcs.encode = test_encode_contact_text;
  response.value.pet_def_name.arg = &text[1];
  response.value.display_name.funcs.encode = test_encode_contact_text;
  response.value.display_name.arg = &text[2];
  pb_ostream_t stream = pb_ostream_from_buffer(buffer, capacity);
  if (!pb_encode(&stream, gizclaw_rpc_v1_ServerPetDeleteResponse_fields,
                 &response)) {
    return false;
  }
  *out_len = stream.bytes_written;
  return true;
}

static int test_pet_delete_rpc_regression(void) {
  int fails = 0;
  test_env_t env;
  h2_gizclaw_service_t *service = create_profile_service(&env);
  h2_gizclaw_async_rpc_test_set_ops(&workspace_test_ops);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  uint8_t buffer[4096];
  h2_gizclaw_resp_storage_t storage = {.data = buffer,
                                       .capacity = sizeof(buffer)};
  const char invalid_utf8[] = {(char)0xc0, (char)0x80};
  test_contact_rpc_t mock;
  workspace_test_use_single(&mock);
  const uint8_t pet_request[] = {
      0x0a, 0x07, 0x0a, 0x05, 'p', 'e', 't', '-', '1',
  };
  uint8_t pet_response[128];
  size_t pet_response_len = 0u;
  fails += workspace_expect(
      test_encode_pet_delete_response(pet_response, sizeof(pet_response),
                                      &pet_response_len),
      "pet delete response fixture encodes");
  mock = (test_contact_rpc_t){
      .expected_method = H2_GIZCLAW_RPC_SERVER_PET_DELETE,
      .expected_request = pet_request,
      .expected_request_len = sizeof(pet_request),
      .response = pet_response,
      .response_len = pet_response_len,
  };
  h2_gizclaw_pet_t pet = {0};
  fails += workspace_expect(
      h2_gizclaw_rpc_pet_delete(service,
                                (h2_gizclaw_str_t){.data = "pet-1", .len = 5u},
                                1234u, &storage, &pet) == H2_PAL_OK,
      "pet delete accepts the typed mocked RPC response");
  fails +=
      workspace_expect(mock.calls == 1 && mock.request_matches &&
                           strcmp(pet.name, "pet-1") == 0 &&
                           strcmp(pet.pet_def_name, "petdef-1") == 0,
                       "pet delete encodes method 69 and owns its snapshot");

  mock = (test_contact_rpc_t){
      .expected_method = H2_GIZCLAW_RPC_SERVER_PET_DELETE,
      .expected_request = pet_request,
      .expected_request_len = sizeof(pet_request),
      .has_error = true,
      .error_code = H2_GIZCLAW_RPC_ERROR_NOT_FOUND,
  };
  pet.name = (char *)0x1;
  fails += workspace_expect(
      h2_gizclaw_rpc_pet_delete(
          service, (h2_gizclaw_str_t){.data = "pet-1", .len = 5u}, 1234u,
          &storage, &pet) == H2_PAL_ERR_NOT_FOUND &&
          pet.name == NULL,
      "pet delete maps RPC errors and clears output");

  mock = (test_contact_rpc_t){
      .expected_method = H2_GIZCLAW_RPC_SERVER_PET_DELETE,
      .expected_request = pet_request,
      .expected_request_len = sizeof(pet_request),
      .response = (const uint8_t *)"\x0a\x01\xff",
      .response_len = 3u,
  };
  pet.name = (char *)0x1;
  fails += workspace_expect(
      h2_gizclaw_rpc_pet_delete(service,
                                (h2_gizclaw_str_t){.data = "pet-1", .len = 5u},
                                1234u, &storage, &pet) == H2_PAL_ERR_FORMAT &&
          pet.name == NULL,
      "pet delete rejects malformed response and clears output");

  fails += workspace_expect(
      h2_gizclaw_rpc_pet_delete(
          service, (h2_gizclaw_str_t){.data = invalid_utf8, .len = 2u}, 1234u,
          &storage, &pet) == H2_PAL_ERR_INVALID_ARG &&
          pet.name == NULL,
      "pet delete rejects invalid UTF-8 before RPC dispatch");
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
  return fails;
}

typedef struct download_test_wire {
  h2_gizclaw_rpc_method_t method;
  h2_gizclaw_rpc_bytes_t input;
  h2_gizclaw_rpc_stream_event_t events[8];
  size_t event_count;
  size_t next_event;
  h2_gizclaw_rpc_stream_fn receive;
  void *receive_user;
  int start_busy;
  int finish_busy;
  int start_count;
  int finish_count;
  int cancel_count;
  int destroy_count;
  int sink_result;
  uint8_t bytes[16];
  size_t bytes_written;
  bool hold;
  bool result_done;
  atomic_bool event_received;
  pthread_t network_thread;
} download_test_wire_t;

static download_test_wire_t *s_download_wire;

static int download_test_start(h2_gizclaw_client_t *client,
                               h2_gizclaw_rpc_method_t method,
                               h2_gizclaw_rpc_bytes_t payload,
                               uint32_t timeout_ms,
                               h2_gizclaw_rpc_stream_fn receive, void *user,
                               h2_gizclaw_rpc_request_t **out_request) {
  (void)client;
  download_test_wire_t *wire = s_download_wire;
  wire->network_thread = pthread_self();
  assert(method == wire->method && payload.len == wire->input.len &&
         memcmp(payload.data, wire->input.data, payload.len) == 0);
  assert(timeout_ms > 0u && timeout_ms <= 1234u);
  *out_request = NULL;
  ++wire->start_count;
  if (wire->start_count <= wire->start_busy)
    return H2_PAL_ERR_WOULD_BLOCK;
  wire->receive = receive;
  wire->receive_user = user;
  *out_request = (h2_gizclaw_rpc_request_t *)wire;
  return H2_PAL_OK;
}

static int download_test_finish(h2_gizclaw_rpc_request_t *request) {
  download_test_wire_t *wire = (download_test_wire_t *)request;
  assert(pthread_equal(pthread_self(), wire->network_thread));
  return ++wire->finish_count <= wire->finish_busy ? H2_PAL_ERR_WOULD_BLOCK
                                                   : H2_PAL_OK;
}

static int download_test_result(h2_gizclaw_rpc_request_t *request,
                                h2_gizclaw_rpc_response_t *out) {
  download_test_wire_t *wire = (download_test_wire_t *)request;
  assert(pthread_equal(pthread_self(), wire->network_thread));
  memset(out, 0, sizeof(*out));
  if (wire->next_event < wire->event_count) {
    int rc =
        wire->receive(wire->receive_user, &wire->events[wire->next_event++]);
    atomic_store(&wire->event_received, true);
    return rc == H2_PAL_OK ? H2_PAL_ERR_WOULD_BLOCK : rc;
  }
  if (wire->hold)
    return H2_PAL_ERR_WOULD_BLOCK;
  wire->result_done = true;
  return H2_PAL_OK;
}

static void download_test_cancel(h2_gizclaw_rpc_request_t *request) {
  assert(request != NULL);
  ++((download_test_wire_t *)request)->cancel_count;
}

static void download_test_destroy(h2_gizclaw_rpc_request_t *request) {
  assert(request != NULL);
  ++((download_test_wire_t *)request)->destroy_count;
}

static int download_test_sink(void *user, const uint8_t *data, size_t len) {
  download_test_wire_t *wire = user;
  assert(!pthread_equal(pthread_self(), wire->network_thread));
  if (wire->sink_result == H2_PAL_ERR_WOULD_BLOCK) {
    wire->sink_result = H2_PAL_OK;
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  if (wire->sink_result != H2_PAL_OK)
    return wire->sink_result;
  assert(len <= sizeof(wire->bytes) - wire->bytes_written);
  memcpy(wire->bytes + wire->bytes_written, data, len);
  wire->bytes_written += len;
  return H2_PAL_OK;
}

static h2_pal_result_t download_test_output(void *user, const uint8_t *data,
                                            size_t len, size_t *out_written) {
  h2_pal_result_t rc = (h2_pal_result_t)download_test_sink(user, data, len);
  *out_written = rc == H2_PAL_OK ? len : 0u;
  return rc;
}

static const h2_gizclaw_async_rpc_ops_t download_test_ops = {
    .start_stream = download_test_start,
    .finish_write = download_test_finish,
    .result = download_test_result,
    .cancel = download_test_cancel,
    .destroy = download_test_destroy,
};

static void test_pixa_download_request_paths(void) {
  for (unsigned mode = 0u; mode < 12u; ++mode) {
    test_env_t env;
    h2_gizclaw_service_t *service = create_profile_service(&env);
    service->client_config.time = h2_desktop_platform_time_api();
    h2_gizclaw_async_rpc_test_set_ops(&download_test_ops);
    assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
    static const uint8_t input[] = {0x0a, 3, 0x0a, 1, 'x'};
    uint8_t metadata[] = {0x0a, 11,   0x0a, 1,   'x',  0x12, 1,
                          'd',  0x1a, 1,    'p', 0x20, 3};
    static const uint8_t data[] = {'a', 'b', 'c'};
    download_test_wire_t wire = {
        .method = H2_GIZCLAW_RPC_SERVER_PET_PIXA_DOWNLOAD,
        .input = {input, sizeof(input)},
        .events = {{.kind = H2_GIZCLAW_RPC_STREAM_RESPONSE,
                    .result_payload = {metadata, sizeof(metadata)}},
                   {.kind = H2_GIZCLAW_RPC_STREAM_DATA,
                    .data = {data, sizeof(data)}},
                   {.kind = H2_GIZCLAW_RPC_STREAM_EOS}},
        .event_count = 3u,
        .start_busy = 2,
        .finish_busy = 2,
    };
    s_download_wire = &wire;
    h2_pal_result_t expected = H2_PAL_ERR_FORMAT;
    switch (mode) {
    case 0:
      expected = H2_PAL_OK;
      break;
    case 1:
      wire.event_count = 2u;
      break; /* Missing EOS. */
    case 2:
      wire.events[0] = wire.events[1];
      break;
    case 3:
      wire.events[3] = wire.events[2];
      wire.event_count = 4u;
      break;
    case 4:
      wire.events[0].has_error = true;
      wire.events[0].error_code = 404;
      expected = H2_PAL_ERR_NOT_FOUND;
      break;
    case 5:
      wire.sink_result = H2_PAL_ERR_IO;
      expected = H2_PAL_ERR_IO;
      break;
    case 6:
      wire.sink_result = H2_PAL_ERR_WOULD_BLOCK;
      expected = H2_PAL_OK;
      break;
    case 7:
      metadata[4] = 'y';
      break;
    case 8:
      metadata[12] = 2u;
      break;
    case 9:
      metadata[12] = 4u;
      break;
    case 10:
      wire.event_count = 1u;
      wire.hold = true;
      expected = H2_PAL_ERR_CLOSED;
      break;
    case 11:
      wire.sink_result = H2_PAL_EXIT;
      expected = H2_PAL_ERR_IO;
      break;
    }
    char name[] = "x";
    h2_gizclaw_req_t *request = NULL;
    assert(h2_gizclaw_req_create_pet_pixa_download(
               service, 1u, (h2_gizclaw_str_t){name, 1u}, 1234u, &request) ==
           H2_PAL_OK);
    assert(wire.start_count == 0 && wire.bytes_written == 0u);
    uint8_t buffer[1024];
    h2_gizclaw_resp_storage_t storage = {.data = buffer,
                                         .capacity = sizeof(buffer)};
    h2_gizclaw_pet_pixa_info_t info;
    assert(h2_gizclaw_resp_parse_pet_pixa_download(request, &storage, &info) ==
           H2_PAL_ERR_INVALID_STATE);
    name[0] = 'y';
    assert(h2_gizclaw_req_do(request, &wire, NULL, download_test_output,
                             NULL) == H2_PAL_OK);
    if (mode == 10u) {
      for (unsigned spins = 0u;
           spins < 2000u && !atomic_load(&wire.event_received); ++spins)
        (void)h2_pal_time_sleep_ms(service->client_config.time, 1u);
      assert(atomic_load(&wire.event_received));
      assert(h2_gizclaw_req_cancel(request) == H2_PAL_OK);
    }
    h2_pal_result_t actual = h2_gizclaw_req_wait_dispatch_internal(request);
    if (actual != expected)
      fprintf(stderr,
              "pixa mode=%u result=%d expected=%d starts=%d events=%zu\n", mode,
              actual, expected, wire.start_count, wire.next_event);
    assert(actual == expected);
    assert(wire.start_count == 3 && wire.finish_count == 3 &&
           wire.cancel_count == (wire.result_done ? 0 : 1) &&
           wire.destroy_count == 1);
    assert(h2_gizclaw_resp_parse_pet_pixa_download(request, &storage, &info) ==
           expected);
    if (mode == 0u || mode == 6u) {
      assert(info.size_bytes == 3u && info.received_bytes == 3u &&
             strcmp(info.pet_name, "x") == 0 &&
             memcmp(wire.bytes, data, sizeof(data)) == 0);
      h2_gizclaw_resp_storage_t tiny = {.data = buffer, .capacity = 1u};
      h2_gizclaw_pet_pixa_info_t empty;
      assert(h2_gizclaw_resp_parse_pet_pixa_download(request, &tiny, &empty) ==
             H2_PAL_ERR_NO_SPACE);
      assert(empty.pet_name == NULL && tiny.used == 0u);
    } else {
      assert(info.pet_name == NULL && storage.used == 0u);
    }
    h2_gizclaw_req_release(request);
    if (mode == 0u) {
      wire.next_event = 0u;
      wire.bytes_written = 0u;
      assert(h2_gizclaw_rpc_pet_pixa_download(
                 service, (h2_gizclaw_str_t){"x", 1u}, download_test_sink,
                 &wire, 1234u, &storage, &info) == H2_PAL_OK);
    }
    assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
    assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
    if (mode == 0u)
      assert(strcmp(info.pet_name, "x") == 0);
  }
}

static bool test_encode_message_audio_response(uint8_t *buffer, size_t capacity,
                                               const char *friend_group_name,
                                               const char *history_name,
                                               size_t audio_len,
                                               size_t *out_len) {
  gizclaw_rpc_v1_FriendGroupMessageAudioDownloadResponse response =
      gizclaw_rpc_v1_FriendGroupMessageAudioDownloadResponse_init_zero;
  (void)snprintf(response.friend_group_name, sizeof(response.friend_group_name),
                 "%s", friend_group_name);
  (void)snprintf(response.history_name, sizeof(response.history_name), "%s",
                 history_name);
  (void)snprintf(response.mime_type, sizeof(response.mime_type), "%s",
                 "audio/ogg");
  response.size_bytes = (int64_t)audio_len;
  pb_ostream_t stream = pb_ostream_from_buffer(buffer, capacity);
  if (!pb_encode(&stream,
                 gizclaw_rpc_v1_FriendGroupMessageAudioDownloadResponse_fields,
                 &response)) {
    return false;
  }
  *out_len = stream.bytes_written;
  return true;
}

static void test_group_audio_download_request_paths(void) {
  for (unsigned mode = 0u; mode < 8u; ++mode) {
    test_env_t env;
    h2_gizclaw_service_t *service = create_profile_service(&env);
    service->client_config.time = h2_desktop_platform_time_api();
    h2_gizclaw_async_rpc_test_set_ops(&download_test_ops);
    assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
    const uint8_t input[] = {0x0a, 7,   'g', 'r', 'o', 'u', 'p', '-', 'a', 0x12,
                             9,    'h', 'i', 's', 't', 'o', 'r', 'y', '-', '1'};
    const uint8_t audio[] = {0x4f, 0x67, 0x67, 0x53};
    uint8_t metadata[256];
    size_t metadata_len;
    assert(test_encode_message_audio_response(
        metadata, sizeof(metadata), mode == 1u ? "other-group" : "group-a",
        mode == 2u ? "other-history" : "history-1",
        mode == 6u ? 0u : sizeof(audio), &metadata_len));
    if (mode == 7u)
      metadata_len = 1u;
    download_test_wire_t wire = {
        .method = H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_MESSAGES_AUDIO_DOWNLOAD,
        .input = {input, sizeof(input)},
        .events = {{.kind = H2_GIZCLAW_RPC_STREAM_RESPONSE,
                    .result_payload = {metadata, metadata_len}},
                   {.kind = H2_GIZCLAW_RPC_STREAM_DATA, .data = {audio, 2u}},
                   {.kind = H2_GIZCLAW_RPC_STREAM_DATA,
                    .data = {audio + 2u, 2u}},
                   {.kind = H2_GIZCLAW_RPC_STREAM_EOS}},
        .event_count = 4u};
    if (mode == 3u)
      wire.event_count = 3u;
    if (mode == 4u) {
      wire.events[4] = wire.events[3];
      wire.event_count = 5u;
    }
    if (mode == 5u)
      wire.events[0] = wire.events[1];
    s_download_wire = &wire;
    char group[] = "group-a", history[] = "history-1";
    h2_gizclaw_req_t *request = NULL;
    assert(h2_gizclaw_req_create_friend_group_message_audio_download(
               service, 1u, (h2_gizclaw_str_t){group, 7u},
               (h2_gizclaw_str_t){history, 9u}, 1234u, &request) == H2_PAL_OK &&
           wire.start_count == 0);
    group[0] = 'X';
    history[0] = 'Y';
    uint8_t buffer[1024];
    h2_gizclaw_resp_storage_t storage = {.data = buffer,
                                         .capacity = sizeof(buffer)};
    h2_gizclaw_friend_group_message_audio_info_t info;
    assert(h2_gizclaw_resp_parse_friend_group_message_audio_download(
               request, &storage, &info) == H2_PAL_ERR_INVALID_STATE);
    assert(h2_gizclaw_req_do(request, &wire, NULL, download_test_output,
                             NULL) == H2_PAL_OK);
    h2_pal_result_t expected = mode == 0u ? H2_PAL_OK : H2_PAL_ERR_FORMAT;
    assert(h2_gizclaw_req_wait_dispatch_internal(request) == expected);
    assert(h2_gizclaw_resp_parse_friend_group_message_audio_download(
               request, &storage, &info) == expected);
    if (mode == 0u) {
      assert(strcmp(info.friend_group_name, "group-a") == 0 &&
             strcmp(info.history_id, "history-1") == 0 &&
             strcmp(info.mime_type, "audio/ogg") == 0 &&
             info.size_bytes == 4u && info.received_bytes == 4u &&
             wire.bytes_written == 4u && memcmp(wire.bytes, audio, 4u) == 0);
      h2_gizclaw_resp_storage_t tiny = {.data = buffer, .capacity = 1u};
      h2_gizclaw_friend_group_message_audio_info_t empty;
      assert(h2_gizclaw_resp_parse_friend_group_message_audio_download(
                 request, &tiny, &empty) == H2_PAL_ERR_NO_SPACE);
      assert(empty.friend_group_name == NULL && tiny.used == 0u);
      h2_gizclaw_pet_pixa_info_t wrong;
      assert(h2_gizclaw_resp_parse_pet_pixa_download(
                 request, &storage, &wrong) == H2_PAL_ERR_INVALID_ARG);
    } else {
      assert(storage.used == 0u && info.friend_group_name == NULL &&
             info.history_id == NULL);
    }
    assert(wire.cancel_count == (wire.result_done ? 0 : 1) &&
           wire.destroy_count == 1);
    h2_gizclaw_req_release(request);
    if (mode == 0u) {
      wire.next_event = 0u;
      wire.bytes_written = 0u;
      assert(h2_gizclaw_rpc_friend_group_message_audio_download(
                 service, (h2_gizclaw_str_t){"group-a", 7u},
                 (h2_gizclaw_str_t){"history-1", 9u}, download_test_sink, &wire,
                 1234u, &storage, &info) == H2_PAL_OK);
    }
    assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
    assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
    if (mode == 0u)
      assert(strcmp(info.history_id, "history-1") == 0);
  }
}

typedef struct track_lifetime_test {
  h2_gizclaw_service_t *service;
  h2_gizclaw_track_t track;
  atomic_uint entered;
  atomic_bool release_read;
  atomic_bool release_write;
  atomic_bool unset_done;
  h2_pal_result_t read_result;
  h2_pal_result_t write_result;
  h2_pal_result_t unset_result;
  size_t read_len;
} track_lifetime_test_t;

static h2_pal_result_t lifetime_track_read(void *user, uint8_t *pcm,
                                           size_t capacity, size_t *out_len) {
  track_lifetime_test_t *test = user;
  assert(capacity >= 2u);
  /* Callback execution must not hold the Service mutex. */
  assert(h2_gizclaw_service_pcm_readable_internal(test->service));
  atomic_fetch_add(&test->entered, 1u);
  while (!atomic_load(&test->release_read))
    sched_yield();
  pcm[0] = 0x12u;
  pcm[1] = 0x34u;
  *out_len = 2u;
  return H2_PAL_OK;
}

static h2_pal_result_t lifetime_track_write(void *user, const uint8_t *pcm,
                                            size_t len) {
  track_lifetime_test_t *test = user;
  assert(len == 2u && pcm[0] == 0x56u && pcm[1] == 0x78u);
  atomic_fetch_add(&test->entered, 1u);
  while (!atomic_load(&test->release_write))
    sched_yield();
  return H2_PAL_OK;
}

static void *lifetime_read_thread(void *user) {
  track_lifetime_test_t *test = user;
  uint8_t pcm[2];
  test->read_result = h2_gizclaw_service_pcm_read_internal(
      test->service, pcm, sizeof(pcm), &test->read_len);
  assert(pcm[0] == 0x12u && pcm[1] == 0x34u);
  return NULL;
}

static void *lifetime_write_thread(void *user) {
  track_lifetime_test_t *test = user;
  const uint8_t pcm[2] = {0x56u, 0x78u};
  test->write_result =
      h2_gizclaw_service_pcm_write_internal(test->service, pcm, sizeof(pcm));
  return NULL;
}

static void *lifetime_unset_thread(void *user) {
  track_lifetime_test_t *test = user;
  test->unset_result =
      h2_gizclaw_service_unset_track(test->service, &test->track);
  atomic_store(&test->unset_done, true);
  return NULL;
}

static void test_track_unset_waits_for_read_and_write(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_service(&env, 1u);
  track_lifetime_test_t test = {.service = service};
  const h2_gizclaw_track_vtable_t vtable = {.read = lifetime_track_read,
                                            .write = lifetime_track_write};
  test.track = (h2_gizclaw_track_t){.vtable = &vtable, .user = &test};
  assert(h2_gizclaw_service_set_track(service, &test.track) == H2_PAL_OK);
  pthread_t reader, writer, unsetter;
  assert(pthread_create(&reader, NULL, lifetime_read_thread, &test) == 0);
  assert(pthread_create(&writer, NULL, lifetime_write_thread, &test) == 0);
  wait_for_count(&test.entered, 2u);
  assert(pthread_create(&unsetter, NULL, lifetime_unset_thread, &test) == 0);
  bool unsetting = false;
  for (unsigned spin = 0u; spin < 1000000u && !unsetting; ++spin) {
    assert(h2_pal_mutex_lock(service->config.sync, service->mutex) ==
           H2_PAL_OK);
    unsetting = service->pcm_track_unsetting;
    assert(h2_pal_mutex_unlock(service->config.sync, service->mutex) ==
           H2_PAL_OK);
    sched_yield();
  }
  assert(unsetting && !atomic_load(&test.unset_done));
  uint8_t pcm[2] = {0};
  size_t len = 99u;
  assert(h2_gizclaw_service_pcm_read_internal(service, pcm, sizeof(pcm),
                                              &len) == H2_PAL_ERR_CLOSED);
  assert(len == 0u);
  assert(h2_gizclaw_service_pcm_write_internal(service, pcm, sizeof(pcm)) ==
         H2_PAL_ERR_CLOSED);
  assert(atomic_load(&test.entered) == 2u);
  assert(h2_gizclaw_service_set_track(service, &test.track) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(h2_gizclaw_service_unset_track(service, &test.track) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_ERR_INVALID_STATE);
  atomic_store(&test.release_read, true);
  assert(pthread_join(reader, NULL) == 0);
  assert(test.read_result == H2_PAL_OK && test.read_len == 2u);
  assert(!atomic_load(&test.unset_done));
  atomic_store(&test.release_write, true);
  assert(pthread_join(writer, NULL) == 0);
  assert(pthread_join(unsetter, NULL) == 0);
  assert(test.write_result == H2_PAL_OK && test.unset_result == H2_PAL_OK);
  /* No borrowed Track access after unset, even if the vtable is invalidated. */
  test.track.vtable = NULL;
  assert(h2_gizclaw_service_pcm_read_internal(service, pcm, sizeof(pcm),
                                              &len) == H2_PAL_ERR_CLOSED);
  assert(h2_gizclaw_service_pcm_write_internal(service, pcm, sizeof(pcm)) ==
         H2_PAL_ERR_CLOSED);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

typedef struct track_read_test {
  size_t len;
  h2_pal_result_t result;
} track_read_test_t;

static h2_pal_result_t boundary_track_read(void *user, uint8_t *pcm,
                                           size_t capacity, size_t *out_len) {
  (void)pcm;
  (void)capacity;
  track_read_test_t *test = user;
  *out_len = test->len;
  return test->result;
}

static void test_track_read_validation_and_rebinding(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_service(&env, 1u);
  track_read_test_t state = {0};
  const h2_gizclaw_track_vtable_t vtable = {.read = boundary_track_read};
  h2_gizclaw_track_t track = {.vtable = &vtable, .user = &state};
  h2_gizclaw_track_t other = track;
  assert(h2_gizclaw_service_set_track(service, &track) == H2_PAL_OK);
  assert(h2_gizclaw_service_set_track(service, &other) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(h2_gizclaw_service_unset_track(service, &other) ==
         H2_PAL_ERR_INVALID_STATE);
  uint8_t pcm[4];
  size_t len;
  const size_t invalid_lengths[] = {0u, 1u, 3u, 5u, SIZE_MAX};
  for (size_t i = 0; i < sizeof(invalid_lengths) / sizeof(invalid_lengths[0]);
       ++i) {
    state.len = invalid_lengths[i];
    len = 99u;
    assert(h2_gizclaw_service_pcm_read_internal(service, pcm, sizeof(pcm),
                                                &len) == H2_PAL_ERR_FORMAT);
    assert(len == 0u);
  }
  state.len = 4u;
  state.result = H2_PAL_ERR_WOULD_BLOCK;
  assert(h2_gizclaw_service_pcm_read_internal(service, pcm, sizeof(pcm),
                                              &len) == H2_PAL_ERR_WOULD_BLOCK);
  assert(len == 0u);
  state.result = H2_PAL_ERR_IO;
  assert(h2_gizclaw_service_pcm_read_internal(service, pcm, sizeof(pcm),
                                              &len) == H2_PAL_ERR_IO);
  assert(len == 0u);
  state.result = H2_PAL_OK;
  assert(h2_gizclaw_service_pcm_read_internal(service, pcm, sizeof(pcm),
                                              &len) == H2_PAL_OK);
  assert(len == 4u);
  assert(h2_gizclaw_service_pcm_write_internal(service, pcm, sizeof(pcm)) ==
         H2_PAL_ERR_UNSUPPORTED);
  assert(h2_gizclaw_service_unset_track(service, &track) == H2_PAL_OK);
  assert(!h2_gizclaw_service_pcm_readable_internal(service));
  assert(h2_gizclaw_service_set_track(service, &other) == H2_PAL_OK);
  assert(h2_gizclaw_service_pcm_readable_internal(service));
  assert(h2_gizclaw_service_unset_track(service, &other) == H2_PAL_OK);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

typedef struct speech_wire_test {
  h2_gizclaw_service_t *service;
  pthread_t app_thread;
  pthread_t network_thread;
  bool network_thread_set;
  bool extract;
  unsigned mode;
  h2_gizclaw_rpc_stream_fn receive;
  void *receive_user;
  atomic_uint starts;
  atomic_uint captures;
  atomic_uint captured_bytes;
  atomic_uint uploaded_bytes;
  atomic_uint destroys;
  atomic_uint callbacks;
  atomic_uint pacing_warnings;
  atomic_bool pause_write;
  unsigned writes;
  unsigned finishes;
  bool finished;
  unsigned response_stage;
  size_t capture_total;
  size_t bytes_len;
  uint8_t bytes[24000];
} speech_wire_test_t;
static speech_wire_test_t *s_speech;

static int speech_test_start(h2_gizclaw_client_t *client,
                             h2_gizclaw_rpc_method_t method,
                             h2_gizclaw_rpc_bytes_t payload, uint32_t timeout,
                             h2_gizclaw_rpc_stream_fn receive, void *user,
                             h2_gizclaw_rpc_request_t **out) {
  speech_wire_test_t *test = s_speech;
  assert(client == (h2_gizclaw_client_t *)s_env);
  assert(!pthread_equal(pthread_self(), test->app_thread));
  test->network_thread = pthread_self();
  test->network_thread_set = true;
  assert(timeout > 0u && timeout <= 2000u);
  assert(method == (test->extract ? H2_GIZCLAW_RPC_SERVER_SPEECH_EXTRACT
                                  : H2_GIZCLAW_RPC_SERVER_SPEECH_TRANSCRIBE));
  pb_istream_t stream = pb_istream_from_buffer(payload.data, payload.len);
  if (test->extract) {
    gizclaw_rpc_v1_SpeechExtractRequest *message = h2_pal_mem_alloc(
        h2_desktop_platform_default_allocator(), sizeof(*message));
    assert(message != NULL);
    memset(message, 0, sizeof(*message));
    assert(pb_decode(&stream, gizclaw_rpc_v1_SpeechExtractRequest_fields,
                     message));
    assert(strcmp(message->asr_model_name, "asr") == 0 &&
           strcmp(message->schema_json, "{}") == 0);
    h2_pal_mem_free(h2_desktop_platform_default_allocator(), message);
  } else {
    gizclaw_rpc_v1_SpeechTranscribeRequest message =
        gizclaw_rpc_v1_SpeechTranscribeRequest_init_zero;
    assert(pb_decode(&stream, gizclaw_rpc_v1_SpeechTranscribeRequest_fields,
                     &message));
    assert(strcmp(message.model_name, "asr") == 0);
  }
  *out = NULL;
  unsigned starts = atomic_fetch_add(&test->starts, 1u) + 1u;
  if (starts <= 2u)
    return H2_PAL_ERR_WOULD_BLOCK;
  test->receive = receive;
  test->receive_user = user;
  *out = (h2_gizclaw_rpc_request_t *)test;
  return H2_PAL_OK;
}

static int speech_test_write(h2_gizclaw_rpc_request_t *request,
                             const uint8_t *data, size_t len) {
  speech_wire_test_t *test = (speech_wire_test_t *)request;
  assert(pthread_equal(pthread_self(), test->network_thread));
  assert(!test->finished && len != 0u && len <= 640u);
  ++test->writes;
  if (atomic_load(&test->pause_write) || test->writes <= 2u)
    return H2_PAL_ERR_WOULD_BLOCK;
  assert(len <= sizeof(test->bytes) - test->bytes_len);
  memcpy(test->bytes + test->bytes_len, data, len);
  test->bytes_len += len;
  atomic_store_explicit(&test->uploaded_bytes, (unsigned)test->bytes_len,
                        memory_order_release);
  return H2_PAL_OK;
}

static int speech_test_finish(h2_gizclaw_rpc_request_t *request) {
  speech_wire_test_t *test = (speech_wire_test_t *)request;
  assert(pthread_equal(pthread_self(), test->network_thread));
  assert(test->bytes_len == atomic_load(&test->captured_bytes));
  assert(!test->finished);
  if (++test->finishes <= 2u)
    return H2_PAL_ERR_WOULD_BLOCK;
  test->finished = true;
  return H2_PAL_OK;
}

static int speech_test_result(h2_gizclaw_rpc_request_t *request,
                              h2_gizclaw_rpc_response_t *out) {
  speech_wire_test_t *test = (speech_wire_test_t *)request;
  assert(pthread_equal(pthread_self(), test->network_thread));
  memset(out, 0, sizeof(*out));
  if (test->mode == 17u || test->mode == 18u) {
    if (atomic_load(&test->captures) < 2u ||
        test->response_stage >= (test->mode == 17u ? 1u : 2u))
      return H2_PAL_ERR_WOULD_BLOCK;
    h2_gizclaw_rpc_stream_event_t event = {
        .kind = test->response_stage++ == 0u ? H2_GIZCLAW_RPC_STREAM_RESPONSE
                                             : H2_GIZCLAW_RPC_STREAM_EOS,
        .has_error = test->mode == 17u};
    int rc = test->receive(test->receive_user, &event);
    return rc == H2_PAL_OK ? H2_PAL_ERR_WOULD_BLOCK : rc;
  }
  if (test->mode == 6u) {
    h2_gizclaw_rpc_stream_event_t event = {
        .kind = H2_GIZCLAW_RPC_STREAM_RESPONSE, .has_error = true};
    return test->receive(test->receive_user, &event);
  }
  if (!test->finished)
    return H2_PAL_ERR_WOULD_BLOCK;
  if (test->response_stage < (test->mode == 9u ? 1u : 2u)) {
    h2_gizclaw_rpc_stream_event_t event = {
        .kind = test->response_stage++ == 0u ? H2_GIZCLAW_RPC_STREAM_RESPONSE
                                             : H2_GIZCLAW_RPC_STREAM_EOS};
    int rc = test->receive(test->receive_user, &event);
    return rc == H2_PAL_OK ? H2_PAL_ERR_WOULD_BLOCK : rc;
  }
  static const uint8_t transcribe[] = {0x0a, 5, 'h', 'e', 'l', 'l', 'o'};
  static const uint8_t extract[] = {0x0a, 5,   'h', 'e', 'l', 'l', 'o',
                                    0x12, 11,  '{', '"', 'v', 'a', 'l',
                                    'u',  'e', '"', ':', '7', '}'};
  static const uint8_t malformed[] = {0x0a, 10, 'x'};
  static const uint8_t invalid_utf8[] = {0x0a, 1, 0xff};
  const uint8_t *bytes = test->extract ? extract : transcribe;
  size_t len = test->extract ? sizeof(extract) : sizeof(transcribe);
  if (test->mode == 7u) {
    bytes = malformed;
    len = sizeof(malformed);
  }
  if (test->mode == 8u) {
    bytes = invalid_utf8;
    len = sizeof(invalid_utf8);
  }
  out->result_payload =
      h2_pal_mem_alloc(h2_desktop_platform_default_allocator(), len);
  assert(out->result_payload != NULL);
  memcpy(out->result_payload, bytes, len);
  out->result_payload_len = len;
  return H2_PAL_OK;
}

static void speech_test_cancel(h2_gizclaw_rpc_request_t *request) {
  assert(request == (h2_gizclaw_rpc_request_t *)s_speech);
  assert(pthread_equal(pthread_self(), s_speech->network_thread));
}
static void speech_test_destroy(h2_gizclaw_rpc_request_t *request) {
  assert(request == (h2_gizclaw_rpc_request_t *)s_speech);
  assert(pthread_equal(pthread_self(), s_speech->network_thread));
  atomic_fetch_add(&s_speech->destroys, 1u);
}
static const h2_gizclaw_async_rpc_ops_t speech_test_ops = {
    .start_stream = speech_test_start,
    .write = speech_test_write,
    .finish_write = speech_test_finish,
    .result = speech_test_result,
    .cancel = speech_test_cancel,
    .destroy = speech_test_destroy};

static h2_pal_result_t speech_test_capture(void *user, uint8_t *pcm,
                                           size_t capacity, size_t *out_len) {
  speech_wire_test_t *test = user;
  assert(!pthread_equal(pthread_self(), test->app_thread));
  /* The published route enables reads only after the network task starts RPC.
   */
  assert(test->network_thread_set &&
         !pthread_equal(pthread_self(), test->network_thread));
  if (test->mode == 3u)
    return H2_PAL_ERR_IO;
  unsigned offset = atomic_load(&test->captured_bytes);
  if (offset >= test->capture_total)
    return H2_PAL_ERR_WOULD_BLOCK;
  if (test->mode == 16u)
    assert(h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 30u) ==
           H2_PAL_OK);
  size_t len = test->capture_total - offset;
  if (len > capacity)
    len = capacity;
  for (size_t i = 0u; i < len; ++i)
    pcm[i] = (uint8_t)(offset + i);
  *out_len = len;
  atomic_store(&test->captured_bytes, offset + (unsigned)len);
  atomic_fetch_add(&test->captures, 1u);
  return H2_PAL_OK;
}
static void assert_conversation_route_conflict(h2_gizclaw_service_t *service) {
  const h2_pal_sync_api_t *sync = service->config.sync;
  assert(h2_gizclaw_service_pcm_readable_internal(service));
  assert(h2_pal_mutex_lock(sync, service->mutex) == H2_PAL_OK);
  void *speech = atomic_load(&service->speech_request);
  void *play = service->audio_play;
  assert(speech != NULL || play != NULL);
  assert(atomic_load(&service->media_request) == NULL);
  assert(h2_pal_mutex_unlock(sync, service->mutex) == H2_PAL_OK);
  h2_gizclaw_conversation_t *conversation = NULL;
  assert(h2_gizclaw_conversation_create(service, (h2_gizclaw_str_t){"test", 4},
                                        NULL, NULL, NULL,
                                        &conversation) == H2_PAL_OK);
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    assert(h2_gizclaw_service_audio_start(service) == H2_PAL_ERR_INVALID_STATE);
    assert(h2_gizclaw_conversation_cancel(conversation) == H2_PAL_OK);
    assert(h2_pal_mutex_lock(sync, service->mutex) == H2_PAL_OK);
    assert(atomic_load(&service->speech_request) == speech);
    assert(service->audio_play == play);
    assert(atomic_load(&service->media_request) == NULL);
    assert(h2_pal_mutex_unlock(sync, service->mutex) == H2_PAL_OK);
  }
  h2_gizclaw_conversation_release(conversation);
}

static h2_pal_result_t speech_running_clock(void *user, uint64_t *out) {
  test_env_t *env = user;
  h2_pal_result_t rc =
      h2_pal_time_get_monotonic_ms(h2_desktop_platform_time_api(), out);
  if (rc == H2_PAL_OK)
    *out += atomic_load(&env->clock_ms);
  return rc;
}

static int speech_pacing_log(void *user, h2_pal_log_level_t level,
                             const char *scope, const char *message) {
  speech_wire_test_t *test = user;
  if (strstr(message, "stage=uplink_cycle_overrun") != NULL) {
    assert(level == H2_PAL_LOG_WARN && strcmp(scope, "gizclaw") == 0);
    unsigned long long elapsed = 0, overrun = 0;
    assert(sscanf(message,
                  "stage=uplink_cycle_overrun elapsed_ms=%llu overrun_ms=%llu",
                  &elapsed, &overrun) == 2);
    assert(elapsed > 20u && overrun == elapsed - 20u);
    atomic_fetch_add(&test->pacing_warnings, 1u);
  }
  return H2_PAL_OK;
}

static void test_speech_managed_requests(void) {
  for (unsigned mode = 0u; mode < 19u; ++mode) {
    test_env_t env;
    h2_gizclaw_service_t *service = create_service(&env, 4u);
    atomic_store(&env.event_emitted, true);
    /* Audio now runs on real 20 ms deadlines; retain an offset to inject the
     * timeout case without freezing the audio worker's clock. */
    static const h2_pal_time_vtable_t tv = {.get_monotonic_ms =
                                                speech_running_clock};
    h2_pal_time_api_t time = {.user = &env, .vtable = &tv};
    service->client_config.time = &time;
    speech_wire_test_t test = {.service = service,
                               .app_thread = pthread_self(),
                               .extract = mode % 2u == 1u,
                               .mode = mode,
                               .capture_total = mode == 0u ? 24000u : 1280u};
    atomic_init(&test.pause_write,
                mode == 0u || mode == 2u || mode == 5u || mode == 12u ||
                    mode == 14u || mode == 15u || mode == 17u || mode == 18u);
    s_speech = &test;
    static const h2_pal_log_vtable_t log_vtable = {.write = speech_pacing_log};
    const h2_pal_log_api_t log = {.user = &test, .vtable = &log_vtable};
    service->client_config.log = &log;
    h2_gizclaw_async_rpc_test_set_ops(&speech_test_ops);
    const h2_gizclaw_track_vtable_t track_vtable = {.read =
                                                        speech_test_capture};
    h2_gizclaw_track_t track = {.user = &test, .vtable = &track_vtable};
    if (mode != 4u)
      assert(h2_gizclaw_service_set_track(service, &track) == H2_PAL_OK);
    char model[] = "asr", schema[] = "{}";
    h2_gizclaw_speech_transcribe_options_t asr = {
        .model_name = {model, 3u}, .content_type = {"audio/pcm", 9u}};
    h2_gizclaw_speech_extract_options_t extract = {
        .asr_model_name = {model, 3u},
        .extract_model_name = {"llm", 3u},
        .content_type = {"audio/pcm", 9u},
        .schema_json = {schema, 2u}};
    h2_gizclaw_req_t *request = NULL;
    h2_pal_result_t rc = test.extract
                             ? h2_gizclaw_req_create_speech_extract(
                                   service, mode, &extract, 2000u, &request)
                             : h2_gizclaw_req_create_speech_transcribe(
                                   service, mode, &asr, 2000u, &request);
    assert(rc == H2_PAL_OK && atomic_load(&test.starts) == 0u);
    model[0] = 'X';
    schema[0] = 'X';
    uint8_t storage_bytes[256];
    h2_gizclaw_resp_storage_t storage = {storage_bytes, sizeof(storage_bytes),
                                         0u};
    h2_gizclaw_speech_extract_response_t result = {0};
    h2_gizclaw_speech_transcribe_response_t transcript = {0};
    assert(h2_gizclaw_service_audio_end(service) == H2_PAL_ERR_INVALID_STATE);
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) ==
           H2_PAL_ERR_INVALID_STATE);
    if (mode == 13u)
      atomic_store(&env.connect_gate, false);
    assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
    if (mode == 4u) {
      assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) ==
             H2_PAL_ERR_INVALID_STATE);
      h2_gizclaw_req_release(request);
      assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
      assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
      continue;
    }
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    assert(h2_gizclaw_service_audio_start(service) == H2_PAL_OK);
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) ==
           H2_PAL_ERR_INVALID_STATE);
    if (mode == 13u) {
      assert(h2_gizclaw_service_audio_end(service) == H2_PAL_OK);
      atomic_store(&env.connect_gate, true);
    }
    if (mode == 12u) {
      wait_for_count(&test.captures, 1u);
      h2_gizclaw_req_release(request);
      assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
      assert(atomic_load(&test.destroys) == 1u);
      assert(atomic_load(&service->speech_request) == NULL);
      assert(h2_gizclaw_service_unset_track(service, &track) == H2_PAL_OK);
      assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
      continue;
    }
    h2_pal_result_t expected = H2_PAL_OK;
    if (mode == 4u)
      expected = H2_PAL_ERR_INVALID_STATE;
    else if (mode == 3u)
      expected = H2_PAL_ERR_IO;
    else if (mode == 6u)
      expected = H2_GIZCLAW_ERR_REMOTE;
    else if (mode == 17u || mode == 18u) {
      /* A busy upload must not hide remote rejection or an invalid early EOS.
       * Neither case requires audio_end, and no input was accepted by wire. */
      expected = mode == 17u ? H2_GIZCLAW_ERR_REMOTE : H2_PAL_ERR_FORMAT;
      rc = h2_gizclaw_req_wait(request, 3000u);
      if (rc != expected)
        fprintf(stderr, "speech early mode=%u rc=%d expected=%d captures=%u\n",
                mode, rc, expected, atomic_load(&test.captures));
      assert(rc == expected);
      assert(atomic_load(&test.captures) == 2u);
      assert(atomic_load(&test.uploaded_bytes) == 0u);
      assert(test.finishes == 0u);
    } else {
      wait_for_count(&test.starts, 3u);
      if (mode == 5u) {
        atomic_store(&env.clock_ms, 2000u);
        expected = H2_PAL_ERR_TIMEOUT;
      } else if (mode == 2u) {
        wait_for_count(&test.captures, 1u);
        assert(h2_gizclaw_req_cancel(request) == H2_PAL_OK);
        expected = H2_PAL_ERR_CLOSED;
      } else if (mode == 14u || mode == 15u) {
        wait_for_count(&test.captures, 1u);
        if (mode == 14u)
          assert(h2_gizclaw_service_unset_track(service, &track) == H2_PAL_OK);
        else
          assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
        expected = H2_PAL_ERR_CLOSED;
      } else {
        if (mode != 13u) {
          wait_for_count(&test.captures, 2u);
          if (mode == 0u) {
            /* One transport slot plus one retained PCM frame, no ASR ring.
             * A blocked write must not consume or overwrite more input. */
            struct timespec pause = {.tv_nsec = 100000000L};
            nanosleep(&pause, NULL);
            assert(atomic_load(&test.captures) == 2u);
            assert(atomic_load(&test.captured_bytes) == 1280u);
            assert(atomic_load(&test.uploaded_bytes) == 0u);
          }
          assert(h2_gizclaw_req_wait(request, 0u) == H2_PAL_ERR_TIMEOUT);
        }
        if (mode <= 1u)
          assert_conversation_route_conflict(service);
        if (mode == 0u) {
          /* A second Speech request cannot steal the first one's Track route.
           */
          asr.model_name = (h2_gizclaw_str_t){"asr", 3u};
          h2_gizclaw_req_t *conflict = NULL;
          assert(h2_gizclaw_req_create_speech_transcribe(
                     service, 99u, &asr, 2000u, &conflict) == H2_PAL_OK);
          assert(h2_gizclaw_req_do(conflict, NULL, NULL, NULL, NULL) ==
                 H2_PAL_ERR_BUSY);
          h2_gizclaw_req_release(conflict);
        }
        assert(h2_gizclaw_service_audio_end(service) == H2_PAL_OK);
        unsigned accepted = atomic_load(&test.captured_bytes);
        assert(h2_gizclaw_service_audio_end(service) == H2_PAL_OK);
        atomic_store(&test.pause_write, false);
        if (mode == 9u)
          expected = H2_PAL_ERR_FORMAT;
        rc = h2_gizclaw_req_wait(request, 3000u);
        if (rc != expected)
          fprintf(stderr, "speech mode %u rc=%d expected=%d\n", mode, rc,
                  expected);
        assert(rc == expected);
        assert(atomic_load(&test.captured_bytes) == accepted);
        if (mode == 13u)
          assert(accepted == 0u);
        assert(test.bytes_len == accepted && test.finishes == 3u);
        for (size_t i = 0u; i < test.bytes_len; ++i)
          assert(test.bytes[i] == (uint8_t)i);
      }
    }
    assert(h2_gizclaw_req_wait(request, 3000u) == expected);
    if (mode == 16u)
      wait_for_count(&test.pacing_warnings, 2u);
    assert(atomic_load(&service->speech_request) == NULL);
    assert(atomic_load(&test.callbacks) == 0u);
    if (mode != 4u)
      assert(atomic_load(&test.destroys) == 1u);
    rc = test.extract
             ? h2_gizclaw_resp_parse_speech_extract(request, &storage, &result)
             : h2_gizclaw_resp_parse_speech_transcribe(request, &storage,
                                                       &transcript);
    h2_pal_result_t parse_expected =
        mode == 7u || mode == 8u ? H2_PAL_ERR_FORMAT : expected;
    assert(rc == parse_expected);
    if (parse_expected != H2_PAL_OK) {
      assert(result.transcript.len == 0u && transcript.transcript.len == 0u &&
             storage.used == 0u);
    } else {
      h2_gizclaw_str_t text =
          test.extract ? result.transcript : transcript.transcript;
      assert(text.len == 5u && memcmp(text.data, "hello", 5u) == 0);
      if (test.extract)
        assert(result.result_json.len == 11u);
      h2_gizclaw_resp_storage_t tiny = {storage_bytes, 1u, 0u};
      if (test.extract) {
        h2_gizclaw_speech_extract_response_t short_result;
        assert(h2_gizclaw_resp_parse_speech_extract(
                   request, &tiny, &short_result) == H2_PAL_ERR_NO_SPACE);
        assert(short_result.transcript.len == 0u && tiny.used == 0u);
      } else {
        assert(h2_gizclaw_resp_parse_speech_extract(
                   request, &storage, &result) == H2_PAL_ERR_INVALID_ARG);
      }
    }
    h2_gizclaw_req_release(request);
    if (mode == 14u) {
      /* Losing a Track closes this request, not the Service / Peer. */
      asr.model_name = (h2_gizclaw_str_t){"asr", 3u};
      h2_gizclaw_req_t *next = NULL;
      assert(h2_gizclaw_req_create_speech_transcribe(service, 100u, &asr, 2000u,
                                                     &next) == H2_PAL_OK);
      assert(h2_gizclaw_req_do(next, NULL, NULL, NULL, NULL) ==
             H2_PAL_ERR_INVALID_STATE);
      h2_gizclaw_req_release(next);
    }
    if (mode != 4u && mode != 14u)
      assert(h2_gizclaw_service_unset_track(service, &track) == H2_PAL_OK);
    assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
    rc = h2_gizclaw_service_deinit(service);
    if (rc != H2_PAL_OK)
      fprintf(stderr,
              "speech mode %u deinit=%d active=%zu refs=%zu caller=%zu "
              "pending=%d\n",
              mode, rc, service->active_count, service->request_reference_count,
              service->caller_reference_count, service->terminal_pending);
    assert(rc == H2_PAL_OK);
    if (parse_expected == H2_PAL_OK) {
      h2_gizclaw_str_t text =
          test.extract ? result.transcript : transcript.transcript;
      assert(memcmp(text.data, "hello", 5u) == 0);
    }
  }
  h2_gizclaw_async_rpc_test_set_ops(NULL);
}

static void test_speech_options_and_created_lifetime(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_service(&env, 1u);
  char schema[16386];
  memset(schema, ' ', sizeof(schema));
  schema[0] = '{';
  schema[1] = '}';
  h2_gizclaw_speech_extract_options_t options = {
      .asr_model_name = {"asr", 3u},
      .extract_model_name = {"llm", 3u},
      .content_type = {"audio/pcm", 9u},
      .schema_json = {schema, 16384u}};
  h2_gizclaw_req_t *request = NULL;
  assert(h2_gizclaw_req_create_speech_extract(service, 1u, &options, 1000u,
                                              &request) == H2_PAL_OK);
  uint8_t bytes[128];
  h2_gizclaw_resp_storage_t storage = {bytes, sizeof(bytes), 0u};
  h2_gizclaw_speech_extract_response_t out = {.transcript = {"dirty", 5u}};
  assert(h2_gizclaw_resp_parse_speech_extract(request, &storage, &out) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(out.transcript.len == 0u && storage.used == 0u);
  assert(h2_gizclaw_req_wait(request, 0u) == H2_PAL_ERR_INVALID_STATE);
  h2_gizclaw_req_t *invalid = (h2_gizclaw_req_t *)1;
  options.schema_json.len = 16385u;
  assert(h2_gizclaw_req_create_speech_extract(service, 2u, &options, 1000u,
                                              &invalid) ==
             H2_PAL_ERR_INVALID_ARG &&
         invalid == NULL);
  options.schema_json.len = 2u;
  options.asr_model_name = (h2_gizclaw_str_t){"\xc0\x80", 2u};
  assert(h2_gizclaw_req_create_speech_extract(
             service, 2u, &options, 1000u, &invalid) == H2_PAL_ERR_INVALID_ARG);
  options.asr_model_name = (h2_gizclaw_str_t){"asr", 3u};
  assert(h2_gizclaw_req_create_speech_extract(
             service, 2u, &options, 0u, &invalid) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_ERR_INVALID_STATE);
  h2_gizclaw_req_release(request);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

typedef struct uplink_start_failure {
  unsigned starts, joins;
  bool downlink;
} uplink_start_failure_t;
static int failing_uplink_start(void *user,
                                const h2_pal_task_options_t *options,
                                h2_pal_task_entry_t entry, void *ctx,
                                h2_pal_task_t **out) {
  uplink_start_failure_t *test = user;
  ++test->starts;
  if (strcmp(options->name, test->downlink
                                ? h2_gizclaw_audio_downlink_task_name
                                : h2_gizclaw_audio_uplink_task_name) == 0) {
    *out = NULL;
    return H2_PAL_ERR_TASK;
  }
  return h2_pal_task_start(h2_desktop_platform_task_api(), options, entry, ctx,
                           out);
}
static int failing_uplink_join(void *user, h2_pal_task_t *task) {
  ++((uplink_start_failure_t *)user)->joins;
  return h2_pal_task_join(h2_desktop_platform_task_api(), task);
}
static void test_service_uplink_start_failure_cleans_network_task(void) {
  for (unsigned mode = 0; mode < 2; ++mode) {
    test_env_t env;
    h2_gizclaw_service_t *service = create_service(&env, 1u);
    atomic_store(&env.event_emitted, true);
    uplink_start_failure_t test = {.downlink = mode == 1};
    const h2_pal_task_vtable_t vt = {.start = failing_uplink_start,
                                     .join = failing_uplink_join};
    const h2_pal_task_api_t tasks = {.user = &test, .vtable = &vt};
    service->config.task = &tasks;
    assert(h2_gizclaw_service_start(service) == H2_PAL_ERR_TASK);
    assert(test.starts == 2u + mode && test.joins == 1u + mode);
    assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
    assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
  }
}

typedef struct playback_test {
  pthread_t app_thread;
  download_test_wire_t *wire;
  atomic_bool blocked;
  atomic_uint calls;
  h2_pal_result_t result;
  uint8_t pcm[1280];
  size_t len;
} playback_test_t;

static h2_pal_result_t playback_idle_read(void *user, uint8_t *pcm,
                                          size_t capacity, size_t *out_len) {
  (void)user;
  (void)pcm;
  (void)capacity;
  *out_len = 0;
  return H2_PAL_ERR_WOULD_BLOCK;
}

static h2_pal_result_t playback_write(void *user, const uint8_t *pcm,
                                      size_t len) {
  playback_test_t *test = user;
  assert(!pthread_equal(pthread_self(), test->app_thread));
  assert(!pthread_equal(pthread_self(), test->wire->network_thread));
  atomic_fetch_add(&test->calls, 1);
  if (atomic_load(&test->blocked))
    return H2_PAL_ERR_WOULD_BLOCK;
  if (test->result != H2_PAL_OK)
    return test->result;
  assert(len > 0 && len <= 640 && len % 2 == 0 &&
         len <= sizeof(test->pcm) - test->len);
  memcpy(test->pcm + test->len, pcm, len);
  test->len += len;
  return H2_PAL_OK;
}

static void test_audio_play_request(void) {
  for (unsigned mode = 0; mode < 16; ++mode) {
    test_env_t env;
    h2_gizclaw_service_t *service = create_profile_service(&env);
    static const h2_pal_time_vtable_t clock_vtable = {.get_monotonic_ms =
                                                          fake_req_clock};
    h2_pal_time_api_t clock = {.user = &env, .vtable = &clock_vtable};
    service->client_config.time =
        mode == 2 ? &clock : h2_desktop_platform_time_api();
    h2_gizclaw_async_rpc_test_set_ops(&download_test_ops);
    fixture_t fixture = {0};
    make_packet(&fixture, 1);
    headers(&fixture, 123, 1, 0, 0);
    packet_page(&fixture, 4, 960, 123, 2, fixture.packet, fixture.packet_len);
    if (mode == 8)
      --fixture.len;
    if (mode == 9)
      fixture.bytes[fixture.len - 1] ^= 1;
    uint8_t metadata[128];
    pb_ostream_t output = pb_ostream_from_buffer(metadata, sizeof(metadata));
    assert(pb_encode_tag(&output, PB_WT_STRING, 1) &&
           pb_encode_string(&output, (const pb_byte_t *)(mode == 6 ? "x" : "h"),
                            1));
    const char *mime = mode == 7 ? "text/html" : "audio/ogg";
    assert(pb_encode_tag(&output, PB_WT_STRING, 2) &&
           pb_encode_string(&output, (const pb_byte_t *)mime, strlen(mime)) &&
           pb_encode_tag(&output, PB_WT_VARINT, 3) &&
           pb_encode_varint(&output, fixture.len + (mode == 13 ? 1 : 0)) &&
           pb_encode_tag(&output, PB_WT_STRING, 4) &&
           pb_encode_string(&output, (const pb_byte_t *)"w", 1));
    const uint8_t input[] = {0x0a, 1, 'h', 0x12, 1, 'w'};
    download_test_wire_t wire = {
        .method = H2_GIZCLAW_RPC_SERVER_WORKSPACE_HISTORY_AUDIO_DOWNLOAD,
        .input = {input, sizeof(input)},
        .events = {{.kind = H2_GIZCLAW_RPC_STREAM_RESPONSE,
                    .result_payload = {metadata, output.bytes_written}},
                   {.kind = H2_GIZCLAW_RPC_STREAM_DATA,
                    .data = {fixture.bytes, 17}},
                   {.kind = H2_GIZCLAW_RPC_STREAM_DATA,
                    .data = {fixture.bytes + 17, fixture.len - 17}},
                   {.kind = H2_GIZCLAW_RPC_STREAM_EOS}},
        .event_count = mode == 10 ? 3 : 4,
        .start_busy = 2,
        .finish_busy = 2};
    if (mode == 12)
      wire.events[0].has_error = true;
    if (mode == 14)
      wire.events[1] = wire.events[0];
    s_download_wire = &wire;
    playback_test_t track_test = {.app_thread = pthread_self(),
                                  .wire = &wire,
                                  .blocked = mode <= 3 || mode == 11,
                                  .result =
                                      mode == 4 ? H2_PAL_ERR_IO : H2_PAL_OK};
    const h2_gizclaw_track_vtable_t vt = {
        .read = mode == 0 ? playback_idle_read : NULL, .write = playback_write};
    h2_gizclaw_track_t track = {.user = &track_test, .vtable = &vt};
    if (mode != 5)
      assert(h2_gizclaw_service_set_track(service, &track) == H2_PAL_OK);
    assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
    char workspace[] = "w", history[] = "h";
    h2_gizclaw_req_t *req = NULL;
    assert(h2_gizclaw_req_create_audio_play(
               service, 1, (h2_gizclaw_str_t){workspace, 1},
               (h2_gizclaw_str_t){history, 1}, 1234, &req) == H2_PAL_OK);
    assert(wire.start_count == 0 && atomic_load(&track_test.calls) == 0);
    workspace[0] = history[0] = 'x'; /* Parameters must have been copied. */
    assert(h2_gizclaw_req_do(req, NULL, NULL, NULL, NULL) ==
           (mode == 5 ? H2_PAL_ERR_INVALID_STATE : H2_PAL_OK));
    if (mode <= 3 || mode == 11) {
      for (unsigned spins = 0; spins < 2000 && !atomic_load(&track_test.calls);
           ++spins)
        h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 1);
      assert(atomic_load(&track_test.calls) > 0);
      assert(h2_gizclaw_req_wait(req, 0) == H2_PAL_ERR_TIMEOUT);
      if (mode == 0) {
        assert_conversation_route_conflict(service);
        h2_gizclaw_req_t *conflict;
        assert(h2_gizclaw_req_create_audio_play(
                   service, 2, (h2_gizclaw_str_t){"w", 1},
                   (h2_gizclaw_str_t){"h", 1}, 1234, &conflict) == H2_PAL_OK);
        assert(h2_gizclaw_req_do(conflict, NULL, NULL, NULL, NULL) ==
               H2_PAL_ERR_BUSY);
        assert(h2_gizclaw_req_wait(conflict, 0) == H2_PAL_ERR_INVALID_STATE);
        h2_gizclaw_req_release(conflict);
        atomic_store(&track_test.blocked, false);
      } else if (mode == 1)
        assert(h2_gizclaw_req_cancel(req) == H2_PAL_OK);
      else if (mode == 2)
        atomic_fetch_add(&env.clock_ms, 2000);
      else if (mode == 3)
        assert(h2_gizclaw_service_unset_track(service, &track) == H2_PAL_OK);
      else {
        h2_gizclaw_req_release(req);
        req = NULL;
        assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
      }
    }
    h2_pal_result_t expected = H2_PAL_ERR_FORMAT;
    switch (mode) {
    case 0:
    case 15:
      expected = H2_PAL_OK;
      break;
    case 1:
    case 3:
      expected = H2_PAL_ERR_CLOSED;
      break;
    case 2:
      expected = H2_PAL_ERR_TIMEOUT;
      break;
    case 4:
      expected = H2_PAL_ERR_IO;
      break;
    case 5:
      expected = H2_PAL_ERR_INVALID_STATE;
      break;
    case 7:
      expected = H2_PAL_ERR_UNSUPPORTED;
      break;
    case 12:
      expected = H2_GIZCLAW_ERR_REMOTE;
      break;
    }
    if (req != NULL) {
      h2_pal_result_t rc = h2_gizclaw_req_wait(req, 2000);
      if (rc != expected)
        fprintf(stderr, "AudioPlay mode=%u rc=%d expected=%d\n", mode, rc,
                expected);
      assert(rc == expected);
    }
    assert(service->audio_play == NULL);
    if (mode == 0 || mode == 15) {
      assert(track_test.len == 640);
      int opus_error;
      OpusDecoder *decoder = opus_decoder_create(16000, 1, &opus_error);
      assert(decoder && opus_error == OPUS_OK);
      opus_int16 pcm[320];
      assert(opus_decode(decoder, fixture.packet,
                         (opus_int32)fixture.packet_len, pcm, 320, 0) == 320);
      size_t nonzero = 0;
      for (size_t i = 0; i < 320; ++i) {
        uint16_t value = (uint16_t)pcm[i];
        assert(track_test.pcm[i * 2] == (uint8_t)value &&
               track_test.pcm[i * 2 + 1] == (uint8_t)(value >> 8));
        nonzero += value != 0;
      }
      assert(nonzero > 0);
      opus_decoder_destroy(decoder);
    }
    unsigned calls = atomic_load(&track_test.calls);
    h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 3);
    assert(calls == atomic_load(&track_test.calls));
    h2_gizclaw_req_release(req);
    assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
    assert(wire.destroy_count == (mode == 5 ? 0 : 1));
    assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
  }
}

typedef struct conversation_test {
  h2_gizclaw_service_t *service;
  test_env_t *env;
  pthread_t app_thread, network_thread;
  atomic_bool connected, connect_gate, bos, eos, canceled, reply_sent;
  atomic_bool playback_blocked, echo_blocked, done;
  atomic_size_t captured, written;
  atomic_uint starts, joins;
  atomic_uint turns_done;
  uint8_t pending[H2_GIZCLAW_CONVERSATION_OPUS_MAX_BYTES];
  size_t pending_len, read_offset, write_offset, hook_offset;
  uint8_t output[16000];
  size_t packets;
  unsigned event_close_count, mode;
  unsigned bos_attempts, eos_attempts;
  unsigned reply_events;
  unsigned reply_text_ends, transcript_text_ends;
  atomic_bool small_buffer_rejected;
  unsigned filler_callbacks;
  unsigned loss_markers;
  h2_pal_result_t result;
  char stream[64];
  /* Mode 20: the library-owned speaker Track, read by the app thread. */
  h2_gizclaw_track_t *owned_track;
} conversation_test_t;
static conversation_test_t *s_conversation;

static void conversation_test_probe(conversation_test_t *test) {
  static const char tag;
  probe_test_t state = {.env = test->env};
  test->env->rpc_result = H2_PAL_OK;
  h2_gizclaw_req_t *request = NULL;
  assert(h2_gizclaw_req_create_send_internal(test->service, 999, 1000, &tag,
                                             probe_send, probe_destroy, &state,
                                             &request) == H2_PAL_OK);
  assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
  assert(h2_gizclaw_req_wait(request, 1000) == H2_PAL_OK);
  h2_gizclaw_req_release(request);
  wait_for_count(&state.destroys, 1u);
  assert(atomic_load(&state.destroys) == 1);
}

static void conversation_test_filler(void *user) {
  conversation_test_t *test = user;
  assert(pthread_equal(pthread_self(), test->app_thread));
  ++test->filler_callbacks;
}

static size_t conversation_test_notification_count(conversation_test_t *test) {
  h2_gizclaw_service_t *service = test->service;
  assert(h2_pal_mutex_lock(service->config.sync, service->mutex) == H2_PAL_OK);
  size_t count = service->queued_event_count;
  assert(h2_pal_mutex_unlock(service->config.sync, service->mutex) ==
         H2_PAL_OK);
  return count;
}

static h2_pal_result_t conversation_test_connect(h2_gizclaw_client_t *client) {
  conversation_test_t *test = s_conversation;
  test->network_thread = pthread_self();
  while (!atomic_load(&test->connect_gate))
    h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 1);
  assert(h2_gizclaw_test_replace_event_stream(
             client, (gzc_event_stream_t *)test) == NULL);
  atomic_store(&test->connected, true);
  return H2_PAL_OK;
}

static int conversation_test_send(void *user, gzc_event_stream_t *stream,
                                  const gzc_peer_event_t *event) {
  conversation_test_t *test = user;
  assert(stream == (gzc_event_stream_t *)test);
  assert(pthread_equal(pthread_self(), test->network_thread));
  if (event->type == gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_BOS) {
    assert(!atomic_load(&test->bos));
    assert(event->payload.bos.sequence == 0u);
    if (test->bos_attempts++ != 0)
      assert(strcmp(test->stream, event->payload.bos.stream_id) == 0);
    snprintf(test->stream, sizeof(test->stream), "%s",
             event->payload.bos.stream_id);
    if ((test->mode == 4 && test->bos_attempts <= 3) || test->mode == 5) {
      assert(atomic_load(&test->captured) == 0);
      return test->bos_attempts % 2 ? GZC_ERR_WOULD_BLOCK : GZC_ERR_TIMEOUT;
    }
    atomic_store(&test->bos, true);
  } else {
    assert(event->type == gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_EOS);
    assert(atomic_load(&test->bos));
    assert(event->payload.eos.sequence == 1u);
    if (test->mode == 4 && ++test->eos_attempts <= 3)
      return GZC_ERR_WOULD_BLOCK;
    if (event->payload.eos.has_error)
      atomic_store(&test->canceled, true);
    atomic_store(&test->eos, true);
  }
  return GZC_OK;
}

static int conversation_test_read_event(void *user, gzc_event_stream_t *stream,
                                        int timeout, gzc_peer_event_t *event) {
  conversation_test_t *test = user;
  (void)timeout;
  assert(stream == (gzc_event_stream_t *)test);
  if (test->mode == 20) {
    /* Realtime barge-in with all twelve first-burst packets already echoed:
     * 0 BOS turn-one, 1 TEXT_DELTA turn-one, 2 BOS turn-two (interrupts
     * turn-one), 3 late EOS{STREAM_INTERRUPTED} turn-one (dropped), then
     * after the second burst and the input commit: 4 TEXT_DONE turn-two,
     * 5 EOS turn-two (terminal). */
    unsigned stage = test->reply_events;
    if (stage >= 6 || test->packets < 12u ||
        (stage >= 4 && (test->packets < 16u || !atomic_load(&test->eos))))
      return GZC_ERR_WOULD_BLOCK;
    ++test->reply_events;
    memset(event, 0, sizeof(*event));
    event->version = GZC_PEER_EVENT_VERSION;
    char id[64];
    snprintf(id, sizeof(id), "%s:%s", test->stream,
             stage == 0 || stage == 1 || stage == 3 ? "turn-one" : "turn-two");
    if (stage == 0 || stage == 2) {
      event->type = gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_BOS;
      event->which_payload = gizclaw_events_v1_PeerEvent_bos_tag;
      snprintf(event->payload.bos.stream_id,
               sizeof(event->payload.bos.stream_id), "%s", id);
      snprintf(event->payload.bos.label, sizeof(event->payload.bos.label),
               "assistant");
      event->payload.bos.kind = gizclaw_events_v1_StreamKind_STREAM_KIND_TEXT;
    } else if (stage == 1) {
      event->type = gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_TEXT_DELTA;
      event->which_payload = gizclaw_events_v1_PeerEvent_text_delta_tag;
      snprintf(event->payload.text_delta.stream_id,
               sizeof(event->payload.text_delta.stream_id), "%s", id);
      snprintf(event->payload.text_delta.label,
               sizeof(event->payload.text_delta.label), "assistant");
      snprintf(event->payload.text_delta.text,
               sizeof(event->payload.text_delta.text), "reply");
    } else if (stage == 4) {
      event->type = gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_TEXT_DONE;
      event->which_payload = gizclaw_events_v1_PeerEvent_text_done_tag;
      snprintf(event->payload.text_done.stream_id,
               sizeof(event->payload.text_done.stream_id), "%s", id);
      snprintf(event->payload.text_done.label,
               sizeof(event->payload.text_done.label), "assistant");
      snprintf(event->payload.text_done.text,
               sizeof(event->payload.text_done.text), "reply");
    } else {
      event->type = gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_EOS;
      event->which_payload = gizclaw_events_v1_PeerEvent_eos_tag;
      snprintf(event->payload.eos.stream_id,
               sizeof(event->payload.eos.stream_id), "%s", id);
      snprintf(event->payload.eos.label, sizeof(event->payload.eos.label),
               "assistant");
      if (stage == 3) {
        event->payload.eos.has_error = true;
        snprintf(event->payload.eos.error.code,
                 sizeof(event->payload.eos.error.code), "STREAM_INTERRUPTED");
      }
    }
    return GZC_OK;
  }
  if (test->mode == 15 || test->mode == 16) {
    unsigned stage = test->reply_events;
    bool second = stage >= 8;
    bool transcript = stage == 0 || stage == 1 || stage == 8 || stage == 9;
    if (stage >= 15 || test->packets < (second ? 4u : 2u) ||
        atomic_load(&test->canceled) ||
        (stage == 14 && test->mode == 15 && !atomic_load(&test->eos)))
      return GZC_ERR_WOULD_BLOCK;
    ++test->reply_events;
    memset(event, 0, sizeof(*event));
    event->version = GZC_PEER_EVENT_VERSION;
    bool stale = stage == 7 || stage == 11 || stage == 12;
    char route_id[64];
    snprintf(route_id, sizeof(route_id), "%s:%s", test->stream,
             second && !stale ? "turn-two" : "turn-one");
    const char *id =
        transcript ? (second ? "transcript-two" : "transcript-one") : route_id;
    const char *label = transcript ? "transcript" : "assistant";
    if (stage == 0 || stage == 2 || stage == 8 || stage == 10) {
      event->type = gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_BOS;
      event->which_payload = gizclaw_events_v1_PeerEvent_bos_tag;
      snprintf(event->payload.bos.stream_id,
               sizeof(event->payload.bos.stream_id), "%s", id);
      snprintf(event->payload.bos.label, sizeof(event->payload.bos.label), "%s",
               label);
      event->payload.bos.kind = gizclaw_events_v1_StreamKind_STREAM_KIND_TEXT;
    } else if (stage == 3) {
      event->type = gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_TEXT_DELTA;
      event->which_payload = gizclaw_events_v1_PeerEvent_text_delta_tag;
      snprintf(event->payload.text_delta.stream_id,
               sizeof(event->payload.text_delta.stream_id), "%s", id);
      snprintf(event->payload.text_delta.label,
               sizeof(event->payload.text_delta.label), "%s", label);
      snprintf(event->payload.text_delta.text,
               sizeof(event->payload.text_delta.text), "reply");
    } else if (stage == 1 || stage == 5 || stage == 7 || stage == 9 ||
               stage == 12 || stage == 13) {
      event->type = gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_TEXT_DONE;
      event->which_payload = gizclaw_events_v1_PeerEvent_text_done_tag;
      snprintf(event->payload.text_done.stream_id,
               sizeof(event->payload.text_done.stream_id), "%s", id);
      snprintf(event->payload.text_done.label,
               sizeof(event->payload.text_done.label), "%s", label);
      snprintf(event->payload.text_done.text,
               sizeof(event->payload.text_done.text), "%s",
               stale        ? "stale"
               : stage == 5 ? ""
               : transcript ? "heard"
                            : "reply");
    } else {
      event->type = gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_EOS;
      event->which_payload = gizclaw_events_v1_PeerEvent_eos_tag;
      snprintf(event->payload.eos.stream_id,
               sizeof(event->payload.eos.stream_id), "%s", id);
      snprintf(event->payload.eos.label, sizeof(event->payload.eos.label), "%s",
               label);
    }
    return GZC_OK;
  }
  if ((test->mode == 13 || test->mode == 14) && atomic_load(&test->bos) &&
      test->reply_events == 0) {
    ++test->reply_events;
    memset(event, 0, sizeof(*event));
    event->version = GZC_PEER_EVENT_VERSION;
    event->type = gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_TEXT_DELTA;
    event->which_payload = gizclaw_events_v1_PeerEvent_text_delta_tag;
    snprintf(event->payload.text_delta.stream_id,
             sizeof(event->payload.text_delta.stream_id), "%s", test->stream);
    snprintf(event->payload.text_delta.text,
             sizeof(event->payload.text_delta.text), "borrowed-wire-text");
    return GZC_OK;
  }
  if (!atomic_load(&test->eos) || atomic_load(&test->canceled) ||
      atomic_load(&test->reply_sent))
    return GZC_ERR_WOULD_BLOCK;
  bool transcript = test->mode == 7 && test->reply_events == 0;
  ++test->reply_events;
  if (!transcript)
    atomic_store(&test->reply_sent, true);
  memset(event, 0, sizeof(*event));
  event->version = GZC_PEER_EVENT_VERSION;
  event->type = gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_EOS;
  event->which_payload = gizclaw_events_v1_PeerEvent_eos_tag;
  snprintf(event->payload.eos.stream_id, sizeof(event->payload.eos.stream_id),
           "%s", test->stream);
  snprintf(event->payload.eos.label, sizeof(event->payload.eos.label), "%s",
           transcript ? "transcript" : "assistant");
  if (test->mode == 6) {
    event->payload.eos.has_error = true;
    snprintf(event->payload.eos.error.code,
             sizeof(event->payload.eos.error.code), "TEST_REMOTE_ERROR");
  }
  return GZC_OK;
}

static void conversation_test_close_event(void *user,
                                          gzc_event_stream_t *stream) {
  conversation_test_t *test = user;
  assert(stream == (gzc_event_stream_t *)test);
  ++test->event_close_count;
}

static int conversation_test_read_packet(void *user, gzc_client_t *client,
                                         int timeout, uint8_t *protocol,
                                         gzc_buf_t *payload) {
  (void)user;
  (void)client;
  (void)timeout;
  (void)protocol;
  (void)payload;
  assert(!"Conversation must receive RTP through PAL Track, not raw packets");
  return GZC_ERR_WOULD_BLOCK;
}

static h2_pal_result_t conversation_test_poll(h2_gizclaw_client_t *client,
                                              int timeout) {
  (void)client;
  (void)timeout;
  conversation_test_t *test = s_conversation;
  if (test->mode == 4 && !atomic_load(&test->small_buffer_rejected)) {
    size_t len = 99;
    h2_pal_result_t rc = h2_gizclaw_service_media_read_opus(
        test->service, test->pending, 1, &len);
    assert(len == 0);
    if (rc == H2_PAL_ERR_WOULD_BLOCK)
      return rc;
    assert(rc == H2_PAL_ERR_INVALID_ARG);
    atomic_store(&test->small_buffer_rejected, true);
  }
  if (test->pending_len == 0) {
    h2_pal_result_t rc = h2_gizclaw_service_media_read_opus(
        test->service, test->pending, sizeof(test->pending),
        &test->pending_len);
    assert(rc == H2_PAL_OK || rc == H2_PAL_ERR_WOULD_BLOCK);
  }
  /* Mode 19: after the first echoed packet the transport reports an RTP
   * loss (opus == NULL, len == 0). It must reach the decoder as a PLC frame
   * and the following valid packets must still play. */
  if (test->mode == 19 && test->packets == 1u && test->loss_markers == 0u) {
    h2_pal_result_t rc =
        h2_gizclaw_service_media_write_opus(test->service, NULL, 0u);
    if (rc == H2_PAL_ERR_WOULD_BLOCK)
      return rc;
    assert(rc == H2_PAL_OK);
    test->loss_markers = 1u;
  }
  if (test->pending_len != 0) {
    assert(atomic_load(&test->bos));
    h2_pal_result_t rc = h2_gizclaw_service_media_write_opus(
        test->service, test->pending, test->pending_len);
    if (rc == H2_PAL_ERR_WOULD_BLOCK)
      atomic_store(&test->echo_blocked, true);
    else {
      assert(rc == H2_PAL_OK);
      test->pending_len = 0;
      ++test->packets;
    }
  }
  return H2_PAL_ERR_WOULD_BLOCK;
}

static h2_pal_result_t conversation_test_track_read(void *user, uint8_t *pcm,
                                                    size_t capacity,
                                                    size_t *len) {
  conversation_test_t *test = user;
  assert(atomic_load(&test->bos));
  assert(!pthread_equal(pthread_self(), test->app_thread));
  assert(!pthread_equal(pthread_self(), test->network_thread));
  if (test->mode == 13 || test->mode == 14)
    return H2_PAL_ERR_WOULD_BLOCK;
  if (test->mode == 2 && test->read_offset != 0)
    return H2_PAL_ERR_CLOSED;
  const bool multi_turn = test->mode == 15 || test->mode == 16;
  /* Mode 18 echoes more reply chunks than the hook ring holds (8 x 1280 B =
   * 16 chunks) so a stalled hook consumer is actually exercised. */
  const size_t total = multi_turn        ? 4u * 640u
                       : test->mode == 18 ? 20u * 640u
                                          : 12u * 640u + 100u;
  if (multi_turn && test->read_offset == 2u * 640u &&
      atomic_load(&test->turns_done) == 0u)
    return H2_PAL_ERR_WOULD_BLOCK;
  if (test->read_offset == total)
    return H2_PAL_ERR_WOULD_BLOCK;
  *len = capacity < 100 ? capacity : 100;
  if (*len > total - test->read_offset)
    *len = total - test->read_offset;
  if (multi_turn && test->read_offset < 2u * 640u &&
      *len > 2u * 640u - test->read_offset)
    *len = 2u * 640u - test->read_offset;
  for (size_t i = 0; i < *len; ++i)
    pcm[i] = (uint8_t)((test->read_offset + i) % 127);
  test->read_offset += *len;
  atomic_store(&test->captured, test->read_offset);
  return H2_PAL_OK;
}

static h2_pal_result_t
conversation_test_track_write(void *user, const uint8_t *pcm, size_t len) {
  conversation_test_t *test = user;
  assert(!pthread_equal(pthread_self(), test->app_thread));
  assert(!pthread_equal(pthread_self(), test->network_thread));
  if (atomic_load(&test->playback_blocked))
    return H2_PAL_ERR_WOULD_BLOCK;
  assert(len && len <= 640 && len <= sizeof(test->output) - test->write_offset);
  memcpy(test->output + test->write_offset, pcm, len);
  test->write_offset += len;
  atomic_store(&test->written, test->write_offset);
  return H2_PAL_OK;
}

static h2_pal_result_t
conversation_test_hook(void *user, h2_gizclaw_conversation_t *conversation,
                       const h2_gizclaw_conversation_event_t *event) {
  conversation_test_t *test = user;
  assert(pthread_equal(pthread_self(), test->app_thread));
  if (test->mode == 20) {
    assert(event->kind != H2_GIZCLAW_CONVERSATION_EVENT_ERROR);
    if (event->kind == H2_GIZCLAW_CONVERSATION_EVENT_REPLY_AUDIO) {
      test->hook_offset += event->audio_len;
    } else if (event->kind == H2_GIZCLAW_CONVERSATION_EVENT_TEXT_DONE) {
      ++test->reply_text_ends;
    } else if (event->kind == H2_GIZCLAW_CONVERSATION_EVENT_REPLY_DONE) {
      unsigned turn = atomic_load(&test->turns_done) + 1u;
      assert(turn <= 2u);
      if (turn == 1u) {
        /* The interrupted reply: every first-burst chunk was decoded and
         * observed by the hook, but the speaker Track holds none of it. Both
         * discards ran (the Track tail at staging, and the frames that
         * drained behind the EOS marker at dispatch). */
        assert(test->hook_offset == 12u * 640u);
        uint8_t probe[2];
        assert(h2_gizclaw_pcm_track_read(test->owned_track, probe,
                                         sizeof(probe)) ==
               H2_PAL_ERR_WOULD_BLOCK);
      }
      atomic_store(&test->turns_done, turn);
    }
    return H2_PAL_OK;
  }
  if (test->mode == 15 || test->mode == 16) {
    if (event->kind == H2_GIZCLAW_CONVERSATION_EVENT_TEXT_DONE) {
      assert(
          event->text_len == 0u ||
          (event->text_len == 5u && (memcmp(event->text, "reply", 5u) == 0 ||
                                     memcmp(event->text, "heard", 5u) == 0)));
      if (event->text_len == 5u && memcmp(event->text, "heard", 5u) == 0)
        ++test->transcript_text_ends;
      else
        ++test->reply_text_ends;
    }
    if (event->kind == H2_GIZCLAW_CONVERSATION_EVENT_REPLY_DONE) {
      unsigned turn = atomic_load(&test->turns_done) + 1u;
      assert(turn <= 2u && test->hook_offset == turn * 1280u);
      assert(atomic_load(&test->written) == test->hook_offset);
      atomic_store(&test->turns_done, turn);
    }
  }
  if (test->mode == 13 || test->mode == 14) {
    assert(event->kind == H2_GIZCLAW_CONVERSATION_EVENT_TEXT_DELTA);
    assert(event->text_len == strlen("borrowed-wire-text"));
    if (test->mode == 13) {
      assert(h2_gizclaw_conversation_cancel(conversation) == H2_PAL_OK);
      conversation_test_probe(test);
      for (unsigned i = 0;
           i < 1000 && atomic_load(&test->service->media_request) != NULL; ++i)
        h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 1);
    } else {
      assert(h2_gizclaw_service_stop(test->service) == H2_PAL_OK);
    }
    assert(atomic_load(&test->service->media_request) == NULL);
    /* The network has freed its wire state while this hook is still running. */
    assert(memcmp(event->text, "borrowed-wire-text", event->text_len) == 0);
  }
  if (event->kind == H2_GIZCLAW_CONVERSATION_EVENT_REPLY_AUDIO) {
    assert(event->audio_len <= atomic_load(&test->written) - test->hook_offset);
    assert(memcmp(event->audio, test->output + test->hook_offset,
                  event->audio_len) == 0);
    test->hook_offset += event->audio_len;
    if (test->mode == 8)
      return H2_PAL_ERR_IO;
  }
  return H2_PAL_OK;
}

static void
conversation_test_complete(void *user, h2_gizclaw_conversation_t *conversation,
                           const h2_gizclaw_operation_result_t *result) {
  (void)conversation;
  conversation_test_t *test = user;
  assert(pthread_equal(pthread_self(), test->app_thread));
  test->result = result->result;
  atomic_store(&test->done, true);
}

static int conversation_test_start_task(void *user,
                                        const h2_pal_task_options_t *options,
                                        h2_pal_task_entry_t entry, void *ctx,
                                        h2_pal_task_t **out) {
  conversation_test_t *test = user;
  atomic_fetch_add(&test->starts, 1);
  return h2_pal_task_start(h2_desktop_platform_task_api(), options, entry, ctx,
                           out);
}
static int conversation_test_join_task(void *user, h2_pal_task_t *task) {
  conversation_test_t *test = user;
  atomic_fetch_add(&test->joins, 1);
  return h2_pal_task_join(h2_desktop_platform_task_api(), task);
}

static void test_conversation_reply_route_ids(void) {
  const size_t lengths[] = {1u, 63u, 64u, 127u, 128u};
  const char *labels[] = {"", "assistant", "transcript"};
  for (size_t label = 0; label < 3u; ++label) {
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
      test_env_t env;
      h2_gizclaw_service_t *service = create_service(&env, 2u);
      h2_gizclaw_conversation_t *conversation = NULL;
      assert(h2_gizclaw_conversation_create(
                 service, (h2_gizclaw_str_t){"workspace", 9u}, NULL, NULL, NULL,
                 &conversation) == H2_PAL_OK);
      gzc_peer_event_t event = {0};
      event.type = gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_BOS;
      snprintf(event.payload.bos.label, sizeof(event.payload.bos.label), "%s",
               labels[label]);
      // Empty and non-terminated wire IDs must not pin a route.
      assert(!h2_gizclaw_conversation_accepts_peer_event_internal(conversation,
                                                                  &event));
      memset(event.payload.bos.stream_id, 'x',
             sizeof(event.payload.bos.stream_id));
      assert(!h2_gizclaw_conversation_accepts_peer_event_internal(conversation,
                                                                  &event));
      const size_t length = lengths[i];
      assert(length < sizeof(event.payload.bos.stream_id));
      memset(event.payload.bos.stream_id, 'a', length);
      event.payload.bos.stream_id[length] = '\0';
      event.payload.bos.kind = gizclaw_events_v1_StreamKind_STREAM_KIND_AUDIO;
      assert(h2_gizclaw_conversation_accepts_peer_event_internal(conversation,
                                                                 &event));
      char id[sizeof(event.payload.bos.stream_id)];
      memcpy(id, event.payload.bos.stream_id, length + 1u);
      memset(&event, 0, sizeof(event));
      event.type = gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_TEXT_DELTA;
      snprintf(event.payload.text_delta.label,
               sizeof(event.payload.text_delta.label), "%s", labels[label]);
      memcpy(event.payload.text_delta.stream_id, id, length + 1u);
      assert(h2_gizclaw_conversation_accepts_peer_event_internal(conversation,
                                                                 &event));
      memset(&event, 0, sizeof(event));
      event.type = gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_EOS;
      snprintf(event.payload.eos.label, sizeof(event.payload.eos.label), "%s",
               labels[label]);
      memcpy(event.payload.eos.stream_id, id, length + 1u);
      // IDs which differ only at the final byte must remain distinct.
      event.payload.eos.stream_id[length - 1u] = 'b';
      assert(!h2_gizclaw_conversation_accepts_peer_event_internal(conversation,
                                                                  &event));
      event.payload.eos.stream_id[length - 1u] = 'a';
      assert(h2_gizclaw_conversation_accepts_peer_event_internal(conversation,
                                                                 &event));
      assert(!h2_gizclaw_conversation_accepts_peer_event_internal(conversation,
                                                                  &event));
      memset(&event, 0, sizeof(event));
      event.type = gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_TEXT_DONE;
      snprintf(event.payload.text_done.label,
               sizeof(event.payload.text_done.label), "%s", labels[label]);
      memcpy(event.payload.text_done.stream_id, id, length + 1u);
      assert(h2_gizclaw_conversation_accepts_peer_event_internal(conversation,
                                                                 &event));
      assert(!h2_gizclaw_conversation_accepts_peer_event_internal(conversation,
                                                                  &event));
      // A distinct BOS may start the next VAD turn after the completed reply.
      memset(&event, 0, sizeof(event));
      event.type = gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_BOS;
      snprintf(event.payload.bos.label, sizeof(event.payload.bos.label), "%s",
               labels[label]);
      memcpy(event.payload.bos.stream_id, id, length + 1u);
      assert(!h2_gizclaw_conversation_accepts_peer_event_internal(conversation,
                                                                  &event));
      event.payload.bos.stream_id[length - 1u] = 'b';
      assert(h2_gizclaw_conversation_accepts_peer_event_internal(conversation,
                                                                 &event));
      h2_gizclaw_conversation_release(conversation);
      assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
      assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
    }
  }
}

static gzc_peer_event_t barge_in_event(int type, const char *label,
                                       const char *id, const char *code) {
  gzc_peer_event_t event = {0};
  event.type = type;
  if (type == gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_BOS) {
    snprintf(event.payload.bos.label, sizeof(event.payload.bos.label), "%s",
             label);
    snprintf(event.payload.bos.stream_id, sizeof(event.payload.bos.stream_id),
             "%s", id);
    event.payload.bos.kind = gizclaw_events_v1_StreamKind_STREAM_KIND_TEXT;
  } else if (type ==
             gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_TEXT_DELTA) {
    snprintf(event.payload.text_delta.label,
             sizeof(event.payload.text_delta.label), "%s", label);
    snprintf(event.payload.text_delta.stream_id,
             sizeof(event.payload.text_delta.stream_id), "%s", id);
  } else if (type ==
             gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_TEXT_DONE) {
    snprintf(event.payload.text_done.label,
             sizeof(event.payload.text_done.label), "%s", label);
    snprintf(event.payload.text_done.stream_id,
             sizeof(event.payload.text_done.stream_id), "%s", id);
  } else {
    snprintf(event.payload.eos.label, sizeof(event.payload.eos.label), "%s",
             label);
    snprintf(event.payload.eos.stream_id, sizeof(event.payload.eos.stream_id),
             "%s", id);
    if (code != NULL) {
      event.payload.eos.has_error = true;
      snprintf(event.payload.eos.error.code,
               sizeof(event.payload.eos.error.code), "%s", code);
    }
  }
  return event;
}

/* Server-side barge-in: the next reply's BOS arrives while the previous
 * assistant route is still open. It must pin the new route instead of being
 * dropped, and the old reply's late EOS must no longer reach the app. */
static void test_conversation_barge_in_supersedes_open_reply(void) {
  static const int BOS = gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_BOS;
  static const int DELTA =
      gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_TEXT_DELTA;
  static const int DONE =
      gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_TEXT_DONE;
  static const int EOS = gizclaw_events_v1_PeerEventType_PEER_EVENT_TYPE_EOS;
  const char *labels[] = {"assistant", ""};
  for (size_t label = 0; label < 2u; ++label) {
    test_env_t env;
    h2_gizclaw_service_t *service = create_service(&env, 2u);
    h2_gizclaw_conversation_t *conversation = NULL;
    assert(h2_gizclaw_conversation_create(
               service, (h2_gizclaw_str_t){"workspace", 9u}, NULL, NULL, NULL,
               &conversation) == H2_PAL_OK);
    const char *reply = labels[label];
    gzc_peer_event_t event = barge_in_event(BOS, reply, "reply-1", NULL);
    assert(h2_gizclaw_conversation_accepts_peer_event_internal(conversation,
                                                               &event));
    event = barge_in_event(DELTA, reply, "reply-1", NULL);
    assert(h2_gizclaw_conversation_accepts_peer_event_internal(conversation,
                                                               &event));
    event = barge_in_event(DONE, reply, "reply-1", NULL);
    assert(h2_gizclaw_conversation_accepts_peer_event_internal(conversation,
                                                               &event));
    // The user speaks again; the transcript route is independent.
    event = barge_in_event(BOS, "transcript", "heard-1", NULL);
    assert(h2_gizclaw_conversation_accepts_peer_event_internal(conversation,
                                                               &event));
    // An open transcript is not superseded by another transcript BOS.
    event = barge_in_event(BOS, "transcript", "heard-2", NULL);
    assert(!h2_gizclaw_conversation_accepts_peer_event_internal(conversation,
                                                                &event));
    // A sub-stream of the same reply is not a new reply.
    event = barge_in_event(BOS, reply, "reply-1:audio", NULL);
    assert(h2_gizclaw_conversation_accepts_peer_event_internal(conversation,
                                                               &event));
    assert(!h2_gizclaw_conversation_wire_take_reply_interrupted_internal(
        conversation));
    // The next reply starts before reply-1 ended: reply-1 is interrupted.
    event = barge_in_event(BOS, reply, "reply-2", NULL);
    assert(h2_gizclaw_conversation_accepts_peer_event_internal(conversation,
                                                               &event));
    assert(h2_gizclaw_conversation_wire_take_reply_interrupted_internal(
        conversation));
    assert(!h2_gizclaw_conversation_wire_take_reply_interrupted_internal(
        conversation));
    // The old reply's late EOS and text no longer match the pinned route.
    event = barge_in_event(EOS, reply, "reply-1", "STREAM_INTERRUPTED");
    assert(!h2_gizclaw_conversation_accepts_peer_event_internal(conversation,
                                                                &event));
    event = barge_in_event(DONE, reply, "reply-1", NULL);
    assert(!h2_gizclaw_conversation_accepts_peer_event_internal(conversation,
                                                                &event));
    event = barge_in_event(DELTA, reply, "reply-2", NULL);
    assert(h2_gizclaw_conversation_accepts_peer_event_internal(conversation,
                                                               &event));
    event = barge_in_event(DONE, reply, "reply-2", NULL);
    assert(h2_gizclaw_conversation_accepts_peer_event_internal(conversation,
                                                               &event));
    event = barge_in_event(EOS, reply, "reply-2", NULL);
    assert(h2_gizclaw_conversation_accepts_peer_event_internal(conversation,
                                                               &event));
    assert(!h2_gizclaw_conversation_wire_take_reply_interrupted_internal(
        conversation));
    h2_gizclaw_conversation_release(conversation);
    assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
    assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
  }
}

static void
assert_conversation_blocks_rpc_audio(h2_gizclaw_service_t *service) {
  void *route = atomic_load(&service->media_request);
  assert(route != NULL);
  const h2_gizclaw_speech_transcribe_options_t asr = {
      .model_name = {"asr", 3}, .content_type = {"audio/pcm", 9}};
  const h2_gizclaw_speech_extract_options_t extract = {
      .asr_model_name = {"asr", 3},
      .extract_model_name = {"llm", 3},
      .content_type = {"audio/pcm", 9},
      .schema_json = {"{}", 2}};
  for (unsigned kind = 0; kind < 3; ++kind) {
    h2_gizclaw_req_t *request = NULL;
    h2_pal_result_t rc =
        kind == 0 ? h2_gizclaw_req_create_speech_transcribe(service, 100, &asr,
                                                            2000, &request)
        : kind == 1 ? h2_gizclaw_req_create_speech_extract(
                          service, 101, &extract, 2000, &request)
                    : h2_gizclaw_req_create_audio_play(
                          service, 102, (h2_gizclaw_str_t){"test", 4},
                          (h2_gizclaw_str_t){"history", 7}, 2000, &request);
    assert(rc == H2_PAL_OK);
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) ==
           H2_PAL_ERR_BUSY);
    assert(h2_gizclaw_req_wait(request, 0) == H2_PAL_ERR_INVALID_STATE);
    assert(h2_gizclaw_req_cancel(request) == H2_PAL_OK);
    h2_gizclaw_req_release(request);
    assert(atomic_load(&service->media_request) == route);
    assert(atomic_load(&service->speech_request) == NULL);
    assert(h2_pal_mutex_lock(service->config.sync, service->mutex) ==
           H2_PAL_OK);
    assert(service->audio_play == NULL);
    assert(h2_pal_mutex_unlock(service->config.sync, service->mutex) ==
           H2_PAL_OK);
  }
}

static void test_conversation_public_audio_tasks(void) {
  for (unsigned mode = 0; mode < 21; ++mode) {
    test_env_t env;
    h2_gizclaw_service_t *service = create_service(&env, 8);
    conversation_test_t test = {.service = service,
                                .env = &env,
                                .app_thread = pthread_self(),
                                .mode = mode,
                                .playback_blocked = mode == 0};
    s_conversation = &test;
    static const h2_gizclaw_service_client_ops_t ops = {
        .connect = conversation_test_connect, .poll = conversation_test_poll};
    h2_gizclaw_service_test_set_client_ops(&ops);
    h2_gizclaw_test_set_event_ops(conversation_test_send,
                                  conversation_test_read_event,
                                  conversation_test_close_event, &test);
    h2_gizclaw_test_set_packet_read(conversation_test_read_packet, &test);
    static const h2_pal_http_api_t http = {0};
    static const h2_pal_crypto_api_t crypto = {0};
    static const h2_pal_webrtc_api_t webrtc = {0};
    service->client_config.http = &http;
    service->client_config.crypto = &crypto;
    service->client_config.webrtc = &webrtc;
    service->client_config.time = h2_desktop_platform_time_api();
    service->client_config.connect_timeout_ms = mode == 5 ? 20 : 1000;
    service->client_config.server_endpoint =
        (h2_gizclaw_str_t){"127.0.0.1:1", 11};
    service->client_config.private_key = (h2_gizclaw_str_t){"test-key", 8};
    service->config.client_config = &service->client_config;
    service->config.on_event = NULL;
    service->config.prepare = NULL;
    service->config.cleanup = NULL;
    service->config.terminal = NULL;
    const h2_pal_task_vtable_t task_vt = {.start = conversation_test_start_task,
                                          .join = conversation_test_join_task};
    const h2_pal_task_api_t task_api = {.user = &test, .vtable = &task_vt};
    service->config.task = &task_api;
    const h2_gizclaw_track_vtable_t track_vt = {
        .read = conversation_test_track_read,
        .write = conversation_test_track_write};
    h2_gizclaw_track_t track = {.user = &test, .vtable = &track_vt};
    h2_gizclaw_track_t *owned_track = NULL;
    if (mode == 17u || mode == 20u) {
      const h2_gizclaw_pcm_track_config_t config = {
          .allocator = service->client_config.allocator,
          .uplink_capacity = 8192u,
          .downlink_capacity = 16384u};
      assert(h2_gizclaw_pcm_track_create(&config, &owned_track) == H2_PAL_OK);
      test.owned_track = owned_track;
    }
    if (mode == 17u) {
      const uint8_t stale[2] = {0xff, 0xee};
      assert(h2_gizclaw_pcm_track_write(owned_track, stale, sizeof(stale)) ==
             H2_PAL_OK);
    }
    assert(h2_gizclaw_service_set_track(service, owned_track != NULL
                                                     ? owned_track
                                                     : &track) == H2_PAL_OK);
    assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
    h2_gizclaw_conversation_t *conversation;
    assert(h2_gizclaw_conversation_create(
               service, (h2_gizclaw_str_t){"test", 4},
               mode == 3 || mode == 17 ? NULL : conversation_test_hook,
               conversation_test_complete, &test, &conversation) == H2_PAL_OK);
    assert(h2_gizclaw_service_audio_start(service) == H2_PAL_OK);
    if (mode == 10) {
      for (unsigned i = 0; i < 8; ++i)
        assert(h2_gizclaw_service_post_internal(
                   service, conversation_test_filler, &test) == H2_PAL_OK);
      assert(h2_gizclaw_service_post_internal(service, conversation_test_filler,
                                              &test) == H2_PAL_ERR_WOULD_BLOCK);
    }
    h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 5);
    assert(atomic_load(&test.captured) == 0 && atomic_load(&test.starts) == 5);
    if (mode == 17u) {
      uint8_t pcm[12u * 640u + 100u];
      for (size_t i = 0u; i < sizeof(pcm); ++i)
        pcm[i] = (uint8_t)(i % 127u);
      assert(h2_gizclaw_pcm_track_write(owned_track, pcm, sizeof(pcm)) ==
             H2_PAL_OK);
      assert(h2_gizclaw_service_audio_end(service) == H2_PAL_OK);
      const uint8_t later[2] = {0x32, 0x71};
      assert(h2_gizclaw_pcm_track_write(owned_track, later, sizeof(later)) ==
             H2_PAL_OK);
      assert(h2_gizclaw_service_audio_end(service) == H2_PAL_OK);
    }
    if (mode == 20u) {
      /* First burst: twelve whole frames echoed back as the first reply's
       * audio. The input stays open (realtime). */
      uint8_t pcm[12u * 640u];
      for (size_t i = 0u; i < sizeof(pcm); ++i)
        pcm[i] = (uint8_t)(i % 127u);
      assert(h2_gizclaw_pcm_track_write(owned_track, pcm, sizeof(pcm)) ==
             H2_PAL_OK);
    }
    atomic_store(&test.connect_gate, true);
    if (mode == 0) {
      for (unsigned spins = 0; spins < 2000 && !atomic_load(&test.bos); ++spins)
        h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 1);
      assert(atomic_load(&test.bos));
      assert_conversation_blocks_rpc_audio(service);
    }
    if (mode >= 9 && mode <= 12) {
      unsigned spins = 0;
      while ((!conversation_test_notification_count(&test) ||
              !atomic_load(&test.written)) &&
             spins++ < 2000)
        h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 1);
      assert(spins < 2000);
      assert(test.hook_offset == 0 && test.filler_callbacks == 0);
      conversation_test_probe(&test); /* No application poll has run yet. */
      assert(conversation_test_notification_count(&test) ==
             (mode == 10 ? 8 : 1));
      if (mode == 9) {
        for (spins = 0;
             spins < 2000 && atomic_load(&test.captured) != 12 * 640 + 100;
             ++spins)
          h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 1);
        assert(atomic_load(&test.captured) == 12 * 640 + 100);
        assert(h2_gizclaw_service_audio_end(service) == H2_PAL_OK);
        for (spins = 0; spins < 2000 && !atomic_load(&test.eos); ++spins)
          h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 1);
        assert(atomic_load(&test.eos) && test.hook_offset == 0);
      }
      if (mode == 11) {
        assert(h2_gizclaw_conversation_cancel(conversation) == H2_PAL_OK);
        for (spins = 0;
             spins < 2000 && atomic_load(&service->media_request) != NULL;
             ++spins)
          h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 1);
        assert(atomic_load(&service->media_request) == NULL);
      } else if (mode == 12) {
        assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
        assert(atomic_load(&service->media_request) == NULL);
      }
    }
    bool input_ended = false;
    unsigned hook_settle = 0u;
    for (unsigned spins = 0; spins < 4000 && !atomic_load(&test.done);
         ++spins) {
      if (mode == 15 && atomic_load(&test.turns_done) == 1u &&
          atomic_load(&test.captured) == 2560u && !input_ended) {
        assert(h2_gizclaw_service_audio_end(service) == H2_PAL_OK);
        input_ended = true;
      } else if (mode == 16 && atomic_load(&test.turns_done) == 2u &&
                 !input_ended) {
        /* Both reply EOS events have already arrived. Hangup must finish
         * without sending a normal input end and awaiting another reply. */
        assert(!atomic_load(&test.eos) && !atomic_load(&test.done));
        conversation_test_probe(&test);
        assert(h2_gizclaw_conversation_cancel(conversation) == H2_PAL_OK);
        input_ended = true;
      } else if (mode == 1 && atomic_load(&test.captured) > 0 && !input_ended) {
        assert(h2_gizclaw_conversation_cancel(conversation) == H2_PAL_OK);
        assert(h2_gizclaw_conversation_cancel(conversation) == H2_PAL_OK);
        input_ended = true;
      } else if ((mode == 0 || mode == 3 || mode == 4 ||
                  (mode >= 6 && mode <= 10) || mode == 19) &&
                 atomic_load(&test.captured) == 12 * 640 + 100 &&
                 !input_ended) {
        assert(h2_gizclaw_service_audio_end(service) == H2_PAL_OK);
        assert(h2_gizclaw_service_audio_end(service) == H2_PAL_OK);
        input_ended = true;
      } else if (mode == 18 && atomic_load(&test.captured) == 20u * 640u &&
                 !input_ended) {
        assert(h2_gizclaw_service_audio_end(service) == H2_PAL_OK);
        input_ended = true;
      } else if (mode == 20 && atomic_load(&test.turns_done) == 1u &&
                 !input_ended) {
        /* The interrupted boundary was non-terminal: the second burst is
         * the next reply's audio, then the input ends (push-to-talk commit)
         * so its EOS becomes the terminal boundary. */
        uint8_t pcm[4u * 640u];
        for (size_t i = 0u; i < sizeof(pcm); ++i)
          pcm[i] = (uint8_t)(i % 127u);
        assert(h2_gizclaw_pcm_track_write(owned_track, pcm, sizeof(pcm)) ==
               H2_PAL_OK);
        assert(h2_gizclaw_service_audio_end(service) == H2_PAL_OK);
        input_ended = true;
      }
      /* Release the fake speaker after either encoded-ring backpressure or
       * input completion. Production downlink capacity may absorb this small
       * fixture completely; fixed-ring saturation is covered separately. */
      if (mode == 0 &&
          (atomic_load(&test.echo_blocked) || input_ended))
        atomic_store(&test.playback_blocked, false);
      /* Mode 18: the app stops polling, so the hook ring fills after 16
       * chunks. Decoding must still deliver every chunk to the speaker Track;
       * a decoder gated on the hook would leave `written` short forever. */
      if (mode == 18 &&
          (atomic_load(&test.written) < 20u * 640u || hook_settle++ < 50u)) {
        /* `written` is stored by the Track write that precedes the hook ring
         * write for the same chunk. Let the decoder finish that last hook
         * write, and the network tick stage its one chunk, before the first
         * poll drains the ring, so the count below is exactly what fit. */
        h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 1);
        continue;
      }
      size_t dispatched;
      assert(h2_gizclaw_service_poll(service, 32, &dispatched) == H2_PAL_OK);
      h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 1);
    }
    assert(atomic_load(&test.done));
    assert(test.result ==
           ((mode == 1 || mode == 2 || (mode >= 11 && mode <= 14) || mode == 16)
                ? H2_PAL_ERR_CLOSED
            : mode == 5              ? H2_PAL_ERR_TIMEOUT
            : mode == 6 || mode == 8 ? H2_PAL_ERR_IO
                                     : H2_PAL_OK));
    assert(test.event_close_count == (mode == 12 || mode == 14 ? 1u : 0u));
    assert(service->stopping == (mode == 12 || mode == 14));
    assert(atomic_load(&service->media_request) == NULL);
    if (mode == 16) {
      assert(atomic_load(&test.canceled));
      conversation_test_probe(&test); /* Hangup leaves the Peer usable. */
    }
    if (mode == 17u) {
      /* Completion does not wait for the external speaker. All decoded PCM
       * is still buffered; the later mic sample did not enter this request. */
      assert(test.packets == 13u && atomic_load(&test.written) == 0u);
      assert(h2_gizclaw_pcm_track_read(owned_track, test.output, 13u * 640u) ==
             H2_PAL_OK);
      test.write_offset = 13u * 640u;
      atomic_store(&test.written, test.write_offset);
      uint8_t remaining[2];
      size_t len = 0u;
      assert(h2_gizclaw_service_pcm_read_internal(service, remaining, 2u,
                                                  &len) == H2_PAL_OK);
      assert(len == 2u && remaining[0] == 0x32 && remaining[1] == 0x71);
      assert(h2_gizclaw_pcm_track_read(owned_track, remaining, 2u) ==
             H2_PAL_ERR_WOULD_BLOCK);
    }
    if (mode == 15 || mode == 16) {
      assert(atomic_load(&test.turns_done) == 2u && test.reply_events == 15u);
      assert(test.reply_text_ends == 2u && test.transcript_text_ends == 2u);
      assert(test.packets == 4u && atomic_load(&test.written) == 2560u);
      assert(test.hook_offset == 2560u && test.bos_attempts == 1u);
    }
    if (mode == 0 || mode == 3 || mode == 4 || mode == 6 || mode == 7 ||
        mode == 9 || mode == 10 || mode == 17) {
      assert(test.packets == 13 && atomic_load(&test.written) == 13 * 640);
      assert(test.hook_offset == (mode == 3 || mode == 17 ? 0 : 13 * 640));
      size_t nonzero = 0;
      for (size_t i = 0; i < test.write_offset; ++i)
        nonzero += test.output[i] != 0;
      assert(nonzero > 0);
    }
    if (mode == 4)
      assert(test.bos_attempts == 4 && test.eos_attempts == 4 &&
             atomic_load(&test.small_buffer_rejected));
    if (mode == 7)
      assert(test.reply_events == 2);
    if (mode == 5)
      assert(test.bos_attempts > 1 && atomic_load(&test.captured) == 0 &&
             !atomic_load(&test.bos) && !atomic_load(&test.eos));
    if (mode == 10)
      assert(test.filler_callbacks == 8);
    if (mode == 18) {
      /* Every chunk reached the speaker Track with no app poll in between;
       * decoding never waited on the hook. The hook kept only the contiguous
       * prefix that fit: sixteen chunks in its ring plus the one chunk the
       * network tick had already staged for dispatch. The rest coalesced. */
      assert(test.packets == 20u && atomic_load(&test.written) == 20u * 640u);
      assert(test.hook_offset == 17u * 640u);
      assert(test.result == H2_PAL_OK);
    }
    if (mode == 19) {
      /* One loss marker between the first and second packet: the decoder
       * concealed it as one 20 ms frame, every later packet still played,
       * and the conversation completed normally instead of failing. */
      assert(test.loss_markers == 1u && test.packets == 13u);
      assert(atomic_load(&test.written) == 14u * 640u);
      assert(test.hook_offset == 14u * 640u);
      assert(test.result == H2_PAL_OK);
    }
    if (mode == 11 || mode == 12)
      assert(test.hook_offset == 0);
    if (mode == 20) {
      /* Both replies reached the hook; the speaker Track holds exactly the
       * second reply, the interrupted first reply was discarded. */
      assert(atomic_load(&test.turns_done) == 2u && test.reply_events == 6u);
      assert(test.reply_text_ends == 1u && test.packets == 16u);
      assert(test.hook_offset == 16u * 640u && test.result == H2_PAL_OK);
      assert(h2_gizclaw_pcm_track_read(owned_track, test.output, 4u * 640u) ==
             H2_PAL_OK);
      assert(h2_gizclaw_pcm_track_read(owned_track, test.output, 2u) ==
             H2_PAL_ERR_WOULD_BLOCK);
    }
    assert(conversation_test_notification_count(&test) == 0);
    size_t captured = atomic_load(&test.captured),
           written = atomic_load(&test.written);
    h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 5);
    assert(captured == atomic_load(&test.captured) &&
           written == atomic_load(&test.written));
    assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
    assert(h2_gizclaw_service_deinit(service) == H2_PAL_ERR_INVALID_STATE);
    h2_gizclaw_conversation_release(conversation);
    assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
    assert(h2_gizclaw_pcm_track_destroy(&owned_track) == H2_PAL_OK);
    assert(atomic_load(&test.starts) == 5 && atomic_load(&test.joins) == 5);
    assert(test.event_close_count == 1);
    h2_gizclaw_test_set_event_ops(NULL, NULL, NULL, NULL);
    h2_gizclaw_test_set_packet_read(NULL, NULL);
  }
}

typedef struct speed_wire_test {
  unsigned mode, starts, writes, finishes, next, destroys, cancels;
  size_t upload, download, accepted, input_produced, output_consumed;
  uint64_t upload_started, download_started;
  uint32_t previous_timeout;
  h2_gizclaw_rpc_stream_fn receive;
  void *receive_user;
  uint8_t metadata[gizclaw_rpc_v1_SpeedTestResponse_size];
  size_t metadata_len;
} speed_wire_test_t;

static speed_wire_test_t *s_speed_wire;

static h2_pal_result_t speed_test_input(void *user, uint8_t *buffer,
                                        size_t capacity, size_t *out_read) {
  speed_wire_test_t *wire = user;
  (void)buffer;
  assert(wire != NULL && out_read != NULL);
  const size_t remaining = wire->upload - wire->input_produced;
  *out_read = remaining < capacity ? remaining : capacity;
  wire->input_produced += *out_read;
  return H2_PAL_OK;
}

static h2_pal_result_t speed_test_output(void *user, const uint8_t *data,
                                         size_t length, size_t *out_written) {
  speed_wire_test_t *wire = user;
  assert(wire != NULL && out_written != NULL);
  if (length != 0u)
    assert(data != NULL);
  wire->output_consumed += length;
  *out_written = length;
  return H2_PAL_OK;
}

static int speed_wire_start(h2_gizclaw_client_t *client,
                            h2_gizclaw_rpc_method_t method,
                            h2_gizclaw_rpc_bytes_t payload, uint32_t timeout,
                            h2_gizclaw_rpc_stream_fn receive, void *user,
                            h2_gizclaw_rpc_request_t **out) {
  (void)client;
  speed_wire_test_t *wire = s_speed_wire;
  assert(method == H2_GIZCLAW_RPC_ALL_SPEED_TEST_RUN);
  assert(timeout > 0u && timeout <= wire->previous_timeout);
  wire->previous_timeout = timeout;
  gizclaw_rpc_v1_SpeedTestRequest params =
      gizclaw_rpc_v1_SpeedTestRequest_init_zero;
  pb_istream_t input = pb_istream_from_buffer(payload.data, payload.len);
  assert(pb_decode(&input, gizclaw_rpc_v1_SpeedTestRequest_fields, &params));
  assert(params.up_content_length == (int64_t)wire->upload &&
         params.down_content_length == (int64_t)wire->download);
  *out = NULL;
  ++wire->starts;
  if (wire->mode != 10u)
    atomic_fetch_add(&s_env->clock_ms, 10u);
  if (wire->mode == 8u || (wire->mode == 3u && wire->starts < 4u))
    return H2_PAL_ERR_WOULD_BLOCK;
  if (wire->mode == 13u)
    return H2_PAL_ERR_IO;
  wire->upload_started = atomic_load(&s_env->clock_ms);
  wire->receive = receive;
  wire->receive_user = user;
  *out = (h2_gizclaw_rpc_request_t *)wire;
  return H2_PAL_OK;
}

static int speed_wire_write(h2_gizclaw_rpc_request_t *request,
                            const uint8_t *data, size_t len) {
  speed_wire_test_t *wire = (speed_wire_test_t *)request;
  assert(wire == s_speed_wire && len > 0u &&
         len <= H2_GIZCLAW_STREAM_INPUT_BYTES);
  ++wire->writes;
  if (wire->mode != 10u)
    atomic_fetch_add(&s_env->clock_ms, 10u);
  for (size_t i = 0u; i < len; ++i)
    assert(data[i] == 0u);
  if (wire->mode == 4u && wire->writes < 4u)
    return H2_PAL_ERR_WOULD_BLOCK;
  assert(len <= wire->upload - wire->accepted);
  wire->accepted += len;
  return H2_PAL_OK;
}

static int speed_wire_finish(h2_gizclaw_rpc_request_t *request) {
  speed_wire_test_t *wire = (speed_wire_test_t *)request;
  assert(wire == s_speed_wire && wire->accepted == wire->upload);
  if (wire->mode != 10u)
    atomic_fetch_add(&s_env->clock_ms, 10u);
  return wire->mode == 4u && ++wire->finishes < 4u ? H2_PAL_ERR_WOULD_BLOCK
                                                   : H2_PAL_OK;
}

static int speed_wire_result(h2_gizclaw_rpc_request_t *request,
                             h2_gizclaw_rpc_response_t *out) {
  speed_wire_test_t *wire = (speed_wire_test_t *)request;
  assert(wire == s_speed_wire);
  memset(out, 0, sizeof(*out));
  h2_gizclaw_rpc_stream_event_t event = {0};
  uint8_t bytes[258];
  for (size_t i = 0u; i < sizeof(bytes); ++i)
    bytes[i] = (uint8_t)i;
  if (wire->next == 0u) {
    event.kind = H2_GIZCLAW_RPC_STREAM_RESPONSE;
    event.result_payload =
        (h2_gizclaw_rpc_bytes_t){wire->metadata, wire->metadata_len};
    event.has_error = wire->mode == 7u;
    wire->download_started = atomic_load(&s_env->clock_ms);
  } else if (wire->next == 1u && wire->download > 0u) {
    event.kind = H2_GIZCLAW_RPC_STREAM_DATA;
    event.data = (h2_gizclaw_rpc_bytes_t){
        bytes, wire->download + (wire->mode == 6u ? 1u : 0u) -
                   (wire->mode == 12u ? 1u : 0u)};
    if (wire->mode == 15u)
      bytes[0] ^= 1u;
    if (wire->mode == 16u)
      bytes[128] ^= 1u;
    if (wire->mode == 17u)
      bytes[256] ^= 1u;
    if (wire->mode == 18u)
      event.data.data = NULL;
    if (wire->mode == 21u) {
      const h2_gizclaw_rpc_stream_event_t empty = {
          .kind = H2_GIZCLAW_RPC_STREAM_DATA, .data = {NULL, 0u}};
      const int empty_rc = wire->receive(wire->receive_user, &empty);
      if (empty_rc != H2_PAL_OK)
        return empty_rc;
    }
    if (wire->mode == 19u || wire->mode == 20u) {
      // A split at a non-period boundary must retain the stream offset.
      event.data.len = 7u;
      const int prefix_rc = wire->receive(wire->receive_user, &event);
      if (prefix_rc != H2_PAL_OK)
        return prefix_rc;
      if (wire->mode == 20u)
        bytes[7] ^= 1u;
      event.data = (h2_gizclaw_rpc_bytes_t){bytes + 7u, wire->download - 7u};
    }
  } else if (wire->next <= 2u) {
    if (wire->mode == 5u)
      return H2_PAL_OK; /* Successful handle but no EOS is not success. */
    event.kind = H2_GIZCLAW_RPC_STREAM_EOS;
    if (wire->mode != 10u)
      atomic_store(&s_env->clock_ms, wire->mode == 11u ? 99u : 1000u);
    wire->next = 2u;
  } else {
    return H2_PAL_OK;
  }
  ++wire->next;
  const int rc = wire->receive(wire->receive_user, &event);
  return rc == H2_PAL_OK ? H2_PAL_ERR_WOULD_BLOCK : rc;
}

static void speed_wire_cancel(h2_gizclaw_rpc_request_t *request) {
  assert(request == (h2_gizclaw_rpc_request_t *)s_speed_wire);
  ++s_speed_wire->cancels;
}

static void speed_wire_destroy(h2_gizclaw_rpc_request_t *request) {
  assert(request == (h2_gizclaw_rpc_request_t *)s_speed_wire);
  ++s_speed_wire->destroys;
}

static const h2_gizclaw_async_rpc_ops_t speed_wire_ops = {
    .start_stream = speed_wire_start,
    .write = speed_wire_write,
    .finish_write = speed_wire_finish,
    .result = speed_wire_result,
    .cancel = speed_wire_cancel,
    .destroy = speed_wire_destroy,
};

static void test_speedtest_managed_requests(void) {
  static const h2_pal_time_vtable_t clock_vtable = {
      .get_monotonic_ms = fake_req_clock,
  };
  for (unsigned mode = 0u; mode < 22u; ++mode) {
    if (mode == 8u)
      continue; /* Request wait timeout is covered by lifecycle tests. */
    test_env_t env;
    h2_gizclaw_service_t *service = create_profile_service(&env);
    h2_pal_time_api_t time = {.user = &env, .vtable = &clock_vtable};
    service->client_config.time = &time;
    atomic_store(&env.clock_ms, 100u);
    const bool upload_only =
        mode == 0u || mode == 3u || mode == 4u || mode == 8u || mode == 10u;
    speed_wire_test_t wire = {
        .mode = mode,
        .upload = upload_only ? 4099u : 0u,
        .download = upload_only ? 0u : 257u,
        .previous_timeout = 1234u,
    };
    gizclaw_rpc_v1_SpeedTestResponse metadata =
        gizclaw_rpc_v1_SpeedTestResponse_init_zero;
    metadata.up_content_length = mode == 14u ? -1 : (int64_t)wire.upload;
    metadata.down_content_length =
        (int64_t)wire.download + (mode == 9u ? 1 : 0);
    pb_ostream_t output =
        pb_ostream_from_buffer(wire.metadata, sizeof(wire.metadata));
    assert(
        pb_encode(&output, gizclaw_rpc_v1_SpeedTestResponse_fields, &metadata));
    wire.metadata_len = output.bytes_written;
    s_speed_wire = &wire;
    h2_gizclaw_async_rpc_test_set_ops(&speed_wire_ops);
    assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
    h2_gizclaw_req_t *request = NULL;
    assert(h2_gizclaw_req_create_speedtest(service, 1u, wire.upload,
                                           wire.download, 1234u,
                                           &request) == H2_PAL_OK);
    assert(wire.starts == 0u);
    h2_gizclaw_speedtest_result_t result;
    assert(h2_gizclaw_resp_parse_speedtest(request, &result) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(h2_gizclaw_req_do(
               request, &wire,
               wire.upload != 0u && mode != 0u ? speed_test_input : NULL,
               wire.download != 0u && mode != 1u ? speed_test_output : NULL,
               NULL) == H2_PAL_OK);
    const int expected =
        (mode == 5u || mode == 13u) ? H2_PAL_ERR_IO
        : (mode == 6u || mode == 9u || mode == 12u || mode == 14u ||
           (mode >= 15u && mode <= 18u) || mode == 20u)
            ? H2_PAL_ERR_FORMAT
        : mode == 7u  ? H2_GIZCLAW_ERR_REMOTE
        : mode == 11u ? H2_PAL_ERR_INVALID_STATE
        : mode == 8u  ? H2_PAL_ERR_TIMEOUT
                      : H2_PAL_OK;
    const int wait_rc = h2_gizclaw_req_wait_dispatch_internal(request);
    if (wait_rc != expected)
      fprintf(stderr, "speedtest mode=%u expected=%d actual=%d\n", mode,
              expected, wait_rc);
    assert(wait_rc == expected);
    assert(h2_gizclaw_resp_parse_speedtest(request, &result) == expected);
    if (expected == H2_PAL_OK) {
      assert(result.upload_bytes == wire.upload &&
             result.download_bytes == wire.download);
      if (wire.upload > 0u) {
        assert(result.upload_elapsed_ms == (mode == 10u ? 1u : 900u));
        assert(result.upload_bits_per_second ==
               wire.upload * 8000u / result.upload_elapsed_ms);
        assert(mode == 10u || result.upload_elapsed_ms > 500u);
      } else {
        assert(result.upload_elapsed_ms == 0u &&
               result.upload_bits_per_second == 0u);
      }
      if (wire.download > 0u) {
        assert(result.elapsed_ms == (mode == 10u ? 1u : 900u));
        assert(result.download_bits_per_second ==
               wire.download * 8000u / result.elapsed_ms);
      } else {
        assert(result.elapsed_ms == 0u &&
               result.download_bits_per_second == 0u);
      }
    } else {
      assert(result.upload_bytes == 0u && result.download_bytes == 0u &&
             result.upload_elapsed_ms == 0u && result.elapsed_ms == 0u &&
             result.upload_bits_per_second == 0u &&
             result.download_bits_per_second == 0u);
    }
    h2_gizclaw_req_release(request);
    assert(wire.destroys == (mode == 8u || mode == 13u ? 0u : 1u));
    if (mode == 3u)
      assert(wire.starts == 4u && wire.accepted == wire.upload);
    if (mode == 4u)
      assert(wire.writes ==
                 3u + (wire.upload + H2_GIZCLAW_STREAM_INPUT_BYTES - 1u) /
                          H2_GIZCLAW_STREAM_INPUT_BYTES &&
             wire.finishes == 4u &&
             wire.accepted == wire.upload);
    {
      wire.starts = wire.writes = wire.finishes = wire.next = wire.accepted =
          0u;
      wire.input_produced = wire.output_consumed = 0u;
      wire.destroys = wire.cancels = 0u;
      wire.previous_timeout = 1234u;
      atomic_store(&env.clock_ms, 100u);
      const h2_gizclaw_speedtest_result_t request_result = result;
      memset(&result, 0xa5, sizeof(result));
      assert(h2_gizclaw_rpc_speedtest(service, wire.upload, wire.download,
                                      1234u, &result) == expected);
      assert(memcmp(&result, &request_result, sizeof(result)) == 0);
      assert(wire.destroys == (mode == 8u || mode == 13u ? 0u : 1u));
    }
    assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
    assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
  }
}

typedef struct stream_handoff_test {
  h2_gizclaw_rpc_stream_fn ingress;
  void *ingress_user;
  pthread_t app, owner, consumer;
  unsigned mode, next, frames;
  size_t upload, accepted, input_produced, output_consumed;
  const h2_pal_mem_api_t *base_allocator;
  h2_gizclaw_service_t *service;
  int stop_result;
  h2_pal_task_entry_t data_entry;
  void *data_ctx;
  int expected;
  atomic_uint entered, rejected, destroyed, callbacks, wire_destroyed, opened,
      writes;
  atomic_bool gate, task_gate, fail_alloc, stop_entered, stop_exited;
  atomic_uint sink_calls, sink_stops;
  bool hold_sink;
  unsigned hook_mode, hook_data_count, hook_seen;
  size_t hook_data_bytes;
  atomic_uint hook_consumed, hook_delivered;
  atomic_uint data_waits;
} stream_handoff_test_t;
static stream_handoff_test_t *s_handoff;
static _Thread_local stream_handoff_test_t *s_data_wait_test;
static const char handoff_tag;

static h2_pal_result_t handoff_input(void *user, uint8_t *buffer,
                                     size_t capacity, size_t *out_read) {
  stream_handoff_test_t *test = user;
  (void)buffer;
  assert(test != NULL && out_read != NULL);
  const size_t remaining = test->upload - test->input_produced;
  *out_read = remaining < capacity ? remaining : capacity;
  test->input_produced += *out_read;
  return H2_PAL_OK;
}

static h2_pal_result_t handoff_output(void *user, const uint8_t *data,
                                      size_t length, size_t *out_written) {
  stream_handoff_test_t *test = user;
  assert(test != NULL && out_written != NULL);
  for (size_t i = 0u; i < length; ++i)
    assert(data[i] == 0x5a);
  test->output_consumed += length;
  *out_written = length;
  return H2_PAL_OK;
}

static void *handoff_alloc(void *user, size_t len) {
  stream_handoff_test_t *test = user;
  if (atomic_exchange(&test->fail_alloc, false))
    return NULL;
  return h2_pal_mem_alloc(test->base_allocator, len);
}
static void *handoff_realloc(void *user, void *ptr, size_t len) {
  return h2_pal_mem_realloc(((stream_handoff_test_t *)user)->base_allocator,
                            ptr, len);
}
static void handoff_free(void *user, void *ptr) {
  h2_pal_mem_free(((stream_handoff_test_t *)user)->base_allocator, ptr);
}

static int handoff_open(h2_gizclaw_client_t *client,
                        h2_gizclaw_rpc_method_t method,
                        h2_gizclaw_rpc_bytes_t payload, uint32_t timeout,
                        h2_gizclaw_rpc_stream_fn receive, void *user,
                        h2_gizclaw_rpc_request_t **out) {
  (void)client;
  assert(method == H2_GIZCLAW_RPC_ALL_SPEED_TEST_RUN && payload.len <= 1u &&
         timeout);
  const unsigned index = payload.len == 0u ? 0u : payload.data[0];
  assert(index < 2u);
  stream_handoff_test_t *test = s_handoff + index;
  test->owner = pthread_self();
  assert(!pthread_equal(test->owner, test->app));
  test->ingress = receive;
  test->ingress_user = user;
  *out = (h2_gizclaw_rpc_request_t *)test;
  atomic_store(&test->opened, 1u);
  return H2_PAL_OK;
}
static int handoff_write(h2_gizclaw_rpc_request_t *request, const uint8_t *data,
                         size_t len) {
  stream_handoff_test_t *test = (stream_handoff_test_t *)request;
  assert(pthread_equal(pthread_self(), test->owner));
  assert(len != 0u && len <= H2_GIZCLAW_STREAM_INPUT_BYTES &&
         len <= test->upload - test->accepted);
  for (size_t i = 0u; i < len; ++i)
    assert(data[i] == 0u);
  test->accepted += len;
  atomic_fetch_add(&test->writes, 1u);
  return H2_PAL_OK;
}
static int handoff_finish(h2_gizclaw_rpc_request_t *request) {
  stream_handoff_test_t *test = (stream_handoff_test_t *)request;
  assert(pthread_equal(pthread_self(), test->owner));
  assert(test->accepted == test->upload);
  return H2_PAL_OK;
}
static int handoff_result(h2_gizclaw_rpc_request_t *request,
                          h2_gizclaw_rpc_response_t *out) {
  stream_handoff_test_t *test = (stream_handoff_test_t *)request;
  assert(pthread_equal(pthread_self(), test->owner));
  memset(out, 0, sizeof(*out));
  if (test->mode == 12u && !atomic_load(&test->gate))
    return H2_PAL_ERR_WOULD_BLOCK;
  uint8_t bytes[32768];
  memset(bytes, 0x5a, sizeof(bytes));
  h2_gizclaw_rpc_stream_event_t event = {0};
  if (test->next == 0u) {
    event.kind = H2_GIZCLAW_RPC_STREAM_RESPONSE;
    event.result_payload = (h2_gizclaw_rpc_bytes_t){bytes, 3u};
    event.error_message = (h2_gizclaw_rpc_bytes_t){bytes + 3u, 2u};
  } else if (test->next == 1u) {
    if (((test->mode >= 3u && test->mode <= 5u) || test->mode == 11u) &&
        atomic_load(&test->entered) == 0u)
      return H2_PAL_ERR_WOULD_BLOCK;
    if (test->mode == 3u || test->mode == 11u)
      return H2_PAL_ERR_WOULD_BLOCK; /* Canceled while consumer is in flight. */
    event.kind = H2_GIZCLAW_RPC_STREAM_DATA;
    event.data = (h2_gizclaw_rpc_bytes_t){bytes, 257u};
    if (test->mode == 6u)
      event.data.len = 128u * 1024u + 1u; /* Reject without dereferencing. */
    if (test->mode == 4u || test->mode == 5u) {
      event.data.len = test->mode == 4u ? 0u : sizeof(bytes);
      int rc = H2_PAL_OK;
      for (unsigned i = 0; i < 65u && rc == H2_PAL_OK; ++i)
        rc = test->ingress(test->ingress_user, &event);
      assert(rc == H2_PAL_ERR_NO_SPACE);
      atomic_store(&test->rejected, 1u);
      return rc;
    }
  } else if (test->next == 2u) {
    event.kind = H2_GIZCLAW_RPC_STREAM_EOS;
  } else {
    return H2_PAL_OK;
  }
  ++test->next;
  if (test->mode == 10u)
    atomic_store(&test->fail_alloc, true);
  const int rc = test->ingress(test->ingress_user, &event);
  /* Ingress must own every view, even when the lane task is delayed. */
  memset(bytes, 0xa5, sizeof(bytes));
  return rc == H2_PAL_OK ? H2_PAL_ERR_WOULD_BLOCK : rc;
}
static void handoff_cancel(h2_gizclaw_rpc_request_t *request) {
  stream_handoff_test_t *test = (stream_handoff_test_t *)request;
  assert(pthread_equal(pthread_self(), test->owner));
}
static void handoff_wire_destroy(h2_gizclaw_rpc_request_t *request) {
  handoff_cancel(request);
  atomic_fetch_add(&((stream_handoff_test_t *)request)->wire_destroyed, 1u);
}
static int handoff_consume(void *user,
                           const h2_gizclaw_rpc_stream_event_t *event) {
  stream_handoff_test_t *test = user;
  assert(!pthread_equal(pthread_self(), test->app));
  assert(!pthread_equal(pthread_self(), test->owner));
  if (test->frames == 0u)
    test->consumer = pthread_self();
  else
    assert(pthread_equal(pthread_self(), test->consumer));
  assert(atomic_load(&test->destroyed) == 0u);
  assert(event->input_finished && event->input_bytes == test->upload);
  if (test->frames++ == 0u) {
    assert(event->kind == H2_GIZCLAW_RPC_STREAM_RESPONSE);
    atomic_store(&test->entered, 1u);
    if ((test->mode >= 3u && test->mode <= 5u) || test->mode == 11u) {
      uint64_t start = 0u, now = 0u;
      assert(h2_pal_time_get_monotonic_ms(h2_desktop_platform_time_api(),
                                          &start) == H2_PAL_OK);
      while (!atomic_load(&test->gate)) {
        assert(h2_pal_time_get_monotonic_ms(h2_desktop_platform_time_api(),
                                            &now) == H2_PAL_OK);
        assert(now - start < 2000u);
        sched_yield();
      }
    }
    assert(event->result_payload.len == 3u && event->error_message.len == 2u);
    for (size_t i = 0u; i < 3u; ++i)
      assert(event->result_payload.data[i] == 0x5a);
    for (size_t i = 0u; i < 2u; ++i)
      assert(event->error_message.data[i] == 0x5a);
  } else if (event->kind == H2_GIZCLAW_RPC_STREAM_DATA) {
    assert(event->data.len == 257u);
    for (size_t i = 0; i < event->data.len; ++i)
      assert(event->data.data[i] == 0x5a);
    if (test->mode == 1u)
      return H2_PAL_ERR_FORMAT;
    if (test->mode == 2u)
      return H2_PAL_ERR_WOULD_BLOCK;
    if (test->mode == 7u)
      return 1;
  } else {
    assert(event->kind == H2_GIZCLAW_RPC_STREAM_EOS && test->frames == 3u);
  }
  return H2_PAL_OK;
}
static void handoff_destroy(void *user) {
  stream_handoff_test_t *test = user;
  atomic_fetch_add(&test->destroyed, 1u);
}
static void handoff_gated_data_task(void *user) {
  stream_handoff_test_t *test = user;
  uint64_t start = 0u, now = 0u;
  assert(h2_pal_time_get_monotonic_ms(h2_desktop_platform_time_api(), &start) ==
         H2_PAL_OK);
  while (!atomic_load(&test->task_gate)) {
    assert(h2_pal_time_get_monotonic_ms(h2_desktop_platform_time_api(), &now) ==
           H2_PAL_OK);
    assert(now - start < 2000u);
    sched_yield();
  }
  s_data_wait_test = test;
  test->data_entry(test->data_ctx);
  s_data_wait_test = NULL;
}
static h2_pal_result_t handoff_wait_cond(void *user, h2_pal_cond_t *cond,
                                         h2_pal_mutex_t *mutex,
                                         uint32_t timeout) {
  (void)user;
  if (s_data_wait_test != NULL) {
    assert(timeout == 1u);
    const h2_gizclaw_stream_lane_t lane = s_data_wait_test->upload != 0u
                                              ? H2_GIZCLAW_STREAM_DATA_UPLINK
                                              : H2_GIZCLAW_STREAM_DATA_DOWNLINK;
    assert(
        !h2_gizclaw_req_data_ready_internal(s_data_wait_test->service, lane));
    atomic_fetch_add(&s_data_wait_test->data_waits, 1u);
  }
  return h2_pal_cond_wait(h2_desktop_platform_sync_api(), cond, mutex, timeout);
}
static int handoff_start_task(void *user, const h2_pal_task_options_t *options,
                              h2_pal_task_entry_t entry, void *ctx,
                              h2_pal_task_t **out) {
  stream_handoff_test_t *test = user;
  const char *target =
      test->upload != 0u ? "$gizclaw/data-up" : "$gizclaw/data-down";
  if (strcmp(options->name, target) == 0) {
    test->data_entry = entry;
    test->data_ctx = ctx;
    return h2_pal_task_start(h2_desktop_platform_task_api(), options,
                             handoff_gated_data_task, test, out);
  }
  return h2_pal_task_start(h2_desktop_platform_task_api(), options, entry, ctx,
                           out);
}
static int handoff_join_task(void *user, h2_pal_task_t *task) {
  (void)user;
  return h2_pal_task_join(h2_desktop_platform_task_api(), task);
}
static void *handoff_stop_service(void *user) {
  stream_handoff_test_t *test = user;
  atomic_store(&test->stop_entered, true);
  test->stop_result = h2_gizclaw_service_stop(test->service);
  atomic_store(&test->stop_exited, true);
  return NULL;
}
static void test_stream_data_task_handoff(void) {
  const h2_gizclaw_async_rpc_ops_t ops = {.start_stream = handoff_open,
                                          .finish_write = handoff_finish,
                                          .write = handoff_write,
                                          .result = handoff_result,
                                          .cancel = handoff_cancel,
                                          .destroy = handoff_wire_destroy};
  for (unsigned mode = 0; mode < 14u; ++mode) {
    if (mode == 2u || mode == 4u || mode == 5u || mode == 6u || mode == 10u)
      continue; /* Covered by the fixed-ring backpressure tests below. */
    test_env_t env;
    h2_gizclaw_service_t *service = create_profile_service(&env);
    stream_handoff_test_t test = {
        .app = pthread_self(),
        .mode = mode,
        .upload = mode == 9u || mode == 12u
                      ? H2_GIZCLAW_STREAM_INPUT_BYTES + 3u
                      : 0u,
        .service = service,
        .base_allocator = service->client_config.allocator,
        .expected = mode == 1u                 ? H2_PAL_ERR_FORMAT
                    : mode == 2u || mode == 7u ? H2_PAL_ERR_IO
                    : mode == 3u || mode == 11u || mode == 13u
                        ? H2_PAL_ERR_CLOSED
                    : mode == 10u              ? H2_PAL_ERR_NO_MEMORY
                    : mode >= 4u && mode <= 6u ? H2_PAL_ERR_NO_SPACE
                                               : H2_PAL_OK};
    s_handoff = &test;
    const h2_pal_mem_vtable_t mem_vtable = {.alloc = handoff_alloc,
                                            .realloc = handoff_realloc,
                                            .free = handoff_free};
    const h2_pal_mem_api_t allocator = {.vtable = &mem_vtable, .user = &test};
    if (mode == 10u)
      service->client_config.allocator = &allocator;
    const h2_pal_task_vtable_t tasks = {.start = handoff_start_task,
                                        .join = handoff_join_task};
    const h2_pal_task_api_t task_api = {.vtable = &tasks, .user = &test};
    if (mode == 9u || mode >= 12u)
      service->config.task = &task_api;
    const h2_pal_sync_api_t *base_sync = h2_desktop_platform_sync_api();
    h2_pal_sync_vtable_t sync_vtable = *base_sync->vtable;
    sync_vtable.wait_cond = handoff_wait_cond;
    const h2_pal_sync_api_t sync_api = {.vtable = &sync_vtable,
                                        .user = base_sync->user};
    if (mode == 12u) {
      service->config.sync = &sync_api;
      atomic_store(&test.task_gate, true);
    }
    h2_gizclaw_async_rpc_test_set_ops(&ops);
    h2_gizclaw_req_t *request = NULL;
    assert(h2_gizclaw_req_create_stream_internal(
               service, 1u, H2_GIZCLAW_RPC_ALL_SPEED_TEST_RUN, &handoff_tag,
               (h2_gizclaw_rpc_bytes_t){0}, 1234u, test.upload, handoff_consume,
               handoff_destroy, &test, &request) == H2_PAL_OK);
    if (mode == 8u) {
      h2_gizclaw_req_release(request); /* CREATED: no task or wire access. */
      assert(atomic_load(&test.destroyed) == 1u && test.next == 0u);
    } else {
      assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
      if (mode == 12u)
        wait_for_count(&test.data_waits, 1u);
      assert(h2_gizclaw_req_do(
                 request, &test, test.upload != 0u ? handoff_input : NULL,
                 test.upload == 0u ? handoff_output : NULL, NULL) == H2_PAL_OK);
      if (mode == 9u) {
        wait_for_count(&test.opened, 1u);
        h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 10u);
        assert(atomic_load(&test.writes) == 0u);
        assert(h2_gizclaw_req_wait(request, 0u) == H2_PAL_ERR_TIMEOUT);
        atomic_store(&test.task_gate, true);
      }
      if (mode == 13u) {
        /* Cancel a queued request before the data worker ever starts. */
        bool queued = false;
        for (unsigned i = 0u; i < 2000u && !queued; ++i) {
          assert(h2_pal_mutex_lock(service->config.sync, service->mutex) ==
                 H2_PAL_OK);
          queued = (void *)service->data_downlink_stream == (void *)request &&
                   h2_gizclaw_req_data_ready_internal(
                       service, H2_GIZCLAW_STREAM_DATA_DOWNLINK);
          assert(h2_pal_mutex_unlock(service->config.sync, service->mutex) ==
                 H2_PAL_OK);
          if (!queued)
            h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 1u);
        }
        assert(queued);
        assert(h2_gizclaw_req_cancel(request) == H2_PAL_OK);
      }
      if (mode == 12u) {
        wait_for_count(&test.writes, 2u);
        wait_for_count(&test.data_waits, 2u);
        /* Keep a live RPC without runnable data. No timed data-worker poll
         * is permitted, even while the network owner still polls its SDK. */
        assert(h2_gizclaw_req_wait(request, 20u) == H2_PAL_ERR_TIMEOUT);
        atomic_store(&test.gate, true);
      }
      pthread_t stopper;
      if ((mode >= 3u && mode <= 5u) || mode == 11u) {
        wait_for_count(&test.entered, 1u);
        if (mode == 3u)
          assert(h2_gizclaw_req_cancel(request) == H2_PAL_OK);
        else if (mode != 11u)
          wait_for_count(&test.rejected, 1u);
        if (mode == 11u) {
          assert(pthread_create(&stopper, NULL, handoff_stop_service, &test) ==
                 0);
          while (!atomic_load(&test.stop_entered))
            sched_yield();
          h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 10u);
          assert(!atomic_load(&test.stop_exited));
        }
        assert(h2_gizclaw_req_wait(request, 0u) == H2_PAL_ERR_TIMEOUT);
        h2_gizclaw_req_release(request);
        request = NULL;
        assert(atomic_load(&test.destroyed) == 0u);
        atomic_store(&test.gate, true);
        if (mode == 11u) {
          assert(pthread_join(stopper, NULL) == 0);
          assert(test.stop_result == H2_PAL_OK &&
                 atomic_load(&test.stop_exited));
        }
      } else {
        assert(h2_gizclaw_req_wait_dispatch_internal(request) == test.expected);
        assert(atomic_load(&test.callbacks) == 0u);
        h2_gizclaw_req_release(request);
        request = NULL;
      }
      wait_for_count(&test.wire_destroyed, 1u);
      if (mode == 12u)
        wait_for_count(&test.data_waits, 2u);
      if (mode == 13u) {
        assert(test.frames == 0u);
        assert(h2_pal_mutex_lock(service->config.sync, service->mutex) ==
               H2_PAL_OK);
        assert(service->data_downlink_stream == NULL &&
               !h2_gizclaw_req_data_ready_internal(
                   service, H2_GIZCLAW_STREAM_DATA_DOWNLINK));
        assert(h2_pal_mutex_unlock(service->config.sync, service->mutex) ==
               H2_PAL_OK);
        atomic_store(&test.task_gate, true);
      }
      wait_for_count(&test.destroyed, 1u);
      if (mode == 0u)
        assert(test.frames == 3u);
      if (mode == 9u || mode == 12u)
        assert(atomic_load(&test.writes) == 2u &&
               test.accepted == test.upload);
    }
    assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
    assert(service->audio_uplink_stream == NULL &&
           service->audio_downlink_stream == NULL &&
           service->data_uplink_stream == NULL &&
           service->data_downlink_stream == NULL &&
           service->data_uplink_task == NULL &&
           service->data_downlink_task == NULL);
    assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
    h2_gizclaw_async_rpc_test_set_ops(NULL);
  }
}

static void handoff_sink_received(void *user, h2_gizclaw_req_t *request) {
  stream_handoff_test_t *test = user;
  assert(request != NULL && test->frames == 3u);
  assert(!pthread_equal(pthread_self(), test->app));
  assert(!pthread_equal(pthread_self(), test->owner));
  assert(atomic_fetch_add(&test->sink_calls, 1u) == 0u);
  while (test->hold_sink && !atomic_load(&test->gate))
    sched_yield();
  assert(atomic_load(&test->destroyed) == 0u);
}

static void handoff_sink_stop(void *user) {
  stream_handoff_test_t *test = user;
  assert(pthread_equal(pthread_self(), test->owner));
  assert(atomic_fetch_add(&test->sink_stops, 1u) == 0u);
}

static void test_stream_sink_one_shot(void) {
  const h2_gizclaw_async_rpc_ops_t ops = {.start_stream = handoff_open,
                                          .write = handoff_write,
                                          .finish_write = handoff_finish,
                                          .result = handoff_result,
                                          .cancel = handoff_cancel,
                                          .destroy = handoff_wire_destroy};
  for (unsigned mode = 0; mode < 4u; ++mode) {
    test_env_t env;
    h2_gizclaw_service_t *service = create_profile_service(&env);
    stream_handoff_test_t test = {.service = service,
                                  .app = pthread_self(),
                                  .hold_sink = mode >= 2u,
                                  .expected = mode == 0u   ? H2_PAL_OK
                                              : mode == 1u ? H2_PAL_ERR_IO
                                                           : H2_PAL_ERR_CLOSED};
    s_handoff = &test;
    h2_gizclaw_async_rpc_test_set_ops(&ops);
    h2_gizclaw_req_t *request = NULL;
    assert(h2_gizclaw_req_create_sink_stream_internal(
               service, 1u, H2_GIZCLAW_RPC_ALL_SPEED_TEST_RUN, &handoff_tag,
               (h2_gizclaw_rpc_bytes_t){0}, 1234u, handoff_consume,
               handoff_sink_received, NULL, handoff_sink_stop, handoff_destroy,
               &test, true, &request) == H2_PAL_OK);
    assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
    assert(h2_gizclaw_req_do(request, &test, NULL, handoff_output, NULL) ==
           H2_PAL_OK);
    wait_for_count(&test.sink_calls, 1u);
    /* Network EOS and consumer notification alone do not finish the request.
     * A pending sink is not called again on subsequent network iterations. */
    assert(h2_gizclaw_req_wait(request, 20u) == H2_PAL_ERR_TIMEOUT);
    assert(atomic_load(&test.sink_calls) == 1u);
    pthread_t stopper;
    if (mode < 2u) {
      h2_gizclaw_req_sink_done_internal(request, test.expected);
      /* Duplicate completion cannot replace the first published result. */
      h2_gizclaw_req_sink_done_internal(request, H2_PAL_ERR_FORMAT);
      assert(h2_gizclaw_req_wait(request, 2000u) == test.expected);
    } else {
      if (mode == 2u)
        assert(h2_gizclaw_req_cancel(request) == H2_PAL_OK);
      else {
        assert(pthread_create(&stopper, NULL, handoff_stop_service, &test) ==
               0);
        while (!atomic_load(&test.stop_entered))
          sched_yield();
      }
      assert(h2_gizclaw_req_wait(request, 20u) == H2_PAL_ERR_TIMEOUT);
      assert(atomic_load(&test.sink_stops) == 0u);
      assert(!atomic_load(&test.stop_exited));
    }
    h2_gizclaw_req_release(request);
    atomic_store(&test.gate, true);
    if (mode == 3u) {
      assert(pthread_join(stopper, NULL) == 0 && test.stop_result == H2_PAL_OK);
    }
    wait_for_count(&test.wire_destroyed, 1u);
    /* The wire is destroyed by the operation's finish hook; the sink context
     * is destroyed only when the execution reference is retired afterwards. */
    wait_for_count(&test.destroyed, 1u);
    assert(atomic_load(&test.sink_calls) == 1u);
    assert(atomic_load(&test.sink_stops) == 1u);
    assert(atomic_load(&test.destroyed) == 1u);
    assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
    assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
    h2_gizclaw_async_rpc_test_set_ops(NULL);
  }
}

static int hook_result(h2_gizclaw_rpc_request_t *request,
                       h2_gizclaw_rpc_response_t *out) {
  stream_handoff_test_t *test = (stream_handoff_test_t *)request;
  assert(pthread_equal(pthread_self(), test->owner));
  memset(out, 0, sizeof(*out));
  /* Keep the lane task caught up so queue-limit tests isolate the application
   * backlog, not the independent ingress queue. */
  if (atomic_load(&test->hook_consumed) < test->next ||
      ((test->hook_mode == 6u || test->hook_mode == 7u) && test->next == 1u) ||
      /* This success scenario paces its producer, exercising credit reuse
       * without assuming OS scheduling will keep the app ahead of a burst. */
      (test->hook_mode == 5u &&
       test->next > atomic_load(&test->hook_delivered) + 8u) ||
      (test->hook_mode == 8u && test->next == 1u && !atomic_load(&test->gate)))
    return H2_PAL_ERR_WOULD_BLOCK;
  if (test->next > test->hook_data_count + 1u)
    return H2_PAL_OK;
  uint8_t bytes[32768];
  memset(bytes, 0x5a, sizeof(bytes));
  h2_gizclaw_rpc_stream_event_t event = {0};
  if (test->next == 0u) {
    event.kind = H2_GIZCLAW_RPC_STREAM_RESPONSE;
    event.result_payload = (h2_gizclaw_rpc_bytes_t){bytes, 3u};
    event.error_message = (h2_gizclaw_rpc_bytes_t){bytes + 3u, 2u};
  } else if (test->next <= test->hook_data_count) {
    event.kind = H2_GIZCLAW_RPC_STREAM_DATA;
    event.data = (h2_gizclaw_rpc_bytes_t){bytes, test->hook_data_bytes};
  } else {
    event.kind = H2_GIZCLAW_RPC_STREAM_EOS;
  }
  ++test->next;
  int rc = test->ingress(test->ingress_user, &event);
  memset(bytes, 0xa5, sizeof(bytes));
  return rc == H2_PAL_OK ? H2_PAL_ERR_WOULD_BLOCK : rc;
}

static int hook_consume(void *user,
                        const h2_gizclaw_rpc_stream_event_t *event) {
  stream_handoff_test_t *test = user;
  assert(!pthread_equal(pthread_self(), test->app));
  assert(!pthread_equal(pthread_self(), test->owner));
  h2_gizclaw_rpc_bytes_t payload = event->kind == H2_GIZCLAW_RPC_STREAM_DATA
                                       ? event->data
                                       : event->result_payload;
  for (size_t i = 0; i < payload.len; ++i)
    assert(payload.data[i] == 0x5a);
  atomic_fetch_add(&test->hook_consumed, 1u);
  return H2_PAL_OK;
}

static __attribute__((unused)) void test_stream_app_hooks(void) {
  const h2_gizclaw_async_rpc_ops_t ops = {.start_stream = handoff_open,
                                          .finish_write = handoff_finish,
                                          .result = hook_result,
                                          .cancel = handoff_cancel,
                                          .destroy = handoff_wire_destroy};
  for (unsigned mode = 0u; mode < 9u; ++mode) {
    test_env_t env;
    h2_gizclaw_service_t *service = create_profile_service(&env);
    stream_handoff_test_t test = {
        .service = service,
        .app = pthread_self(),
        .hook_mode = mode,
        .hook_data_count =
            mode == 2u || mode == 4u || mode == 5u || mode == 8u ? 80u : 5u,
        .hook_data_bytes = mode == 2u || mode == 8u   ? 0u
                           : mode == 3u || mode == 4u ? 32768u
                                                      : 257u,
        .expected = mode == 2u || mode == 3u || mode == 8u ? H2_PAL_ERR_NO_SPACE
                    : mode >= 6u                           ? H2_PAL_ERR_CLOSED
                                                           : H2_PAL_OK};
    s_handoff = &test;
    h2_gizclaw_async_rpc_test_set_ops(&ops);
    h2_gizclaw_req_t *request = NULL;
    assert(h2_gizclaw_req_create_stream_internal(
               service, 1u, H2_GIZCLAW_RPC_ALL_SPEED_TEST_RUN, &handoff_tag,
               (h2_gizclaw_rpc_bytes_t){0}, 1234u, 0u, hook_consume,
               handoff_destroy, &test, &request) == H2_PAL_OK);
    assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
    assert(h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL) == H2_PAL_OK);
    if (mode < 5u) {
      /* Terminal wait never requires app dispatch, even with queued hooks. */
      assert(h2_gizclaw_req_wait(request, 2000u) == test.expected);
      assert(test.hook_seen == 0u && atomic_load(&test.callbacks) == 0u);
      h2_gizclaw_req_release(request);
      request = NULL;
      assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
      if (mode != 4u)
        assert(h2_gizclaw_service_deinit(service) == H2_PAL_ERR_INVALID_STATE);
    }
    if (mode != 4u) {
      for (unsigned n = 0; n < 2000u && !atomic_load(&test.callbacks); ++n) {
        size_t count = 0u;
        assert(h2_gizclaw_service_poll(service, 1u, &count) == H2_PAL_OK);
        assert(count <= 1u);
        h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 1u);
      }
      assert(atomic_load(&test.callbacks) == 1u);
      assert(test.hook_seen == (mode == 2u || mode == 8u ? 64u
                                : mode == 3u             ? 4u
                                : mode >= 6u ? 1u
                                             : test.hook_data_count + 2u));
    }
    h2_gizclaw_req_release(request);
    assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
    assert(atomic_load(&test.destroyed) == 1u);
    assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
  }
}

static void test_stream_data_task_parallel_requests(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_profile_service(&env);
  stream_handoff_test_t pair[2] = {{.app = pthread_self()},
                                   {.app = pthread_self(), .upload = 4099u}};
  s_handoff = pair;
  const h2_gizclaw_async_rpc_ops_t ops = {.start_stream = handoff_open,
                                          .write = handoff_write,
                                          .finish_write = handoff_finish,
                                          .result = handoff_result,
                                          .cancel = handoff_cancel,
                                          .destroy = handoff_wire_destroy};
  h2_gizclaw_async_rpc_test_set_ops(&ops);
  atomic_store(&env.connect_gate, false);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  h2_gizclaw_req_t *requests[2] = {0};
  for (uint8_t i = 0u; i < 2u; ++i) {
    assert(h2_gizclaw_req_create_stream_internal(
               service, i, H2_GIZCLAW_RPC_ALL_SPEED_TEST_RUN, &handoff_tag,
               (h2_gizclaw_rpc_bytes_t){&i, 1u}, 1234u, pair[i].upload,
               handoff_consume, handoff_destroy, &pair[i],
               &requests[i]) == H2_PAL_OK);
    assert(h2_gizclaw_req_do(requests[i], &pair[i],
                             pair[i].upload != 0u ? handoff_input : NULL,
                             pair[i].upload == 0u ? handoff_output : NULL,
                             NULL) == H2_PAL_OK);
  }
  /* Uplink and downlink are independent active slots. A second request in the
   * same direction is rejected immediately; it is never queued behind one. */
  stream_handoff_test_t conflicts[2] = {
      {.app = pthread_self()}, {.app = pthread_self(), .upload = 4099u}};
  for (uint8_t i = 0u; i < 2u; ++i) {
    h2_gizclaw_req_t *conflict = NULL;
    assert(h2_gizclaw_req_create_stream_internal(
               service, i + 2u, H2_GIZCLAW_RPC_ALL_SPEED_TEST_RUN, &handoff_tag,
               (h2_gizclaw_rpc_bytes_t){&i, 1u}, 1234u, conflicts[i].upload,
               handoff_consume, handoff_destroy, &conflicts[i],
               &conflict) == H2_PAL_OK);
    assert(h2_gizclaw_req_do(conflict, &conflicts[i],
                             conflicts[i].upload != 0u ? handoff_input : NULL,
                             conflicts[i].upload == 0u ? handoff_output : NULL,
                             NULL) == H2_PAL_ERR_BUSY);
    assert(h2_gizclaw_req_wait(conflict, 0u) == H2_PAL_ERR_INVALID_STATE);
    h2_gizclaw_req_release(conflict);
    assert(atomic_load(&conflicts[i].destroyed) == 1u);
  }
  atomic_store(&env.connect_gate, true);
  for (unsigned i = 0u; i < 2u; ++i) {
    assert(h2_gizclaw_req_wait_dispatch_internal(requests[i]) == H2_PAL_OK);
    h2_gizclaw_req_release(requests[i]);
  }
  for (unsigned i = 0u; i < 2u; ++i) {
    wait_for_count(&pair[i].destroyed, 1u);
    assert(pair[i].frames == 3u && pair[i].accepted == pair[i].upload);
    assert(atomic_load(&pair[i].destroyed) == 1u);
    assert(atomic_load(&pair[i].wire_destroyed) == 1u);
  }
  /* Protocol calls share the sole network owner, while data consumption is
   * independently owned by the two directional Random tasks. */
  assert(pthread_equal(pair[0].owner, pair[1].owner));
  assert(!pthread_equal(pair[0].consumer, pair[1].consumer));
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(service->audio_uplink_stream == NULL &&
         service->audio_downlink_stream == NULL &&
         service->data_uplink_stream == NULL &&
         service->data_downlink_stream == NULL);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
  h2_gizclaw_async_rpc_test_set_ops(NULL);
}

static void test_rejected_stream_submission_releases_lane(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_service(&env, 1u);
  atomic_store(&env.event_emitted, true);
  atomic_store(&env.connect_gate, false);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);

  h2_gizclaw_operation_t *blocker = NULL;
  assert(submit_test_operation(service, 1u, run_immediate, record_completion,
                               &env, &blocker) == H2_PAL_OK);

  stream_handoff_test_t test = {.app = pthread_self()};
  s_handoff = &test;
  const h2_gizclaw_async_rpc_ops_t ops = {
      .start_stream = handoff_open,
      .finish_write = handoff_finish,
      .result = handoff_result,
      .cancel = handoff_cancel,
      .destroy = handoff_wire_destroy,
  };
  h2_gizclaw_async_rpc_test_set_ops(&ops);
  h2_gizclaw_req_t *request = NULL;
  assert(h2_gizclaw_req_create_stream_internal(
             service, 2u, H2_GIZCLAW_RPC_ALL_SPEED_TEST_RUN, &handoff_tag,
             (h2_gizclaw_rpc_bytes_t){0}, 1234u, 0u, handoff_consume,
             handoff_destroy, &test, &request) == H2_PAL_OK);
  assert(h2_gizclaw_req_do(request, &test, NULL, handoff_output, NULL) ==
         H2_PAL_ERR_WOULD_BLOCK);
  assert(service->data_downlink_stream == NULL);
  assert(h2_gizclaw_req_wait(request, 0u) == H2_PAL_ERR_INVALID_STATE);
  h2_gizclaw_req_release(request);
  assert(atomic_load(&test.destroyed) == 1u);

  atomic_store(&env.connect_gate, true);
  wait_until(&env, 1u, 0u, 0u);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
  h2_gizclaw_async_rpc_test_set_ops(NULL);
}

static void test_diagnostics_public_invalid_arguments(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_profile_service(&env);
  const struct {
    size_t upload, download;
    uint32_t timeout;
    bool null_service;
  } invalid[] = {
      {1u, 1u, 1000u, true},
      {1u, 1u, 1000u, false},
      {0u, 0u, 1000u, false},
      {H2_GIZCLAW_SPEEDTEST_MAX_BYTES + 1u, 0u, 1000u, false},
      {0u, H2_GIZCLAW_SPEEDTEST_MAX_BYTES + 1u, 1000u, false},
      {1u, 0u, 0u, false},
      {0u, 1u, (uint32_t)INT32_MAX + 1u, false},
  };
  const h2_gizclaw_speedtest_result_t empty = {0};
  for (size_t i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    h2_gizclaw_service_t *owner = invalid[i].null_service ? NULL : service;
    h2_gizclaw_req_t *request = (void *)1;
    assert(h2_gizclaw_req_create_speedtest(
               owner, 1u, invalid[i].upload, invalid[i].download,
               invalid[i].timeout, &request) == H2_PAL_ERR_INVALID_ARG);
    assert(request == NULL);
    h2_gizclaw_speedtest_result_t result;
    memset(&result, 0xa5, sizeof(result));
    assert(h2_gizclaw_rpc_speedtest(owner, invalid[i].upload,
                                    invalid[i].download, invalid[i].timeout,
                                    &result) == H2_PAL_ERR_INVALID_ARG);
    assert(memcmp(&result, &empty, sizeof(result)) == 0);
  }
  h2_gizclaw_req_t *request = NULL;
  assert(h2_gizclaw_req_create_speedtest(service, 1u,
                                         H2_GIZCLAW_SPEEDTEST_MAX_BYTES, 0u,
                                         1000u, &request) == H2_PAL_OK);
  h2_gizclaw_req_release(request); // Size validation does not send any bytes.
  assert(h2_gizclaw_req_create_speedtest(service, 1u, 0u,
                                         H2_GIZCLAW_SPEEDTEST_MAX_BYTES, 1000u,
                                         &request) == H2_PAL_OK);
  h2_gizclaw_req_release(request);
  assert(h2_gizclaw_req_create_speedtest(service, 1u, 1u, 1u, 1000u, NULL) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_rpc_speedtest(service, 1u, 1u, 1000u, NULL) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_resp_parse_speedtest(NULL, NULL) == H2_PAL_ERR_INVALID_ARG);
  h2_gizclaw_ping_result_t ping = {UINT64_MAX, INT64_MAX};
  assert(h2_gizclaw_rpc_ping(NULL, 1000u, &ping) == H2_PAL_ERR_INVALID_ARG);
  assert(ping.round_trip_ms == 0u && ping.server_time_ms == 0);
  request = (void *)1;
  assert(h2_gizclaw_req_create_ping(NULL, 1u, 1000u, &request) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(request == NULL);
  assert(h2_gizclaw_rpc_peer_delete(NULL, 1000u) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
}

typedef struct audio_interaction {
  h2_gizclaw_service_t *service;
  h2_gizclaw_req_t *request;
  atomic_uint phase;
  atomic_bool start;
  h2_pal_result_t result;
} audio_interaction_t;

static void *audio_request_task(void *user) {
  audio_interaction_t *interaction = user;
  assert(h2_gizclaw_req_do(interaction->request, NULL, NULL, NULL, NULL) ==
         H2_PAL_OK);
  atomic_store(&interaction->phase, 1u);
  while (!atomic_load(&interaction->start))
    assert(h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 1u) ==
           H2_PAL_OK);
  assert(h2_gizclaw_service_audio_start(interaction->service) == H2_PAL_OK);
  atomic_store(&interaction->phase, 2u);
  interaction->result = h2_gizclaw_req_wait(interaction->request, 2000u);
  return NULL;
}

static void test_asr_from_owned_pcm_track(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_service(&env, 4u);
  atomic_store(&env.event_emitted, true);
  speech_wire_test_t wire = {.service = service, .app_thread = pthread_self()};
  s_speech = &wire;
  h2_gizclaw_async_rpc_test_set_ops(&speech_test_ops);
  h2_gizclaw_track_t *track = NULL;
  const h2_gizclaw_pcm_track_config_t config = {
      .allocator = service->client_config.allocator,
      .uplink_capacity = 2048u,
      .downlink_capacity = 1024u};
  assert(h2_gizclaw_pcm_track_create(&config, &track) == H2_PAL_OK);
  assert(h2_gizclaw_service_set_track(service, track) == H2_PAL_OK);
  uint8_t input[1346];
  for (size_t i = 0u; i < sizeof(input); ++i)
    input[i] = (uint8_t)i;
  const uint8_t stale[2] = {0xff, 0xee};
  assert(h2_gizclaw_pcm_track_write(track, stale, sizeof(stale)) == H2_PAL_OK);
  /* The boundary server expects exactly the samples supplied by the mic pump.
   */
  atomic_store(&wire.captured_bytes, sizeof(input));
  const h2_gizclaw_speech_transcribe_options_t options = {
      .model_name = {"asr", 3u}, .content_type = {"audio/pcm", 9u}};
  h2_gizclaw_req_t *request = NULL;
  assert(h2_gizclaw_req_create_speech_transcribe(service, 42u, &options, 2000u,
                                                 &request) == H2_PAL_OK);
  assert(h2_gizclaw_service_start(service) == H2_PAL_OK);
  audio_interaction_t interaction = {.service = service, .request = request};
  pthread_t request_task;
  assert(pthread_create(&request_task, NULL, audio_request_task,
                        &interaction) == 0);
  wait_for_count(&interaction.phase, 1u);
  assert(h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), 50u) ==
         H2_PAL_OK);
  assert(h2_gizclaw_pcm_track_pending_internal(track) == sizeof(stale));
  assert(wire.bytes_len == 0u); /* do alone never opens the microphone. */
  assert(h2_gizclaw_service_audio_end(service) == H2_PAL_ERR_INVALID_STATE);
  atomic_store(&interaction.start, true);
  wait_for_count(&interaction.phase, 2u);
  assert(h2_gizclaw_pcm_track_write(track, input, sizeof(input)) == H2_PAL_OK);
  assert(h2_gizclaw_service_audio_end(service) == H2_PAL_OK);
  /* Finish freezes the accepted prefix, including an incomplete final frame.
   * Later writes must neither extend this request nor be consumed by it. */
  const uint8_t later[640] = {0x73};
  assert(h2_gizclaw_pcm_track_write(track, later, sizeof(later)) == H2_PAL_OK);
  assert(h2_gizclaw_service_audio_end(service) == H2_PAL_OK);
  assert(pthread_join(request_task, NULL) == 0);
  assert(interaction.result == H2_PAL_OK);
  assert(wire.bytes_len == sizeof(input) &&
         memcmp(wire.bytes, input, sizeof(input)) == 0 && wire.finished);
  uint8_t storage_bytes[64];
  h2_gizclaw_resp_storage_t storage = {storage_bytes, sizeof(storage_bytes),
                                       0u};
  h2_gizclaw_speech_transcribe_response_t response = {0};
  assert(h2_gizclaw_resp_parse_speech_transcribe(request, &storage,
                                                 &response) == H2_PAL_OK);
  assert(response.transcript.len == 5u &&
         memcmp(response.transcript.data, "hello", 5u) == 0);
  h2_gizclaw_req_release(request);
  uint8_t remaining[640];
  size_t remaining_len = 0u;
  assert(h2_gizclaw_service_pcm_read_internal(service, remaining,
                                              sizeof(remaining),
                                              &remaining_len) == H2_PAL_OK);
  assert(remaining_len == sizeof(later) &&
         memcmp(remaining, later, sizeof(later)) == 0);
  /* Request EOS does not close either PCM ring or enqueue an audio marker. */
  uint8_t sample[2];
  assert(h2_gizclaw_pcm_track_read(track, sample, sizeof(sample)) ==
         H2_PAL_ERR_WOULD_BLOCK);
  assert(h2_gizclaw_pcm_track_write(track, input, sizeof(input)) == H2_PAL_OK);
  assert(h2_gizclaw_service_unset_track(service, track) == H2_PAL_OK);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_OK);
  h2_gizclaw_async_rpc_test_set_ops(NULL);
}

static void test_owned_pcm_track_binding(void) {
  test_env_t env;
  h2_gizclaw_service_t *service = create_service(&env, 2u);
  h2_gizclaw_track_t *track = NULL;
  const h2_gizclaw_pcm_track_config_t config = {
      .allocator = service->client_config.allocator,
      .uplink_capacity = 1024u,
      .downlink_capacity = 1024u};
  assert(h2_gizclaw_pcm_track_create(&config, &track) == H2_PAL_OK);
  assert(h2_gizclaw_service_set_track(service, track) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_ERR_BUSY);
  uint8_t pcm[640], output[640];
  memset(pcm, 0x17, sizeof(pcm));
  size_t len = 0u;
  assert(h2_gizclaw_pcm_track_write(track, pcm, sizeof(pcm)) == H2_PAL_OK);
  assert(h2_gizclaw_service_pcm_read_internal(service, output, sizeof(output),
                                              &len) == H2_PAL_OK);
  assert(len == sizeof(pcm) && memcmp(pcm, output, len) == 0);
  assert(h2_gizclaw_service_pcm_write_internal(service, pcm, sizeof(pcm)) ==
         H2_PAL_OK);
  assert(h2_gizclaw_pcm_track_read(track, output, sizeof(output)) == H2_PAL_OK);
  assert(memcmp(pcm, output, sizeof(pcm)) == 0);
  assert(h2_gizclaw_service_unset_track(service, track) == H2_PAL_OK);
  assert(h2_gizclaw_service_set_track(service, track) == H2_PAL_OK);
  assert(h2_gizclaw_service_stop(service) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(service) == H2_PAL_OK);
  assert(h2_gizclaw_pcm_track_destroy(&track) == H2_PAL_OK);
}

int main(int argc, char **argv) {
  if (argc == 2 && strcmp(argv[1], "--app-hooks-only") == 0) {
    test_stream_data_task_handoff();
    return 0;
  }
  if (argc == 2 && strcmp(argv[1], "--download-stream-only") == 0) {
    test_stream_sink_one_shot();
    test_audio_play_request();
    test_pixa_download_request_paths();
    test_group_audio_download_request_paths();
    test_track_unset_waits_for_read_and_write();
    return 0;
  }
  if (argc == 2 && strcmp(argv[1], "--pixa-only") == 0) {
    test_pixa_download_request_paths();
    return 0;
  }
  if (argc == 2 && strcmp(argv[1], "--group-download-only") == 0) {
    test_group_audio_download_request_paths();
    return 0;
  }
  if (argc == 2 && strcmp(argv[1], "--pcm-stream-only") == 0) {
    test_asr_from_owned_pcm_track();
    test_speech_options_and_created_lifetime();
    test_speech_managed_requests();
    test_owned_pcm_track_binding();
    test_service_uplink_start_failure_cleans_network_task();
    return 0;
  }
  if (argc == 2 && strcmp(argv[1], "--stream-data-only") == 0) {
    test_speedtest_managed_requests();
    test_stream_data_task_handoff();
    test_stream_data_task_parallel_requests();
    test_rejected_stream_submission_releases_lane();
    test_service_partial_start_and_join_failures();
    h2_gizclaw_service_test_set_client_ops(NULL);
    puts("gizclaw stream lane task tests passed");
    return 0;
  }
  if (argc == 2 && strcmp(argv[1], "--speedtest-only") == 0) {
    test_speedtest_managed_requests();
    return 0;
  }
  assert(argc == 1);
  test_stream_sink_one_shot();
  test_workspace_selection_boundaries();
  test_req_remote_error_mapping();
  test_asr_from_owned_pcm_track();
  test_owned_pcm_track_binding();
  test_firmware_public_request_paths();
  test_service_partial_start_and_join_failures();
  test_service_terminal_callback_obeys_poll_budget();
  test_conversation_reply_route_ids();
  test_conversation_barge_in_supersedes_open_reply();
  test_diagnostics_public_invalid_arguments();
  test_speedtest_managed_requests();
  test_stream_data_task_handoff();
  test_stream_data_task_parallel_requests();
  test_rejected_stream_submission_releases_lane();
  test_service_event_queue_backpressure();
  test_conversation_public_audio_tasks();
  test_audio_play_request();
  test_speech_options_and_created_lifetime();
  test_service_uplink_start_failure_cleans_network_task();
  test_speech_managed_requests();
  test_track_read_validation_and_rebinding();
  test_track_unset_waits_for_read_and_write();
  test_group_audio_download_request_paths();
  test_pixa_download_request_paths();
  assert(test_pet_delete_rpc_regression() == 0);
  test_pet_public_request_paths();
  test_social_full_page_arena_growth();
  test_group_member_and_message_request_paths();
  assert(test_friend_group_message_projection_rpcs() == 0);
  test_friend_group_public_request_paths();
  test_friend_public_request_paths();
  test_req_start_backpressure_deadline_and_cancel();
  test_contact_and_group_public_paths();
  test_contact_mutation_requests();
  test_workspace_request_and_response_paths();
  test_workspace_direct_input_update();
  test_req_wait_does_not_require_callback_dispatch();
  test_req_callback_reference_and_independent_wait();
  test_req_dispatch_queue_full_drops_hook_and_settles();
  test_req_profile_no_poll_and_copied_results();
  test_req_profile_put_copies_input_and_checks_parser();
  test_req_cancel_stop_and_created_lifetime();
  test_req_response_errors();
  test_req_multiple_waiters_and_queued_close();
  test_req_register_and_peer_delete();
  test_req_ping_execution_timing();
  test_req_unary_context_lifetime();
  test_req_telemetry_copy_and_backpressure();
  test_req_point_storage_and_limits();
  test_req_workflow_public_paths();
  h2_gizclaw_async_rpc_test_set_ops(NULL);
  test_fifo_capacity_and_dispatch();
  test_runtime_dispatch_wakeup_is_coalesced();
  test_pending_operation_does_not_block_following_work();
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
  h2_gizclaw_service_test_set_client_ops(NULL);
  puts("h2_gizclaw service tests passed");
  return 0;
}
