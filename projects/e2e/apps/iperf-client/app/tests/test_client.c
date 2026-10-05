#include "h2_iperf_client_app.h"
#include "h2_iperf_test_support.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

typedef struct server {
  h2_iperf_server_t *handle;
  unsigned expected;
  unsigned completed;
} server_t;

static void *serve(void *user) {
  server_t *server = user;
  for (unsigned i = 0; i < server->expected; ++i) {
    h2_iperf_result_t result = {0};
    assert(h2_iperf_server_run_once(server->handle, 10000u, &result) ==
           H2_PAL_OK);
    assert(result.local.bytes > 0);
    ++server->completed;
  }
  return NULL;
}
static unsigned checkpoints;
static void checkpoint(void *user, const char *name) {
  assert(user == &checkpoints);
  assert(strcmp(name, "case_start") == 0 || strcmp(name, "case_end") == 0);
  ++checkpoints;
}
static void test_matrix(h2_iperf_client_app_mode_t mode, unsigned rounds) {
  h2_iperf_test_env_t env;
  h2_iperf_test_env_init(&env, false);
  h2_runtime_t runtime = {.mem = env.config.mem,
                          .net = env.config.net,
                          .time = env.config.time,
                          .crypto = env.config.crypto,
                          .log = env.config.log};
  server_t servers[2] = {0};
  pthread_t threads[2];
  uint16_t port = 0;
  for (unsigned i = 0; i < 2; ++i) {
    if ((i == 0 && mode == H2_IPERF_CLIENT_APP_IPV6) ||
        (i == 1 && mode == H2_IPERF_CLIENT_APP_IPV4))
      continue;
    h2_iperf_server_params_t params = {
        .family = i == 0 ? H2_PAL_NET_FAMILY_IPV4 : H2_PAL_NET_FAMILY_IPV6,
        .port = port,
        .ephemeral_port = port == 0,
        .max_block_len = 16384u};
    assert(h2_iperf_server_create(&env.config, &params, &servers[i].handle) ==
           H2_PAL_OK);
    port = h2_iperf_server_port(servers[i].handle);
    servers[i].expected = rounds * 10u;
    assert(pthread_create(&threads[i], NULL, serve, &servers[i]) == 0);
  }
  h2_iperf_client_app_config_t config = {
      .mode = mode,
      .ipv4 = h2_iperf_test_loopback(port),
      .ipv6 = {.family = H2_PAL_NET_FAMILY_IPV6, .ip = {[15] = 1}},
      .port = port,
      .rounds = rounds,
      .duration_ms = 100u,
      .settle_ms = 1u,
      .checkpoint = checkpoint,
      .checkpoint_user = &checkpoints};
  h2_iperf_client_app_report_t report;
  checkpoints = 0;
  assert(h2_iperf_client_app_run(&runtime, &config, &report) == H2_PAL_OK);
  unsigned expected =
      rounds * 10u * (mode == H2_IPERF_CLIENT_APP_DUAL ? 2u : 1u);
  assert(report.total == expected && report.passed == expected);
  assert(checkpoints == expected * 2u);
  for (unsigned i = 0; i < 2; ++i) {
    if (servers[i].handle != NULL) {
      assert(pthread_join(threads[i], NULL) == 0);
      assert(servers[i].completed == servers[i].expected);
      h2_iperf_server_destroy(&servers[i].handle);
    }
  }
  /* All cases are accounted for on a refused endpoint, with no early exit. */
  config.mode = H2_IPERF_CLIENT_APP_IPV4;
  config.rounds = 1u;
  assert(h2_iperf_client_app_run(&runtime, &config, &report) == H2_PAL_ERR_IO);
  assert(report.total == 10u && report.passed == 0u);
  config.mode = (h2_iperf_client_app_mode_t)5;
  report = (h2_iperf_client_app_report_t){99, 99};
  assert(h2_iperf_client_app_run(&runtime, &config, &report) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(report.total == 0 && report.passed == 0);
  config.mode = H2_IPERF_CLIENT_APP_DUAL;
  config.ipv6.family = H2_PAL_NET_FAMILY_IPV4;
  assert(h2_iperf_client_app_run(&runtime, &config, &report) ==
         H2_PAL_ERR_INVALID_ARG);
  config.mode = H2_IPERF_CLIENT_APP_IPV4;
  config.rounds = 11u;
  assert(h2_iperf_client_app_run(&runtime, &config, &report) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_iperf_client_app_run(NULL, &config, &report) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_iperf_client_app_run(&runtime, NULL, &report) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_iperf_client_app_run(&runtime, &config, NULL) ==
         H2_PAL_ERR_INVALID_ARG);
  h2_iperf_test_env_deinit(&env);
}
int main(void) {
  test_matrix(H2_IPERF_CLIENT_APP_IPV4, 1);
  test_matrix(H2_IPERF_CLIENT_APP_IPV6, 1);
  test_matrix(H2_IPERF_CLIENT_APP_DUAL, 2);
  puts("iperf-client PASS: 60 real IPv4/IPv6 TCP/UDP exchanges");
  return 0;
}
