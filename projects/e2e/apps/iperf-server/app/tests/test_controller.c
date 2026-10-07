#include "h2_desktop_platform.h"
#include "h2_atomic.h"
#include "h2_iperf_server_app.h"
#include "h2_iperf_test_support.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static unsigned starts, stops;
static bool fail_start;
static h2_pal_net_addr_t loopback(unsigned family, uint16_t port) {
  if (family == 4)
    return h2_iperf_test_loopback(port);
  h2_pal_net_addr_t address = {.family = H2_PAL_NET_FAMILY_IPV6, .port = port};
  address.ip[15] = 1;
  return address;
}
static int network_start(void *user, h2_iperf_server_app_mode_t mode,
                         h2_iperf_server_app_network_t *out) {
  (void)user;
  ++starts;
  if (fail_start)
    return H2_PAL_ERR_IO;
  if (mode != H2_IPERF_SERVER_APP_MODE_IPV6) {
    out->ipv4 = loopback(4, 0);
    strcpy(out->ipv4_text, "127.0.0.1");
  }
  if (mode != H2_IPERF_SERVER_APP_MODE_IPV4) {
    out->ipv6 = loopback(6, 0);
    strcpy(out->ipv6_text, "::1");
  }
  return H2_PAL_OK;
}
static int network_stop(void *user) {
  (void)user;
  ++stops;
  return H2_PAL_OK;
}
static uint64_t now(const h2_pal_time_api_t *time) {
  uint64_t ms;
  assert(h2_pal_time_get_monotonic_ms(time, &ms) == H2_PAL_OK);
  return ms;
}
static h2_iperf_server_app_snapshot_t
wait_phase(h2_iperf_server_app_t *app, h2_iperf_server_app_phase_t phase,
           const h2_pal_time_api_t *time) {
  uint64_t deadline = now(time) + 2000;
  h2_iperf_server_app_snapshot_t snapshot;
  do {
    assert(h2_iperf_server_app_snapshot(app, &snapshot) == H2_PAL_OK);
    if (snapshot.phase == phase)
      return snapshot;
    h2_iperf_test_sleep_ms(5);
  } while (now(time) < deadline);
  fprintf(stderr, "wanted phase %d, got %d, error %d\n", phase, snapshot.phase,
          snapshot.error);
  assert(false);
  return snapshot;
}
static void stop(h2_iperf_server_app_t *app, h2_iperf_server_app_mode_t mode,
                 const h2_pal_time_api_t *time) {
  uint64_t begin = now(time);
  assert(h2_iperf_server_app_request(app, mode, false) == H2_PAL_OK);
  (void)wait_phase(app, H2_IPERF_SERVER_APP_STOPPED, time);
  assert(now(time) - begin < 500);
}
static void run_client(h2_iperf_server_app_t *app, h2_iperf_test_env_t *env,
                       unsigned family, uint16_t port, bool udp, bool reverse) {
  h2_iperf_server_app_snapshot_t before, after;
  assert(h2_iperf_server_app_snapshot(app, &before) == H2_PAL_OK);
  unsigned index = family == 4 ? 0 : 1;
  h2_iperf_client_params_t params = {.server_addr = loopback(family, port),
                                     .port = port,
                                     .protocol = udp ? H2_IPERF_PROTOCOL_UDP
                                                     : H2_IPERF_PROTOCOL_TCP,
                                     .reverse = reverse,
                                     .duration_ms = 180,
                                     .block_len = udp ? 1200u : 16384u,
                                     .bitrate_bps = 2000000u,
                                     .control_timeout_ms = 2000u};
  h2_iperf_result_t result;
  assert(h2_iperf_client_run(&env->config, &params, &result) == H2_PAL_OK);
  for (unsigned i = 0; i < 200; ++i) {
    assert(h2_iperf_server_app_snapshot(app, &after) == H2_PAL_OK);
    if (after.streams[index].completed_tests >
        before.streams[index].completed_tests)
      break;
    h2_iperf_test_sleep_ms(5);
  }
  assert(after.streams[index].completed_tests ==
         before.streams[index].completed_tests + 1);
  assert(after.streams[index].result.local.bytes > 0);
  assert(after.streams[index].progress.bytes > 0);
  assert(after.streams[index].progress.sending == reverse);
  assert(after.streams[index].result.protocol == params.protocol);
  printf("PASS family=%u protocol=%s reverse=%d\n", family, udp ? "udp" : "tcp",
         reverse);
}
static void send_all(const h2_iperf_config_t *config, int sock,
                     const void *bytes, size_t len) {
  const uint8_t *data = bytes;
  while (len) {
    int sent = h2_pal_net_tcp_send_timeout(config->net, sock, data, len, 1000);
    assert(sent > 0);
    data += sent;
    len -= (size_t)sent;
  }
}
static int connect_ctrl(h2_iperf_test_env_t *env, unsigned family,
                        uint16_t port) {
  int sock;
  assert(h2_pal_net_tcp_open_bound(env->config.net, family, NULL, &sock) ==
         H2_PAL_OK);
  h2_pal_net_addr_t address = loopback(family, port);
  assert(h2_pal_net_tcp_connect(env->config.net, sock, &address, 1000) ==
         H2_PAL_OK);
  return sock;
}
static void wait_state(h2_iperf_test_env_t *env, int sock, uint8_t expected) {
  uint8_t value = 0;
  assert(h2_pal_net_tcp_recv(env->config.net, sock, &value, 1, 1000) == 1);
  assert(value == expected);
}
static void parameters(h2_iperf_test_env_t *env, int sock, const char *json) {
  uint32_t length = (uint32_t)strlen(json);
  const uint8_t header[] = {(uint8_t)(length >> 24), (uint8_t)(length >> 16),
                            (uint8_t)(length >> 8), (uint8_t)length};
  send_all(&env->config, sock, header, sizeof(header));
  send_all(&env->config, sock, json, length);
}
static void recv_all(h2_iperf_test_env_t *env, int sock, uint8_t *data,
                     size_t len) {
  while (len != 0) {
    int got = h2_pal_net_tcp_recv(env->config.net, sock, data, len, 1000);
    assert(got > 0);
    len -= (size_t)got;
    data += got;
  }
}
static void cancel_setup(h2_iperf_server_app_t *app, h2_iperf_test_env_t *env,
                         uint16_t port, unsigned stage) {
  assert(h2_iperf_server_app_request(app, H2_IPERF_SERVER_APP_MODE_IPV6,
                                     true) == H2_PAL_OK);
  (void)wait_phase(app, H2_IPERF_SERVER_APP_LISTENING, env->config.time);
  int sock = connect_ctrl(env, 6, port);
  int data_sock = -1;
  if (stage > 0) {
    char cookie[37] = "abcdefghijklmnopqrstuvwxyz0123456789";
    send_all(&env->config, sock, cookie, sizeof(cookie));
    wait_state(env, sock, 9);
    if (stage > 1) {
      parameters(
          env, sock,
          stage == 3
              ? "{\"udp\":true,\"parallel\":1,\"time\":10,\"len\":1200}"
              : "{\"tcp\":true,\"parallel\":1,\"time\":10,\"len\":16384}");
      wait_state(env, sock, 10);
    }
    if (stage >= 4) {
      data_sock = connect_ctrl(env, 6, port);
      send_all(&env->config, data_sock, cookie, sizeof(cookie));
      wait_state(env, sock, 1);
      wait_state(env, sock, 2);
      uint8_t payload[512] = {0};
      send_all(&env->config, data_sock, payload, sizeof(payload));
      uint8_t end = 4;
      send_all(&env->config, sock, &end, 1);
      wait_state(env, sock, 13);
      if (stage == 5) {
        parameters(env, sock,
                   "{\"streams\":[{\"id\":1,\"bytes\":512,\"start_time\":0,"
                   "\"end_time\":0.1}]}");
        uint8_t header[4];
        recv_all(env, sock, header, sizeof(header));
        uint32_t length = ((uint32_t)header[0] << 24) |
                          ((uint32_t)header[1] << 16) |
                          ((uint32_t)header[2] << 8) | header[3];
        assert(length < 2048);
        uint8_t result[2048];
        recv_all(env, sock, result, length);
        wait_state(env, sock, 14); // Withhold IPERF_DONE, then cancel.
      }
    }
  }
  h2_iperf_test_sleep_ms(30);
  stop(app, H2_IPERF_SERVER_APP_MODE_IPV6, env->config.time);
  h2_iperf_server_app_snapshot_t snapshot;
  assert(h2_iperf_server_app_snapshot(app, &snapshot) == H2_PAL_OK);
  assert(snapshot.streams[1].completed_tests == 0);
  h2_pal_net_close(env->config.net, sock);
  h2_pal_net_close(env->config.net, data_sock);
  printf("PASS cancellation control-stage=%u\n", stage);
}
static void reject_limits(h2_iperf_server_app_t *app, h2_iperf_test_env_t *env,
                          uint16_t port) {
  assert(h2_iperf_server_app_request(app, H2_IPERF_SERVER_APP_MODE_IPV6,
                                     true) == H2_PAL_OK);
  (void)wait_phase(app, H2_IPERF_SERVER_APP_LISTENING, env->config.time);
  for (unsigned oversized = 0; oversized < 2; ++oversized) {
    int sock = connect_ctrl(env, 6, port);
    char cookie[37] = "abcdefghijklmnopqrstuvwxyz0123456789";
    send_all(&env->config, sock, cookie, sizeof(cookie));
    wait_state(env, sock, 9);
    if (oversized == 0) {
      parameters(env, sock,
                 "{\"tcp\":true,\"parallel\":1,\"time\":1,\"len\":262144}");
      wait_state(env, sock,
                 254); // SERVER_ERROR, before allocating a data block.
    } else {
      const uint8_t header[] = {0, 0, 0x10, 1}; // 4097 exceeds the JSON limit.
      send_all(&env->config, sock, header, sizeof(header));
      uint8_t byte;
      assert(h2_pal_net_tcp_recv(env->config.net, sock, &byte, 1, 1000) ==
             H2_PAL_ERR_CLOSED);
    }
    h2_pal_net_close(env->config.net, sock);
  }
  run_client(app, env, 6, port, false,
             false); // Rejected clients do not poison the listener.
  stop(app, H2_IPERF_SERVER_APP_MODE_IPV6, env->config.time);
  puts("PASS bounded-block-and-json recovery=1");
}

typedef struct client_thread {
  h2_iperf_config_t config;
  h2_iperf_client_params_t params;
  int rc;
} client_thread_t;
static void *long_client(void *user) {
  client_thread_t *client = user;
  h2_iperf_result_t result;
  client->rc = h2_iperf_client_run(&client->config, &client->params, &result);
  return NULL;
}
static void cancel_running(h2_iperf_server_app_t *app, h2_iperf_test_env_t *env,
                           uint16_t port, bool udp, bool reverse) {
  assert(h2_iperf_server_app_request(app, H2_IPERF_SERVER_APP_MODE_DUAL,
                                     true) == H2_PAL_OK);
  (void)wait_phase(app, H2_IPERF_SERVER_APP_LISTENING, env->config.time);
  client_thread_t client = {.config = env->config,
                            .params = {.server_addr = loopback(6, port),
                                       .port = port,
                                       .duration_ms = 10000,
                                       .protocol = udp ? H2_IPERF_PROTOCOL_UDP
                                                       : H2_IPERF_PROTOCOL_TCP,
                                       .reverse = reverse,
                                       .block_len = udp ? 1200 : 16384,
                                       .bitrate_bps = 2000000,
                                       .control_timeout_ms = 2000}};
  pthread_t thread;
  assert(pthread_create(&thread, NULL, long_client, &client) == 0);
  h2_iperf_server_app_snapshot_t snapshot;
  bool active = false;
  for (unsigned i = 0; i < 400; ++i) {
    assert(h2_iperf_server_app_snapshot(app, &snapshot) == H2_PAL_OK);
    if (snapshot.streams[1].active && snapshot.streams[1].progress.bytes > 0) {
      active = true;
      break;
    }
    h2_iperf_test_sleep_ms(5);
  }
  assert(active);
  stop(app, H2_IPERF_SERVER_APP_MODE_DUAL, env->config.time);
  assert(pthread_join(thread, NULL) == 0);
  assert(client.rc != H2_PAL_OK);
  printf("PASS cancellation active protocol=%s reverse=%d\n",
         udp ? "udp" : "tcp", reverse);
}

typedef struct cleanup_fixture {
  const h2_pal_task_api_t *task;
  const h2_pal_sync_api_t *sync;
  h2_pal_task_entry_t manager_entry;
  void *manager_context;
  h2_pal_task_t *manager;
  h2_pal_task_t *workers[2];
  h2_atomic_bool_t fail_manager_lock;
  h2_atomic_bool_t manager_lock_failed;
  bool invalid_ipv6;
  bool fail_network_start;
  bool fail_network_stop;
  bool fail_worker_join;
  bool network_active;
  h2_atomic_u32_t stop_calls;
} cleanup_fixture_t;

static _Thread_local bool cleanup_manager_thread;
static void cleanup_manager_entry(void *user) {
  cleanup_fixture_t *f = user;
  cleanup_manager_thread = true;
  f->manager_entry(f->manager_context);
}
static int cleanup_task_start(void *user, const h2_pal_task_options_t *options,
                              h2_pal_task_entry_t entry, void *context,
                              h2_pal_task_t **out) {
  cleanup_fixture_t *f = user;
  if (strcmp(options->name, "iperf-server/control") == 0) {
    f->manager_entry = entry;
    f->manager_context = context;
    int rc = h2_pal_task_start(f->task, options, cleanup_manager_entry, f, out);
    if (rc == H2_PAL_OK)
      f->manager = *out;
    return rc;
  }
  unsigned index = strcmp(options->name, "iperf-server/ipv4") == 0 ? 0u : 1u;
  int rc = h2_pal_task_start(f->task, options, entry, context, out);
  if (rc == H2_PAL_OK)
    f->workers[index] = *out;
  return rc;
}
static int cleanup_task_join(void *user, h2_pal_task_t *task) {
  cleanup_fixture_t *f = user;
  if (task == f->manager) {
    int rc = h2_pal_task_join(f->task, task);
    if (rc == H2_PAL_OK)
      f->manager = NULL;
    return rc;
  }
  for (unsigned i = 0; i < 2u; ++i) {
    if (task != f->workers[i])
      continue;
    if (f->fail_worker_join)
      return H2_PAL_ERR_IO;
    int rc = h2_pal_task_join(f->task, task);
    if (rc == H2_PAL_OK)
      f->workers[i] = NULL;
    return rc;
  }
  return h2_pal_task_join(f->task, task);
}
static h2_pal_result_t cleanup_mutex_create(void *user,
                                            const h2_pal_mutex_config_t *config,
                                            h2_pal_mutex_t **out) {
  return h2_pal_mutex_create(((cleanup_fixture_t *)user)->sync, config, out);
}
static h2_pal_result_t cleanup_mutex_destroy(void *user, h2_pal_mutex_t *mutex) {
  return h2_pal_mutex_destroy(((cleanup_fixture_t *)user)->sync, mutex);
}
static h2_pal_result_t cleanup_mutex_lock(void *user, h2_pal_mutex_t *mutex) {
  cleanup_fixture_t *f = user;
  if (cleanup_manager_thread &&
      h2_atomic_bool_load(&f->fail_manager_lock, H2_ATOMIC_ACQUIRE)) {
    h2_atomic_bool_store(&f->manager_lock_failed, true, H2_ATOMIC_RELEASE);
    return H2_PAL_ERR_IO;
  }
  return h2_pal_mutex_lock(f->sync, mutex);
}
static h2_pal_result_t cleanup_mutex_unlock(void *user, h2_pal_mutex_t *mutex) {
  return h2_pal_mutex_unlock(((cleanup_fixture_t *)user)->sync, mutex);
}
static int cleanup_network_start(void *user, h2_iperf_server_app_mode_t mode,
                                 h2_iperf_server_app_network_t *out) {
  cleanup_fixture_t *f = user;
  assert(mode == H2_IPERF_SERVER_APP_MODE_DUAL);
  f->network_active = true;
  if (f->fail_network_start)
    return H2_PAL_ERR_IO;
  out->ipv4 = loopback(4u, 0u);
  if (!f->invalid_ipv6)
    out->ipv6 = loopback(6u, 0u);
  return H2_PAL_OK;
}
static int cleanup_network_stop(void *user) {
  cleanup_fixture_t *f = user;
  /* No AP teardown is allowed while a failed join retains worker ownership. */
  assert(f->workers[0] == NULL && f->workers[1] == NULL);
  (void)h2_atomic_u32_fetch_add(&f->stop_calls, 1u, H2_ATOMIC_RELAXED);
  if (f->fail_network_stop)
    return H2_PAL_ERR_IO;
  f->network_active = false;
  return H2_PAL_OK;
}
typedef struct destroy_attempt {
  h2_iperf_server_app_t *app;
  h2_atomic_bool_t done;
  int rc;
} destroy_attempt_t;
static void *destroy_once(void *user) {
  destroy_attempt_t *attempt = user;
  attempt->rc = h2_iperf_server_app_destroy(&attempt->app);
  h2_atomic_bool_store(&attempt->done, true, H2_ATOMIC_RELEASE);
  return NULL;
}
static void test_cleanup_failure(h2_iperf_test_env_t *env, unsigned scenario) {
  cleanup_fixture_t f = {.task = h2_desktop_platform_task_api(),
                         .sync = h2_desktop_platform_sync_api(),
                         .invalid_ipv6 = scenario == 1u,
                         .fail_network_start = scenario == 4u,
                         .fail_network_stop = scenario != 2u,
                         .fail_worker_join = scenario == 2u};
  assert(h2_atomic_bool_init(&f.fail_manager_lock, false) == H2_ATOMIC_OK);
  assert(h2_atomic_bool_init(&f.manager_lock_failed, false) == H2_ATOMIC_OK);
  assert(h2_atomic_u32_init(&f.stop_calls, 0u) == H2_ATOMIC_OK);
  const h2_pal_task_vtable_t task_vtable = {
      .start = cleanup_task_start, .join = cleanup_task_join};
  const h2_pal_task_api_t task = {&f, &task_vtable};
  const h2_pal_sync_vtable_t sync_vtable = {
      .create_mutex = cleanup_mutex_create,
      .destroy_mutex = cleanup_mutex_destroy,
      .lock_mutex = cleanup_mutex_lock,
      .unlock_mutex = cleanup_mutex_unlock};
  const h2_pal_sync_api_t sync = {&f, &sync_vtable};
  h2_runtime_t runtime = {.mem = env->config.mem,
                          .net = env->config.net,
                          .time = env->config.time,
                          .crypto = env->config.crypto,
                          .log = env->config.log,
                          .task = &task,
                          .sync = &sync};
  const h2_iperf_server_app_config_t config = {
      .network_user = &f,
      .network_start = cleanup_network_start,
      .network_stop = cleanup_network_stop,
      .port = h2_iperf_test_free_port(env->config.net)};
  h2_iperf_server_app_t *app = NULL;
  assert(h2_iperf_server_app_create(&runtime, &config, &app) == H2_PAL_OK);
  assert(h2_iperf_server_app_request(app, H2_IPERF_SERVER_APP_MODE_DUAL, true) ==
         H2_PAL_OK);
  if (scenario != 1u && scenario != 4u)
    (void)wait_phase(app, H2_IPERF_SERVER_APP_LISTENING, env->config.time);
  uint64_t deadline = now(env->config.time) + 2000u;
  if (scenario == 3u) {
    h2_atomic_bool_store(&f.fail_manager_lock, true, H2_ATOMIC_RELEASE);
    while (!h2_atomic_bool_load(&f.manager_lock_failed, H2_ATOMIC_ACQUIRE) &&
           now(env->config.time) < deadline)
      h2_iperf_test_sleep_ms(5u);
    assert(h2_atomic_bool_load(&f.manager_lock_failed, H2_ATOMIC_ACQUIRE));
  } else {
    if (scenario != 1u && scenario != 4u)
      assert(h2_iperf_server_app_request(app, H2_IPERF_SERVER_APP_MODE_DUAL,
                                         false) == H2_PAL_OK);
    h2_iperf_server_app_snapshot_t snapshot;
    do {
      snapshot = wait_phase(app, H2_IPERF_SERVER_APP_STOPPING, env->config.time);
      if (snapshot.error == H2_PAL_ERR_IO)
        break;
      h2_iperf_test_sleep_ms(5u);
    } while (now(env->config.time) < deadline);
    assert(snapshot.error == H2_PAL_ERR_IO);
    unsigned stop_calls = h2_atomic_u32_load(&f.stop_calls, H2_ATOMIC_RELAXED);
    h2_iperf_test_sleep_ms(100u);
    assert(h2_atomic_u32_load(&f.stop_calls, H2_ATOMIC_RELAXED) == stop_calls);
    assert(h2_iperf_server_app_request(app, H2_IPERF_SERVER_APP_MODE_DUAL,
                                       true) == H2_PAL_ERR_BUSY);
    assert(h2_iperf_server_app_request(app, H2_IPERF_SERVER_APP_MODE_IPV4,
                                       false) == H2_PAL_ERR_BUSY);
    if (scenario == 2u)
      assert(h2_atomic_u32_load(&f.stop_calls, H2_ATOMIC_RELAXED) == 0u &&
             f.workers[0] != NULL);
  }
  if (scenario == 4u) {
    /* Failed start still owns its network. A Stop retry releases it before
     * another Start, without spinning or creating either server task. */
    assert(f.network_active && f.workers[0] == NULL && f.workers[1] == NULL);
    f.fail_network_stop = false;
    assert(h2_iperf_server_app_request(app, H2_IPERF_SERVER_APP_MODE_DUAL,
                                       false) == H2_PAL_OK);
    (void)wait_phase(app, H2_IPERF_SERVER_APP_STOPPED, env->config.time);
    assert(!f.network_active);
    f.fail_network_start = false;
    assert(h2_iperf_server_app_request(app, H2_IPERF_SERVER_APP_MODE_DUAL,
                                       true) == H2_PAL_OK);
    (void)wait_phase(app, H2_IPERF_SERVER_APP_LISTENING, env->config.time);
    f.fail_network_stop = true;
  }
  destroy_attempt_t attempt = {.app = app};
  assert(h2_atomic_bool_init(&attempt.done, false) == H2_ATOMIC_OK);
  pthread_t thread;
  assert(pthread_create(&thread, NULL, destroy_once, &attempt) == 0);
  deadline = now(env->config.time) + 1000u;
  while (!h2_atomic_bool_load(&attempt.done, H2_ATOMIC_ACQUIRE) &&
         now(env->config.time) < deadline)
    h2_iperf_test_sleep_ms(5u);
  assert(h2_atomic_bool_load(&attempt.done, H2_ATOMIC_ACQUIRE));
  assert(pthread_join(thread, NULL) == 0);
  assert(attempt.rc == H2_PAL_ERR_IO && attempt.app == app && f.network_active);
  if (scenario == 2u)
    assert(h2_atomic_u32_load(&f.stop_calls, H2_ATOMIC_RELAXED) == 0u &&
           f.workers[0] != NULL);
  assert(h2_iperf_server_app_request(app, H2_IPERF_SERVER_APP_MODE_DUAL, false) ==
         H2_PAL_ERR_CLOSED);
  f.fail_network_stop = false;
  f.fail_worker_join = false;
  h2_atomic_bool_store(&f.fail_manager_lock, false, H2_ATOMIC_RELEASE);
  assert(h2_iperf_server_app_destroy(&app) == H2_PAL_OK && app == NULL);
  assert(!f.network_active && f.workers[0] == NULL && f.workers[1] == NULL);
  h2_atomic_bool_destroy(&attempt.done);
  h2_atomic_bool_destroy(&f.fail_manager_lock);
  h2_atomic_bool_destroy(&f.manager_lock_failed);
  h2_atomic_u32_destroy(&f.stop_calls);
  printf("PASS cleanup failure scenario=%u retained=1 destroy-retry=1\n",
         scenario);
}
int main(int argc, char **argv) {
  assert(argc == 2);
  h2_iperf_test_env_t env;
  h2_iperf_test_env_init(&env, false);
  h2_runtime_t runtime = {.mem = env.config.mem,
                          .net = env.config.net,
                          .time = env.config.time,
                          .crypto = env.config.crypto,
                          .log = env.config.log,
                          .task = h2_desktop_platform_task_api(),
                          .sync = h2_desktop_platform_sync_api()};
  uint16_t port = h2_iperf_test_free_port(env.config.net);
  h2_iperf_server_app_config_t config = {.network_start = network_start,
                                         .network_stop = network_stop,
                                         .port = port};
  h2_iperf_server_app_t *app;
  assert(h2_iperf_server_app_create(&runtime, &config, &app) == H2_PAL_OK);
  assert(h2_iperf_server_app_request(app, 5, true) == H2_PAL_ERR_INVALID_ARG);
  fail_start = true;
  assert(h2_iperf_server_app_request(app, H2_IPERF_SERVER_APP_MODE_IPV4,
                                     true) == H2_PAL_OK);
  h2_iperf_server_app_snapshot_t state =
      wait_phase(app, H2_IPERF_SERVER_APP_ERROR, env.config.time);
  assert(state.error == H2_PAL_ERR_IO);
  fail_start = false;
  const h2_iperf_server_app_mode_t modes[] = {H2_IPERF_SERVER_APP_MODE_IPV4,
                                              H2_IPERF_SERVER_APP_MODE_IPV6,
                                              H2_IPERF_SERVER_APP_MODE_DUAL};
  for (unsigned m = 0; m < 3; ++m) {
    assert(h2_iperf_server_app_request(app, modes[m], true) == H2_PAL_OK);
    state = wait_phase(app, H2_IPERF_SERVER_APP_LISTENING, env.config.time);
    assert(h2_iperf_server_app_request(app, modes[m], true) == H2_PAL_ERR_BUSY);
    assert(h2_iperf_server_app_request(app, modes[(m + 1) % 3], false) ==
           H2_PAL_ERR_BUSY);
    for (unsigned family = 4; family <= 6; family += 2) {
      if ((modes[m] == H2_IPERF_SERVER_APP_MODE_IPV4 && family == 6) ||
          (modes[m] == H2_IPERF_SERVER_APP_MODE_IPV6 && family == 4))
        continue;
      for (unsigned proto = 0; proto < 2; ++proto)
        for (unsigned reverse = 0; reverse < 2; ++reverse)
          run_client(app, &env, family, port, proto != 0, reverse != 0);
    }
    stop(app, modes[m], env.config.time);
  }
  for (unsigned stage = 0; stage < 6; ++stage)
    cancel_setup(app, &env, port, stage);
  for (unsigned proto = 0; proto < 2; ++proto)
    for (unsigned reverse = 0; reverse < 2; ++reverse)
      cancel_running(app, &env, port, proto != 0, reverse != 0);
  reject_limits(app, &env, port);
  // Exercise the external iperf3 client's real wire protocol on both families.
  assert(h2_iperf_server_app_request(app, H2_IPERF_SERVER_APP_MODE_DUAL,
                                     true) == H2_PAL_OK);
  (void)wait_phase(app, H2_IPERF_SERVER_APP_LISTENING, env.config.time);
  char port_text[8];
  snprintf(port_text, sizeof(port_text), "%u", port);
  for (unsigned family = 4; family <= 6; family += 2) {
    for (unsigned proto = 0; proto < 2; ++proto) {
      for (unsigned reverse = 0; reverse < 2; ++reverse) {
        char *args[24] = {argv[1], family == 4 ? "-4" : "-6",
                          "-c",    family == 4 ? "127.0.0.1" : "::1",
                          "-p",    port_text,
                          "-t",    "1",
                          "-l",    proto ? "1200" : "16384",
                          "-b",    "2M"};
        unsigned n = 12;
        if (proto)
          args[n++] = "-u";
        if (reverse)
          args[n++] = "-R";
        args[n] = NULL;
        pid_t pid = h2_iperf_test_spawn(args);
        assert(pid > 0);
        assert(h2_iperf_test_wait(pid, 5000) == 0);
      }
    }
  }
  stop(app, H2_IPERF_SERVER_APP_MODE_DUAL, env.config.time);
  // Cancel a queued start before the manager has opened any network.
  assert(h2_iperf_server_app_request(app, H2_IPERF_SERVER_APP_MODE_IPV4,
                                     true) == H2_PAL_OK);
  assert(h2_iperf_server_app_request(app, H2_IPERF_SERVER_APP_MODE_IPV4,
                                     false) == H2_PAL_OK);
  (void)wait_phase(app, H2_IPERF_SERVER_APP_STOPPED, env.config.time);
  assert(h2_iperf_server_app_destroy(&app) == H2_PAL_OK && app == NULL);
  assert(starts == stops); // Every start attempt has a matching cleanup.
  for (unsigned scenario = 0u; scenario < 5u; ++scenario)
    test_cleanup_failure(&env, scenario);
  h2_iperf_test_env_deinit(&env);
  puts("H2_IPERF_SERVER_CONTROLLER_PASS modes=3 pal-wire=16 official-wire=8 "
       "control-cancel=6 active-cancel=4 limits=2");
  return 0;
}
