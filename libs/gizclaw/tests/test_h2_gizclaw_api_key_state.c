#include "h2_desktop_platform.h"
#include "h2_gizclaw_api_key.h"
#include "h2_gizclaw_service_internal.h"
#include "payload/system.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* Use the same client/request seams as the Service tests. The real Service
 * worker, request queues, cancellation and owner dispatch remain under test. */
typedef struct test_env {
  h2_gizclaw_service_t *service;
  h2_gizclaw_api_key_state_t *state;
  h2_gizclaw_cancel_fn cancel;
  void *cancel_user;
  atomic_bool connected;
  atomic_bool reply;
  atomic_uint starts;
  atomic_uint destroys;
  atomic_uint_fast64_t now;
  int methods[32];
  int method;
  h2_pal_result_t revoke_result;
  h2_pal_result_t create_result;
  h2_pal_time_api_t time;
} test_env_t;
static test_env_t *s_env;

static h2_pal_result_t fake_init(const h2_gizclaw_config_t *config,
                                 h2_gizclaw_client_t **out_client) {
  s_env->cancel = config->cancel_requested;
  s_env->cancel_user = config->cancel_user;
  *out_client = (h2_gizclaw_client_t *)s_env;
  return H2_PAL_OK;
}
static h2_pal_result_t fake_connect(h2_gizclaw_client_t *client) {
  (void)client;
  while (!atomic_load(&s_env->connected)) {
    if (s_env->cancel(s_env->cancel_user))
      return H2_PAL_ERR_CLOSED;
    sched_yield();
  }
  return H2_PAL_OK;
}
static h2_pal_result_t fake_poll(h2_gizclaw_client_t *client, int timeout_ms) {
  (void)client;
  (void)timeout_ms;
  return H2_PAL_ERR_TIMEOUT;
}
static h2_pal_result_t fake_handler(h2_gizclaw_client_t *client,
                                    h2_gizclaw_client_event_sink_fn handler,
                                    void *user) {
  (void)client;
  (void)handler;
  (void)user;
  return H2_PAL_OK;
}
static h2_pal_result_t fake_dispatch(h2_gizclaw_client_t *client) {
  (void)client;
  return H2_PAL_ERR_WOULD_BLOCK;
}
static h2_pal_result_t fake_close(h2_gizclaw_client_t *client) {
  (void)client;
  return H2_PAL_OK;
}
static void fake_deinit(h2_gizclaw_client_t *client) { (void)client; }
static const h2_gizclaw_service_client_ops_t client_ops = {
    .init = fake_init,
    .connect = fake_connect,
    .poll = fake_poll,
    .set_event_handler = fake_handler,
    .dispatch_event = fake_dispatch,
    .close = fake_close,
    .deinit = fake_deinit};

static int fake_start(h2_gizclaw_client_t *client,
                      h2_gizclaw_rpc_method_t method,
                      h2_gizclaw_rpc_bytes_t payload, uint32_t timeout_ms,
                      h2_gizclaw_rpc_request_t **out_request) {
  (void)client;
  assert(timeout_ms != 0u);
  pb_istream_t stream = pb_istream_from_buffer(payload.data, payload.len);
  if (method == 96) {
    gizclaw_rpc_v1_APIKeyCreateRequest message = {0};
    assert(pb_decode(&stream, gizclaw_rpc_v1_APIKeyCreateRequest_fields,
                     &message));
    assert(strcmp(message.display_name, "binding") == 0);
    assert(message.manage_api_keys);
  } else {
    assert(method == 98);
    gizclaw_rpc_v1_APIKeyRevokeRequest message = {0};
    assert(pb_decode(&stream, gizclaw_rpc_v1_APIKeyRevokeRequest_fields,
                     &message));
    assert(strcmp(message.name, "key-name") == 0);
  }
  s_env->method = method;
  unsigned count = atomic_load(&s_env->starts);
  assert(count < 32u);
  s_env->methods[count] = method;
  *out_request = (h2_gizclaw_rpc_request_t *)s_env;
  atomic_fetch_add(&s_env->starts, 1u);
  return H2_PAL_OK;
}
static int fake_result(h2_gizclaw_rpc_request_t *request,
                       h2_gizclaw_rpc_response_t *out_response) {
  (void)request;
  if (!atomic_load(&s_env->reply))
    return H2_PAL_ERR_WOULD_BLOCK;
  int rc = s_env->method == 98 ? s_env->revoke_result : s_env->create_result;
  if (rc != H2_PAL_OK)
    return rc;
  uint8_t bytes[256];
  pb_ostream_t stream = pb_ostream_from_buffer(bytes, sizeof(bytes));
  if (s_env->method == 96) {
    gizclaw_rpc_v1_APIKeyCreateResponse message = {.has_value = true};
    strcpy(message.value.name, "key-name");
    strcpy(message.api_key, "test-secret");
    assert(pb_encode(&stream, gizclaw_rpc_v1_APIKeyCreateResponse_fields,
                     &message));
  } else {
    gizclaw_rpc_v1_APIKeyRevokeResponse message = {0};
    assert(pb_encode(&stream, gizclaw_rpc_v1_APIKeyRevokeResponse_fields,
                     &message));
  }
  *out_response = (h2_gizclaw_rpc_response_t){0};
  if (stream.bytes_written != 0u) {
    out_response->result_payload = h2_pal_mem_alloc(
        h2_desktop_platform_default_allocator(), stream.bytes_written);
    assert(out_response->result_payload != NULL);
    memcpy(out_response->result_payload, bytes, stream.bytes_written);
    out_response->result_payload_len = stream.bytes_written;
  }
  return H2_PAL_OK;
}
static void fake_cancel(h2_gizclaw_rpc_request_t *request) { (void)request; }
static void fake_destroy(h2_gizclaw_rpc_request_t *request) {
  (void)request;
  atomic_fetch_add(&s_env->destroys, 1u);
}
static const h2_gizclaw_async_rpc_ops_t rpc_ops = {.start = fake_start,
                                                   .result = fake_result,
                                                   .cancel = fake_cancel,
                                                   .destroy = fake_destroy};
static h2_pal_result_t fake_time(void *user, uint64_t *out_ms) {
  test_env_t *env = user;
  *out_ms = atomic_load(&env->now);
  return H2_PAL_OK;
}
static const h2_pal_time_vtable_t time_ops = {.get_monotonic_ms = fake_time};

static void setup(test_env_t *env, bool connected) {
  memset(env, 0, sizeof(*env));
  atomic_init(&env->connected, connected);
  atomic_init(&env->reply, true);
  atomic_init(&env->starts, 0u);
  atomic_init(&env->destroys, 0u);
  atomic_init(&env->now, 100u);
  s_env = env;
  h2_gizclaw_service_test_set_client_ops(&client_ops);
  h2_gizclaw_async_rpc_test_set_ops(&rpc_ops);
  h2_gizclaw_config_t client_config = {
      .allocator = h2_desktop_platform_default_allocator(),
      .time = h2_desktop_platform_time_api()};
  h2_gizclaw_service_config_t config = {.client_config = &client_config,
                                        .task = h2_desktop_platform_task_api(),
                                        .queue =
                                            h2_desktop_platform_queue_api(),
                                        .sync = h2_desktop_platform_sync_api(),
                                        .operation_capacity = 8u,
                                        .client_poll_timeout_ms = 1};
  assert(h2_gizclaw_service_init(&config, &env->service) == H2_PAL_OK);
  assert(h2_gizclaw_service_start(env->service) == H2_PAL_OK);
  env->time = (h2_pal_time_api_t){.user = env, .vtable = &time_ops};
  char name[] = "binding";
  h2_gizclaw_api_key_state_config_t state_config = {
      .service = env->service,
      .mem = client_config.allocator,
      .sync = config.sync,
      .time = &env->time,
      .display_name = {name, strlen(name)},
      .manage_api_keys = true,
      .timeout_ms = 1000u};
  assert(h2_gizclaw_api_key_state_create(&state_config, &env->state) ==
         H2_PAL_OK);
  memset(name, 'x', sizeof(name)); /* State must own its display name. */
}
static h2_gizclaw_api_key_snapshot_t snapshot(test_env_t *env) {
  h2_gizclaw_api_key_snapshot_t result;
  assert(h2_gizclaw_api_key_state_snapshot(env->state, &result) == H2_PAL_OK);
  return result;
}
static void poll_once(test_env_t *env) {
  size_t count = 0u;
  assert(h2_gizclaw_service_poll(env->service, 1u, &count) == H2_PAL_OK);
  sched_yield();
}
static void finish(test_env_t *env) {
  for (unsigned i = 0u; i < 1000000u; ++i) {
    poll_once(env);
    if (!snapshot(env).busy)
      return;
  }
  assert(false);
}
static void wait_count(atomic_uint *count, unsigned expected) {
  for (unsigned i = 0u; i < 1000000u; ++i) {
    if (atomic_load(count) >= expected)
      return;
    sched_yield();
  }
  assert(false);
}
static void teardown(test_env_t *env) {
  assert(h2_gizclaw_api_key_state_close(env->state) == H2_PAL_OK);
  assert(h2_gizclaw_service_stop(env->service) == H2_PAL_OK);
  for (unsigned i = 0u; i < 32u; ++i)
    poll_once(env);
  assert(h2_gizclaw_api_key_state_destroy(&env->state) == H2_PAL_OK);
  assert(env->state == NULL);
  assert(h2_gizclaw_api_key_state_destroy(&env->state) == H2_PAL_OK);
  assert(h2_gizclaw_service_deinit(env->service) == H2_PAL_OK);
}
static void refresh(test_env_t *env, bool revoke) {
  assert(h2_gizclaw_api_key_state_request_refresh(env->state, revoke) ==
         H2_PAL_OK);
  finish(env);
}

static void test_refresh_chain(void) {
  test_env_t env;
  setup(&env, true);
  h2_gizclaw_api_key_snapshot_t initial = snapshot(&env);
  assert(!initial.valid && initial.stale && !initial.busy && !initial.closed);
  assert(h2_gizclaw_api_key_state_request_refresh(env.state, false) ==
         H2_PAL_OK);
  for (unsigned i = 0u; i < 10u; ++i)
    assert(h2_gizclaw_api_key_state_request_refresh(env.state, true) ==
           H2_PAL_OK);
  finish(&env);
  h2_gizclaw_api_key_snapshot_t ready = snapshot(&env);
  assert(ready.valid && !ready.stale && ready.last_error == H2_PAL_OK);
  assert(strcmp(ready.key.secret, "test-secret") == 0);
  assert(ready.revision > initial.revision && atomic_load(&env.starts) == 1u);
  assert(snapshot(&env).revision == ready.revision);
  refresh(&env, true);
  assert(atomic_load(&env.starts) == 3u);
  assert(env.methods[0] == 96 && env.methods[1] == 98 && env.methods[2] == 96);
  assert(snapshot(&env).valid && !snapshot(&env).stale);
  teardown(&env);
}
static void test_failures_and_not_found(void) {
  test_env_t env;
  setup(&env, true);
  refresh(&env, false);
  env.revoke_result = H2_PAL_ERR_IO;
  refresh(&env, true);
  h2_gizclaw_api_key_snapshot_t failed = snapshot(&env);
  assert(failed.valid && failed.stale && failed.last_error == H2_PAL_ERR_IO);
  assert(strcmp(failed.key.secret, "test-secret") == 0);
  assert(atomic_load(&env.starts) == 2u);
  env.revoke_result = H2_PAL_ERR_NOT_FOUND;
  refresh(&env, true);
  assert(snapshot(&env).valid && !snapshot(&env).stale);
  assert(atomic_load(&env.starts) == 4u && env.methods[2] == 98 &&
         env.methods[3] == 96);
  env.create_result = H2_PAL_ERR_IO;
  refresh(&env, false);
  failed = snapshot(&env);
  assert(!failed.valid && failed.stale && failed.key.secret[0] == 0);
  assert(failed.last_error == H2_PAL_ERR_IO);
  teardown(&env);
}
static void test_queue_timeout_and_generation(void) {
  test_env_t env;
  setup(&env, false);
  assert(h2_gizclaw_api_key_state_request_refresh(env.state, false) ==
         H2_PAL_OK);
  atomic_store(&env.now, 1099u);
  assert(snapshot(&env).busy);
  assert(h2_gizclaw_api_key_state_request_refresh(env.state, true) ==
         H2_PAL_OK);
  atomic_store(&env.now, 1100u);
  h2_gizclaw_api_key_snapshot_t expired = snapshot(&env);
  assert(!expired.busy && expired.last_error == H2_PAL_ERR_TIMEOUT);
  assert(atomic_load(&env.starts) == 0u);
  assert(h2_gizclaw_api_key_state_destroy(&env.state) == H2_PAL_ERR_BUSY);
  assert(h2_gizclaw_api_key_state_request_refresh(env.state, false) ==
         H2_PAL_OK);
  atomic_store(&env.connected, true);
  finish(&env);
  assert(snapshot(&env).valid && snapshot(&env).last_error == H2_PAL_OK);
  assert(atomic_load(&env.starts) == 1u);
  teardown(&env);
}
static void test_request_refresh_checks_deadline(void) {
  test_env_t env;
  setup(&env, false);
  assert(h2_gizclaw_api_key_state_request_refresh(env.state, false) ==
         H2_PAL_OK);
  atomic_store(&env.now, 1100u);
  assert(h2_gizclaw_api_key_state_request_refresh(env.state, false) ==
         H2_PAL_OK);
  h2_gizclaw_api_key_snapshot_t next = snapshot(&env);
  assert(next.busy && next.last_error == H2_PAL_ERR_TIMEOUT);
  atomic_store(&env.connected, true);
  finish(&env);
  assert(snapshot(&env).valid && atomic_load(&env.starts) == 1u);
  teardown(&env);
}
static void test_close_and_late_completion(bool complete_before_close) {
  test_env_t env;
  setup(&env, true);
  atomic_store(&env.reply, complete_before_close);
  assert(h2_gizclaw_api_key_state_request_refresh(env.state, false) ==
         H2_PAL_OK);
  wait_count(&env.starts, 1u);
  if (complete_before_close)
    wait_count(&env.destroys,
               1u); /* Wire finished; owner has not dispatched. */
  assert(h2_gizclaw_api_key_state_close(env.state) == H2_PAL_OK);
  h2_gizclaw_api_key_snapshot_t closed = snapshot(&env);
  assert(closed.closed && !closed.busy && !closed.valid);
  assert(h2_gizclaw_api_key_state_close(env.state) == H2_PAL_OK);
  assert(snapshot(&env).revision == closed.revision);
  assert(h2_gizclaw_api_key_state_request_refresh(env.state, false) ==
         H2_PAL_ERR_CLOSED);
  assert(h2_gizclaw_api_key_state_destroy(&env.state) == H2_PAL_ERR_BUSY);
  assert(h2_gizclaw_service_stop(env.service) == H2_PAL_OK);
  assert(h2_gizclaw_api_key_state_destroy(&env.state) == H2_PAL_ERR_BUSY);
  for (unsigned i = 0u; i < 32u; ++i)
    poll_once(&env);
  h2_gizclaw_api_key_snapshot_t drained = snapshot(&env);
  assert(memcmp(&closed, &drained, sizeof(closed)) == 0);
  teardown(&env);
}
static void test_revoke_create_share_deadline(void) {
  test_env_t env;
  setup(&env, true);
  refresh(&env, false);
  assert(h2_gizclaw_api_key_state_request_refresh(env.state, true) ==
         H2_PAL_OK);
  wait_count(&env.destroys, 2u);
  /* Revoke has completed, but only the owner can submit the create step. */
  assert(atomic_load(&env.starts) == 2u);
  atomic_store(&env.reply, false);
  atomic_store(&env.now, 1099u);
  poll_once(&env);
  wait_count(&env.starts, 3u);
  h2_gizclaw_api_key_snapshot_t creating = snapshot(&env);
  assert(creating.busy && !creating.valid && creating.key.secret[0] == 0);
  assert(h2_gizclaw_api_key_state_request_refresh(env.state, true) ==
         H2_PAL_OK);
  atomic_store(&env.now, 1100u);
  h2_gizclaw_api_key_snapshot_t expired = snapshot(&env);
  assert(!expired.busy && !expired.valid &&
         expired.last_error == H2_PAL_ERR_TIMEOUT);
  assert(h2_gizclaw_api_key_state_destroy(&env.state) == H2_PAL_ERR_BUSY);
  teardown(&env);
}

static void test_close_during_revoke(void) {
  test_env_t env;
  setup(&env, true);
  refresh(&env, false);
  atomic_store(&env.reply, false);
  assert(h2_gizclaw_api_key_state_request_refresh(env.state, true) ==
         H2_PAL_OK);
  wait_count(&env.starts, 2u);
  for (unsigned i = 0u; i < 10u; ++i)
    assert(h2_gizclaw_api_key_state_request_refresh(env.state, true) ==
           H2_PAL_OK);
  assert(h2_gizclaw_api_key_state_close(env.state) == H2_PAL_OK);
  h2_gizclaw_api_key_snapshot_t closed = snapshot(&env);
  assert(closed.valid && closed.stale && closed.closed && !closed.busy);
  assert(strcmp(closed.key.secret, "test-secret") == 0);
  teardown(&env);
  assert(atomic_load(&env.starts) == 2u);
}

static void test_submission_failure(void) {
  test_env_t env;
  setup(&env, true);
  refresh(&env, false);
  assert(h2_gizclaw_service_stop(env.service) == H2_PAL_OK);
  assert(h2_gizclaw_api_key_state_request_refresh(env.state, true) ==
         H2_PAL_ERR_CLOSED);
  h2_gizclaw_api_key_snapshot_t failed = snapshot(&env);
  assert(failed.valid && failed.stale && !failed.busy);
  assert(failed.last_error == H2_PAL_ERR_CLOSED);
  assert(h2_gizclaw_api_key_state_request_refresh(env.state, false) ==
         H2_PAL_ERR_CLOSED);
  failed = snapshot(&env);
  assert(!failed.valid && !failed.busy && failed.key.secret[0] == 0);
  teardown(&env);
}

int main(void) {
  test_revoke_create_share_deadline();
  test_close_during_revoke();
  test_submission_failure();
  test_refresh_chain();
  test_failures_and_not_found();
  test_queue_timeout_and_generation();
  test_request_refresh_checks_deadline();
  test_close_and_late_completion(false);
  test_close_and_late_completion(true);
  h2_gizclaw_service_test_set_client_ops(NULL);
  h2_gizclaw_async_rpc_test_set_ops(NULL);
  puts("API key state tests passed");
  return 0;
}
