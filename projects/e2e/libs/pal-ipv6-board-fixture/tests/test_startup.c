#include "fixture.h"
#include "h2_sctp.h"
#include "peer.h"
#include "peer_connection.h"
#include "sctp.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <setjmp.h>
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
static jmp_buf udp_iteration;
static unsigned dns_scenario, dns_sends, dns_witnesses;
static uint8_t dns_query[128];
static size_t dns_query_size;
static jmp_buf http_iteration;
static unsigned reset_scenario, reset_calls, forget_calls, reset_witnesses;
static unsigned connection_round, connection_closed, connection_destroyed;
static int http_active, http_offered, in_connection_loop;
static void (*reset_callback)(const h2_pal_sctp_stream_reset_event_t *, void *);
static unsigned connection_token;
static PeerConnection *test_connection(void) {
  return (PeerConnection *)&connection_token;
}
PeerConnection *h2_test_connection_create(PeerConfiguration *config) {
  assert(http_active && config && config->sctp);
  reset_callback = NULL;
  return test_connection();
}
void h2_test_connection_close(PeerConnection *connection) {
  assert(connection == test_connection() && !in_connection_loop);
  ++connection_closed;
}
void h2_test_connection_destroy(PeerConnection *connection) {
  assert(connection == test_connection() && !in_connection_loop);
  ++connection_destroyed;
}
const char *h2_test_connection_answer(PeerConnection *connection) {
  assert(connection == test_connection());
  return "v=0\r\n";
}
int h2_test_connection_remote(PeerConnection *connection, const char *sdp,
                              SdpType type) {
  assert(connection == test_connection() && !strcmp(sdp, "v=0\r\n"));
  assert(type == SDP_TYPE_OFFER);
  return H2_PAL_OK;
}
void h2_test_connection_state(PeerConnection *connection,
    void (*callback)(PeerConnectionState, void *)) {
  assert(connection == test_connection() && callback);
}
void h2_test_connection_data(PeerConnection *connection,
    h2_pal_result_t (*message)(char *, size_t, void *, uint16_t, int),
    void (*open)(void *), void (*close)(void *)) {
  assert(connection == test_connection() && message && !open && !close);
}
void h2_test_connection_channel(PeerConnection *connection,
    int (*callback)(const SctpRemoteChannel *, void *)) {
  assert(connection == test_connection() && callback);
}
void h2_test_connection_reset(PeerConnection *connection,
    void (*callback)(const h2_pal_sctp_stream_reset_event_t *, void *)) {
  assert(connection == test_connection() && callback && !reset_callback);
  reset_callback = callback;
}
h2_pal_result_t h2_test_connection_close_sid(PeerConnection *connection,
                                               uint16_t sid) {
  assert(connection == test_connection() && sid == 4 && !in_connection_loop);
  ++reset_calls;
  if (reset_scenario == 1u && reset_calls == 1u)
    return H2_PAL_ERR_BUSY;
  if (reset_scenario == 2u && reset_calls == 1u)
    return H2_PAL_ERR_WOULD_BLOCK;
  return reset_scenario == 3u ? H2_PAL_ERR_IO : H2_PAL_OK;
}
int h2_test_connection_forget_sid(PeerConnection *connection, uint16_t sid) {
  assert(connection == test_connection() && sid == 4 && !in_connection_loop);
  assert(reset_calls == (reset_scenario == 1u || reset_scenario == 2u ? 2u : 1u));
  ++forget_calls;
  return reset_scenario == 6u ? -1 : 0;
}
h2_pal_result_t h2_test_connection_loop(PeerConnection *connection,
                                          uint32_t timeout_ms) {
  assert(connection == test_connection() && timeout_ms == 0u);
  assert(reset_callback); /* The regression must register the real reset path. */
  ++connection_round;
  const unsigned completed_round =
      reset_scenario == 1u || reset_scenario == 2u ? 3u : 2u;
  if (connection_round > completed_round)
    longjmp(http_iteration, 1);
  in_connection_loop = 1;
  h2_pal_sctp_stream_reset_event_t event = {
      .stream_id = reset_scenario == 4u ? 300u : 4u,
      .direction = H2_PAL_SCTP_STREAM_RESET_INCOMING_RESET,
      .result = reset_scenario == 5u ? H2_PAL_ERR_IO : H2_PAL_OK};
  if (connection_round == completed_round)
    event.direction = H2_PAL_SCTP_STREAM_RESET_OUTGOING_COMPLETED;
  reset_callback(&event, NULL);
  reset_callback(&event, NULL); /* A duplicate cannot submit/retire twice. */
  assert(forget_calls == 0u && reset_witnesses == 0u);
  assert(reset_calls == connection_round - 1u);
  in_connection_loop = 0;
  return H2_PAL_OK;
}

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
  if (socket == 100) {
    assert(http_active && workers == 3u);
    return;
  }
  if (http_active && socket < 0)
    return;
  assert(workers == 0u);
  if (socket >= 0) {
    assert(sockets);
    --sockets;
  }
}
static int accept_http(void *user, int listener, int *out,
                        h2_pal_net_addr_t *peer, uint32_t timeout_ms) {
  (void)user;
  assert(http_active && listener == 2 && timeout_ms == 2u);
  if (!http_offered) {
    http_offered = 1;
    *out = 100;
    *peer = (h2_pal_net_addr_t){.family = H2_PAL_NET_FAMILY_IPV6};
    peer->ip[15] = 2u;
    return H2_PAL_OK;
  }
  if (connection_destroyed)
    longjmp(http_iteration, 1);
  return H2_PAL_ERR_TIMEOUT;
}
static int receive_http(void *user, int socket, uint8_t *data, size_t capacity,
                         uint32_t timeout_ms) {
  (void)user;
  static const char request[] =
      "POST /offer HTTP/1.1\r\nContent-Length: 5\r\n\r\nv=0\r\n";
  assert(http_active && socket == 100 && timeout_ms == 1000u);
  assert(capacity >= sizeof(request) - 1u);
  memcpy(data, request, sizeof(request) - 1u);
  return (int)sizeof(request) - 1;
}
static int send_http(void *user, int socket, const uint8_t *data, size_t size,
                      uint32_t timeout_ms) {
  (void)user;
  assert(http_active && socket == 100 && data && timeout_ms == 5000u);
  return (int)size;
}
static void *allocate(void *user, size_t size) {
  (void)user;
  return malloc(size);
}
static void release(void *user, void *value) {
  (void)user;
  free(value);
}
static int sleep_ms(void *user, uint32_t delay) {
  (void)user;
  assert(http_active && delay == 1u);
  return H2_PAL_OK;
}
static int ready_failure(h2_runtime_t *runtime) {
  assert(runtime && sockets == 5u && workers == 3u);
  return H2_PAL_ERR_IO;
}
static int udp_receive(void *user, int socket, h2_pal_net_addr_t *peer,
                       uint8_t *data, size_t capacity, uint32_t timeout_ms) {
  (void)user;
  assert(timeout_ms == 10u);
  /* Yield the controlled task at its next service receive, after the real DNS
   * parser/send/log path has completed. Cleanup subsequently runs it stopped. */
  if (socket == 5)
    longjmp(udp_iteration, 1);
  assert(socket == 4 && capacity >= dns_query_size);
  *peer = (h2_pal_net_addr_t){.family = H2_PAL_NET_FAMILY_IPV6, .port = 12345};
  peer->ip[15] = 2u;
  memcpy(data, dns_query, dns_query_size);
  return (int)dns_query_size;
}
static int udp_send(void *user, int socket, const h2_pal_net_addr_t *peer,
                    const uint8_t *data, size_t size) {
  (void)user;
  assert(socket == 4 && peer->family == H2_PAL_NET_FAMILY_IPV6);
  assert(peer->port == 12345 && peer->ip[15] == 2u);
  assert(size == dns_query_size + 28u);
  assert(data[0] == 0x12 && data[1] == 0x34);
  assert(data[2] == 0x85 && data[3] == 0x80 && data[7] == 1);
  assert(!memcmp(data + 12, dns_query + 12, dns_query_size - 12));
  static const uint8_t answer[] = {0xc0, 12, 0, 28, 0, 1, 0, 0, 0, 1, 0, 16};
  assert(!memcmp(data + dns_query_size, answer, sizeof(answer)));
  for (size_t i = dns_query_size + sizeof(answer); i < size - 1u; ++i)
    assert(data[i] == 0u);
  assert(data[size - 1u] == 1u);
  ++dns_sends;
  if (dns_scenario == 1u)
    return H2_PAL_ERR_IO;
  if (dns_scenario == 2u)
    return (int)size - 1;
  if (dns_scenario == 3u)
    return 0;
  return (int)size;
}
static int log_write(void *user, h2_pal_log_level_t level, const char *scope,
                     const char *message) {
  (void)user;
  assert(level == H2_PAL_LOG_INFO && !strcmp(scope, "ipv6-fixture"));
  if (!strncmp(message, "H2_IPV6_FIXTURE_DNS", strlen("H2_IPV6_FIXTURE_DNS"))) {
    assert(!strcmp(message,
                   "H2_IPV6_FIXTURE_DNS family=6 query_type=AAAA nonce_valid=1"));
    ++dns_witnesses;
  }
  if (!strncmp(message, "H2_IPV6_FIXTURE_STREAM_RESET ",
               strlen("H2_IPV6_FIXTURE_STREAM_RESET "))) {
    assert(http_active && forget_calls == 1u && !in_connection_loop);
    assert(strstr(message, "sid=4 incoming=1 outgoing=1"));
    ++reset_witnesses;
  }
  return H2_PAL_OK;
}
static int ready_dns(h2_runtime_t *runtime) {
  assert(runtime && sockets == 5u && workers == 3u);
  if (!setjmp(udp_iteration))
    tasks[2].entry(tasks[2].context);
  assert(dns_sends == (dns_scenario < 4u ? 1u : 0u));
  assert(dns_witnesses == (dns_scenario == 0u ? 1u : 0u));
  return H2_PAL_ERR_IO;
}
static void make_dns_query(void) {
  memset(dns_query, 0, sizeof(dns_query));
  dns_query[0] = 0x12;
  dns_query[1] = 0x34;
  dns_query[2] = 1;
  dns_query[5] = 1;
  size_t offset = 12;
  const char *labels[] = {h2_ipv6_fixture_session(), "ipv6", "test"};
  for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); ++i) {
    size_t size = strlen(labels[i]);
    dns_query[offset++] = (uint8_t)size;
    memcpy(dns_query + offset, labels[i], size);
    offset += size;
  }
  dns_query[offset++] = 0;
  dns_query[offset++] = 0;
  dns_query[offset++] = dns_scenario == 5u ? 1 : 28;
  dns_query[offset++] = 0;
  dns_query[offset++] = 1;
  dns_query_size = offset;
  if (dns_scenario == 4u)
    dns_query[13] = 'f';
}
static int ready_reset(h2_runtime_t *runtime) {
  assert(runtime && sockets == 5u && workers == 3u);
  http_active = 1;
  if (!setjmp(http_iteration))
    tasks[0].entry(tasks[0].context);
  const int success = reset_scenario < 3u;
  assert(reset_callback);
  assert(reset_calls == (reset_scenario == 1u || reset_scenario == 2u ? 2u
                         : reset_scenario == 4u || reset_scenario == 5u ? 0u : 1u));
  assert(forget_calls == (success || reset_scenario == 6u ? 1u : 0u));
  assert(reset_witnesses == (unsigned)success);
  assert(connection_closed == (unsigned)!success);
  assert(connection_destroyed == (unsigned)!success);
  http_active = 0;
  return H2_PAL_ERR_IO;
}
int main(void) {
  const h2_pal_net_vtable_t net_vtable = {.resolve_addr = resolve,
      .tcp_listen = open_socket, .udp_open_bound = open_socket,
      .close = close_socket, .udp_recvfrom = udp_receive, .udp_sendto = udp_send,
      .tcp_accept = accept_http, .tcp_recv = receive_http,
      .tcp_send_timeout = send_http};
  const h2_pal_net_api_t net = {.vtable = &net_vtable};
  const h2_pal_task_vtable_t task_vtable = {.start = start, .join = join};
  const h2_pal_task_api_t task = {.vtable = &task_vtable};
  const h2_pal_sync_vtable_t sync_vtable = {.create_mutex = create_mutex,
      .destroy_mutex = destroy_mutex, .lock_mutex = mutex_op,
      .unlock_mutex = mutex_op};
  const h2_pal_sync_api_t sync = {.vtable = &sync_vtable};
  const h2_pal_time_vtable_t time_vtable = {.sleep_ms = sleep_ms};
  const h2_pal_time_api_t time = {.vtable = &time_vtable};
  const h2_pal_mem_vtable_t mem_vtable = {.alloc = allocate, .free = release};
  const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};
  const h2_pal_crypto_api_t crypto = {0};
  const h2_pal_dtls_api_t dtls = {0};
  const h2_ipv6_tls_server_api_t tls = {0};
  const h2_pal_log_vtable_t log_vtable = {.write = log_write};
  const h2_pal_log_api_t log = {.vtable = &log_vtable};
  h2_runtime_t runtime = {.net = &net, .task = &task, .sync = &sync,
      .time = &time, .mem = &mem, .crypto = &crypto, .log = &log};
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
    assert(h2_ipv6_board_fixture_ownership_released());
    if (scenario == 11u)
      assert(joins == 4u);
  }
  fail_mutex = fail_provider = fail_peer = 0;
  fail_open = fail_start = fail_join = 0u;
  for (dns_scenario = 0; dns_scenario < 6u; ++dns_scenario) {
    starts = opens = joins = dns_sends = dns_witnesses = 0u;
    make_dns_query();
    assert(h2_ipv6_board_fixture_run(&runtime, &dtls, &tls, ready_dns) == H2_PAL_ERR_IO);
    assert(!sockets && !workers && !providers && !peers && !mutex.live);
    assert(h2_ipv6_board_fixture_ownership_released());
  }
  for (reset_scenario = 0; reset_scenario < 7u; ++reset_scenario) {
    starts = opens = joins = 0u;
    http_offered = connection_round = connection_closed = connection_destroyed = 0u;
    reset_calls = forget_calls = reset_witnesses = 0u;
    assert(h2_ipv6_board_fixture_run(&runtime, &dtls, &tls, ready_reset) == H2_PAL_ERR_IO);
    assert(connection_closed == 1u && connection_destroyed == 1u);
    assert(!sockets && !workers && !providers && !peers && !mutex.live);
    assert(h2_ipv6_board_fixture_ownership_released());
  }
  starts = opens = joins = 0u;
  fail_join = 9u;
  assert(h2_ipv6_board_fixture_run(&runtime, &dtls, &tls, ready_failure) == H2_PAL_ERR_TIMEOUT);
  assert(!h2_ipv6_board_fixture_ownership_released());
  assert(sockets == 5u && workers == 3u && providers == 1u && peers == 1u && mutex.live);
  assert(joins == 9u);
  assert(h2_ipv6_board_fixture_run(&runtime, &dtls, &tls, ready_failure) == H2_PAL_ERR_INVALID_STATE);
  puts("board fixture startup: 12 released paths; persistent join failure retains ownership and dependencies");
  puts("board fixture DNS: complete send alone publishes evidence; error/short/zero sends and invalid nonce/type do not");
  puts("board fixture reset: callback registration, deferred reciprocal reset, busy/retry, complete retirement, and failure cleanup");
  return 0;
}
