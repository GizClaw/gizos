#include "fixture.h"
#include "h2_sctp.h"
#include "peer.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct h2_pal_task {
  h2_pal_task_entry_t entry;
  void *context;
};
struct h2_pal_mutex { int live; };
static struct h2_pal_task tasks[3];
static struct h2_pal_mutex mutex;
static unsigned sockets, workers, providers, peers, starts, opens;
static unsigned fail_start, fail_open, fail_join, joins;
static int fail_mutex, fail_provider, fail_peer;

int h2_test_peer_init(const h2_pal_mem_api_t *mem,
                      const h2_pal_crypto_api_t *crypto) {
  assert(mem && crypto);
  if (fail_peer)
    return H2_PAL_ERR_IO;
  ++peers;
  return H2_PAL_OK;
}
void h2_test_peer_deinit(void) { assert(peers == 1u); --peers; }
h2_pal_result_t h2_test_sctp_create(const h2_sctp_config_t *config,
                                     h2_sctp_t **out) {
  assert(config && out);
  *out = NULL;
  if (fail_provider)
    return H2_PAL_ERR_NO_MEMORY;
  ++providers;
  *out = (h2_sctp_t *)&providers;
  return H2_PAL_OK;
}
h2_pal_result_t h2_test_sctp_destroy(h2_sctp_t **out) {
  assert(out && workers == 0u);
  if (*out) {
    assert(providers == 1u);
    --providers;
    *out = NULL;
  }
  return H2_PAL_OK;
}
static h2_pal_result_t create_mutex(void *user,
    const h2_pal_mutex_config_t *config, h2_pal_mutex_t **out) {
  (void)user;
  assert(config && !mutex.live);
  *out = NULL;
  if (fail_mutex)
    return H2_PAL_ERR_NO_MEMORY;
  mutex.live = 1;
  *out = &mutex;
  return H2_PAL_OK;
}
static h2_pal_result_t destroy_mutex(void *user, h2_pal_mutex_t *value) {
  (void)user;
  assert(value == &mutex && mutex.live && !workers && !providers && !peers);
  mutex.live = 0;
  return H2_PAL_OK;
}
static h2_pal_result_t mutex_op(void *user, h2_pal_mutex_t *value) {
  (void)user;
  assert(value == &mutex && mutex.live);
  return H2_PAL_OK;
}
static int start(void *user, const h2_pal_task_options_t *options,
                 h2_pal_task_entry_t entry, void *context,
                 h2_pal_task_t **out) {
  (void)user;
  assert(options && entry && context && mutex.live && peers && providers);
  *out = NULL;
  if (++starts == fail_start)
    return H2_PAL_ERR_NO_MEMORY;
  assert(starts <= 3u);
  struct h2_pal_task *task = &tasks[starts - 1u];
  *task = (struct h2_pal_task){entry, context};
  ++workers;
  *out = task;
  return H2_PAL_OK;
}
static int join(void *user, h2_pal_task_t *task) {
  (void)user;
  assert(task && task->entry && workers && mutex.live && sockets == 5u);
  ++joins;
  if (fail_join) {
    --fail_join;
    return H2_PAL_ERR_TIMEOUT;
  }
  /* Execute each actual worker only after the stop request. It must return
   * without touching a listener or any released protocol/Runtime object. */
  task->entry(task->context);
  task->entry = NULL;
  --workers;
  return H2_PAL_OK;
}
static int resolve(void *user, const char *name, h2_pal_net_addr_t *out) {
  (void)user;
  assert(strcmp(name, "::1") == 0);
  *out = (h2_pal_net_addr_t){.family = H2_PAL_NET_FAMILY_IPV6};
  out->ip[15] = 1u;
  return H2_PAL_OK;
}
static int open_socket(void *user, h2_pal_net_family_t family, uint16_t port,
    const h2_pal_net_bind_t *bind, int *out, h2_pal_net_addr_t *address) {
  (void)user;
  assert(family == H2_PAL_NET_FAMILY_IPV6 && bind && out && address);
  *out = -1;
  if (++opens == fail_open)
    return H2_PAL_ERR_IO;
  *out = (int)opens;
  *address = bind->source_addr;
  address->port = port;
  ++sockets;
  return H2_PAL_OK;
}
static void close_socket(void *user, int socket) {
  (void)user;
  assert(workers == 0u);
  if (socket >= 0) {
    assert(sockets);
    --sockets;
  }
}
static int ready_failure(h2_runtime_t *runtime) {
  assert(runtime && sockets == 5u && workers == 3u);
  return H2_PAL_ERR_IO;
}
int main(void) {
  const h2_pal_net_vtable_t net_vtable = {.resolve_addr = resolve,
      .tcp_listen = open_socket, .udp_open_bound = open_socket,
      .close = close_socket};
  const h2_pal_net_api_t net = {.vtable = &net_vtable};
  const h2_pal_task_vtable_t task_vtable = {.start = start, .join = join};
  const h2_pal_task_api_t task = {.vtable = &task_vtable};
  const h2_pal_sync_vtable_t sync_vtable = {.create_mutex = create_mutex,
      .destroy_mutex = destroy_mutex, .lock_mutex = mutex_op,
      .unlock_mutex = mutex_op};
  const h2_pal_sync_api_t sync = {.vtable = &sync_vtable};
  const h2_pal_time_api_t time = {0};
  const h2_pal_mem_api_t mem = {0};
  const h2_pal_crypto_api_t crypto = {0};
  const h2_pal_dtls_api_t dtls = {0};
  const h2_ipv6_tls_server_api_t tls = {0};
  h2_runtime_t runtime = {.net = &net, .task = &task, .sync = &sync,
      .time = &time, .mem = &mem, .crypto = &crypto};
  assert(h2_ipv6_board_fixture_run(NULL, &dtls, &tls, NULL) == H2_PAL_ERR_INVALID_ARG);
  for (unsigned scenario = 0; scenario < 12u; ++scenario) {
    starts = opens = joins = 0u;
    fail_mutex = scenario == 0u;
    fail_provider = scenario == 1u;
    fail_peer = scenario == 2u;
    fail_open = scenario >= 3u && scenario < 8u ? scenario - 2u : 0u;
    fail_start = scenario >= 8u && scenario < 11u ? scenario - 7u : 0u;
    fail_join = scenario == 11u ? 1u : 0u;
    assert(h2_ipv6_board_fixture_run(&runtime, &dtls, &tls, ready_failure) < 0);
    assert(!sockets && !workers && !providers && !peers && !mutex.live);
    if (scenario == 11u)
      assert(joins == 4u);
  }
  puts("board fixture startup: 12 failure paths; all owned resources released");
  return 0;
}
