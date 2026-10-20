/*
 * PAL client against PAL server over loopback for TCP, UDP, and SCTP in both
 * directions. The server runs on a second thread; both sides own independent
 * PAL providers (including separate H2SCTP instances).
 */
#include "h2_iperf_internal.h"
#include "h2_iperf_test_support.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define SERVER_ACCEPT_TIMEOUT_MS 10000u

/* Only PAL inputs are intercepted: SCTP, emit accounting and pump are real. */
static struct {
  const h2_pal_net_api_t *net;
  const h2_pal_sctp_api_t *sctp;
  h2_pal_net_vtable_t net_vtable;
  h2_pal_net_api_t net_api;
  h2_pal_sctp_vtable_t sctp_vtable;
  h2_pal_sctp_api_t sctp_api;
  h2_pal_time_vtable_t time_vtable;
  h2_pal_time_api_t time_api;
  unsigned left, sends, sleeps, reads;
  uint64_t now;
  int error;
  bool forward, in_service, fail_sleep;
} pressure;

static int pressure_send(void *user, h2_pal_net_socket_t sock,
                         const h2_pal_net_addr_t *addr, const uint8_t *data,
                         size_t len) {
  (void)user;
  ++pressure.sends;
  if (pressure.left != 0u) {
    --pressure.left;
    return pressure.error;
  }
  return pressure.forward
             ? h2_pal_net_udp_sendto(pressure.net, sock, addr, data, len)
             : (int)len;
}

static int pressure_recv(void *user, h2_pal_net_socket_t sock,
                         h2_pal_net_addr_t *addr, uint8_t *data, size_t len,
                         uint32_t wait) {
  (void)user;
  return pressure.forward ? h2_pal_net_udp_recvfrom(pressure.net, sock, addr,
                                                    data, len, wait)
                          : H2_PAL_ERR_WOULD_BLOCK;
}

static void pressure_net(h2_iperf_config_t *config) {
  pressure.net = config->net;
  pressure.net_vtable = *config->net->vtable;
  pressure.net_vtable.udp_sendto = pressure_send;
  pressure.net_vtable.udp_recvfrom = pressure_recv;
  pressure.net_api = (h2_pal_net_api_t){.user = config->net->user,
                                        .vtable = &pressure.net_vtable};
  config->net = &pressure.net_api;
}

static h2_pal_result_t pressure_clock(void *user, uint64_t *now) {
  (void)user;
  assert(++pressure.reads <=
         8u); /* Frozen clock makes a fallback spin fail promptly. */
  *now = pressure.now;
  return H2_PAL_OK;
}

static h2_pal_result_t pressure_sleep(void *user, uint32_t ms) {
  (void)user;
  assert(!pressure.in_service && ms == 1u);
  ++pressure.sleeps;
  pressure.now += pressure.fail_sleep ? 0u : ms;
  return pressure.fail_sleep ? H2_PAL_ERR_IO : H2_PAL_OK;
}

static h2_pal_result_t pressure_service(void *user,
                                        h2_pal_sctp_association_t *assoc,
                                        uint64_t now, uint64_t *next) {
  (void)user;
  pressure.in_service = true;
  h2_pal_result_t rc =
      h2_pal_sctp_association_service(pressure.sctp, assoc, now, next);
  pressure.in_service = false;
  return rc;
}

static void pressure_init(h2_iperf_test_env_t *env, h2_iperf_stream_t *stream,
                          int error) {
  memset(&pressure, 0, sizeof(pressure));
  pressure.left = 1001u;
  pressure.error = error;
  h2_iperf_test_env_init(env, true);
  pressure_net(&env->config);
  pressure.sctp = env->config.sctp;
  pressure.sctp_vtable = *pressure.sctp->vtable;
  pressure.sctp_vtable.association_service = pressure_service;
  pressure.sctp_api = (h2_pal_sctp_api_t){.user = pressure.sctp->user,
                                          .vtable = &pressure.sctp_vtable};
  env->config.sctp = &pressure.sctp_api;
  pressure.time_vtable = (h2_pal_time_vtable_t){
      .get_monotonic_ms = pressure_clock, .sleep_ms = pressure_sleep};
  pressure.time_api = (h2_pal_time_api_t){.vtable = &pressure.time_vtable};
  env->config.time = &pressure.time_api;
  h2_iperf_stream_init(stream, &env->config, H2_IPERF_PROTOCOL_SCTP, NULL);
  stream->sock = 1;
  stream->peer_known = true;
  stream->peer = h2_iperf_test_loopback(1u);
  assert(h2_iperf_stream_sctp_create(stream, H2_PAL_SCTP_ROLE_ACTIVE, 5000u,
                                     5001u, 1200u, 1024u) == H2_PAL_OK);
}

static void test_sctp_backpressure_bounds(void) {
  h2_iperf_test_env_t env;
  h2_iperf_stream_t stream;
  for (unsigned mode = 0u; mode < 2u; ++mode) {
    pressure_init(&env, &stream,
                  mode == 0u ? H2_PAL_ERR_WOULD_BLOCK : H2_PAL_ERR_IO);
    assert(h2_pal_sctp_association_start(env.config.sctp, stream.assoc, 0u) ==
           H2_PAL_OK);
    h2_pal_result_t rc = H2_PAL_OK;
    while (rc == H2_PAL_OK && pressure.sends < 1001u) {
      pressure.reads = 0u;
      rc = h2_iperf_stream_sctp_pump(&stream, 1u);
    }
    assert(rc == H2_PAL_ERR_IO && pressure.sends == 1001u);
    assert(mode == 0u ? pressure.sleeps > 0u : pressure.sleeps == 0u);
    h2_iperf_stream_close(&stream);
    h2_iperf_test_env_deinit(&env);
  }
  for (unsigned mode = 0u; mode < 3u; ++mode) {
    pressure_init(&env, &stream, H2_PAL_ERR_WOULD_BLOCK);
    assert(h2_pal_sctp_association_start(env.config.sctp, stream.assoc, 0u) ==
           H2_PAL_OK);
    if (mode == 1u)
      pressure.time_vtable.sleep_ms = NULL;
    pressure.fail_sleep = mode == 2u;
    assert(h2_iperf_stream_sctp_pump(&stream, mode == 0u ? 0u : 1u) ==
           H2_PAL_OK);
    assert(pressure.sleeps == (mode == 2u ? 1u : 0u));
    h2_iperf_stream_close(&stream);
    h2_iperf_test_env_deinit(&env);
  }
  pressure_init(&env, &stream, H2_PAL_ERR_WOULD_BLOCK);
  assert(h2_iperf_stream_sctp_connect(&stream, 2u) == H2_PAL_ERR_TIMEOUT);
  assert(pressure.now == 2u && pressure.sends < 1001u);
  h2_iperf_stream_close(&stream);
  h2_iperf_test_env_deinit(&env);
}

typedef struct server_thread {
    h2_iperf_server_t *server;
    h2_iperf_result_t result;
    h2_pal_result_t status;
} server_thread_t;

static void *server_main(void *arg) {
    server_thread_t *thread = arg;
    thread->status = h2_iperf_server_run_once(
        thread->server, SERVER_ACCEPT_TIMEOUT_MS, &thread->result);
    return NULL;
}

typedef struct scenario {
    const char *name;
    h2_iperf_protocol_t protocol;
    bool reverse;
    uint32_t duration_ms;
    uint64_t bytes;
    uint32_t block_len;
    uint64_t bitrate_bps;
} scenario_t;

static void run_scenario(const scenario_t *scenario) {
    bool with_sctp = scenario->protocol == H2_IPERF_PROTOCOL_SCTP;
    h2_iperf_test_env_t server_env;
    h2_iperf_test_env_t client_env;
    h2_iperf_test_env_init(&server_env, with_sctp);
    h2_iperf_test_env_init(&client_env, with_sctp);
    if (pressure.forward) {
      pressure_net(&client_env.config);
    }

    const h2_iperf_server_params_t server_params = {
        .family = H2_PAL_NET_FAMILY_IPV4,
        .ephemeral_port = true,
        .ephemeral_sctp_udp_port = true,
        .control_timeout_ms = 10000u,
    };
    server_thread_t thread;
    memset(&thread, 0, sizeof(thread));
    assert(h2_iperf_server_create(&server_env.config, &server_params, &thread.server) == H2_PAL_OK);
    uint16_t port = h2_iperf_server_port(thread.server);
    assert(port != 0u);
    if (with_sctp) {
        assert(h2_iperf_server_sctp_udp_port(thread.server) != 0u);
    }
    pthread_t handle;
    assert(pthread_create(&handle, NULL, server_main, &thread) == 0);

    h2_iperf_client_params_t params;
    memset(&params, 0, sizeof(params));
    params.server_addr = h2_iperf_test_loopback(0u);
    params.port = port;
    params.protocol = scenario->protocol;
    params.reverse = scenario->reverse;
    params.duration_ms = scenario->duration_ms;
    params.bytes = scenario->bytes;
    params.block_len = scenario->block_len;
    params.bitrate_bps = scenario->bitrate_bps;
    params.sctp_udp_port = h2_iperf_server_sctp_udp_port(thread.server);
    params.control_timeout_ms = 10000u;
    h2_iperf_result_t client_result;
    h2_pal_result_t status = h2_iperf_client_run(&client_env.config, &params, &client_result);
    assert(pthread_join(handle, NULL) == 0);

    printf("== %s\n", scenario->name);
    printf("client status %d, server status %d\n", (int)status, (int)thread.status);
    h2_iperf_test_print_result("client", &client_result);
    h2_iperf_test_print_result("server", &thread.result);
    assert(status == H2_PAL_OK);
    if (pressure.forward) {
      assert(pressure.left == 0u && pressure.sends > 3u);
    }
    assert(thread.status == H2_PAL_OK);
    assert(memcmp(client_result.cookie, thread.result.cookie, H2_IPERF_COOKIE_SIZE) == 0);
    assert(client_result.protocol == scenario->protocol);
    assert(thread.result.protocol == scenario->protocol);
    assert(thread.result.reverse == scenario->reverse);

    const h2_iperf_stream_stats_t *sender = scenario->reverse ? &thread.result.local : &client_result.local;
    const h2_iperf_stream_stats_t *receiver = scenario->reverse ? &client_result.local : &thread.result.local;
    assert(sender->bytes > 0u);
    assert(receiver->bytes > 0u);
    assert(receiver->bytes <= sender->bytes);
    /* Loopback keeps almost everything for reliable transports; UDP on a
     * loaded CI host can drop a large share, so only a coarse bound applies. */
    if (scenario->protocol == H2_IPERF_PROTOCOL_UDP) {
        assert(receiver->bytes * 2u >= sender->bytes);
    } else {
        assert(receiver->bytes * 10u >= sender->bytes * 9u);
    }
    /* The peer's report is exactly what the other side measured locally. */
    assert(client_result.remote.bytes == thread.result.local.bytes);
    assert(thread.result.remote.bytes == client_result.local.bytes);
    if (scenario->bytes != 0u) {
        assert(sender->bytes == scenario->bytes);
    } else {
        assert(sender->duration_ms >= scenario->duration_ms);
        assert(sender->duration_ms < scenario->duration_ms + 1000u);
    }
    if (scenario->protocol == H2_IPERF_PROTOCOL_UDP) {
        assert(sender->packets > 0u);
        assert(receiver->packets > 0u);
        assert(receiver->lost_packets >= 0);
        assert(receiver->lost_packets * 2 <= (int64_t)sender->packets);
        assert(receiver->packets + (uint64_t)receiver->lost_packets <= sender->packets);
        if (scenario->bitrate_bps != 0u) {
            /* The pacer sends a datagram only while the bits already sent fit
             * the elapsed time, so at most one datagram runs ahead of the
             * rate. duration_ms truncates the elapsed time, which is shorter
             * than duration_ms + 1: a 3005-byte budget that ends at 2.9 ms
             * reports 2 ms and a 12 Mbit/s rate against 8 Mbit/s. Bound the
             * bytes by that contract instead of dividing by the truncated
             * duration. */
            uint64_t datagram = scenario->block_len != 0u
                ? scenario->block_len
                : H2_IPERF_DEFAULT_UDP_BLOCK_LEN;
            uint64_t allowed_bits =
                scenario->bitrate_bps * ((uint64_t)sender->duration_ms + 1u) / 1000u;
            assert(sender->bytes * 8u <= allowed_bits + datagram * 8u);
        }
    }
    if (scenario->protocol == H2_IPERF_PROTOCOL_SCTP) {
        assert(sender->packets > 0u);
        assert(receiver->packets > 0u);
        assert(receiver->packets <= sender->packets);
    }

    h2_iperf_server_destroy(&thread.server);
    assert(thread.server == NULL);
    h2_iperf_test_env_deinit(&client_env);
    h2_iperf_test_env_deinit(&server_env);
}

/* A UDP byte limit smaller than the datagram header cannot be represented
 * exactly, so the client rejects it before touching the network. */
static void test_udp_limit_below_header_rejected(void) {
    h2_iperf_test_env_t env;
    h2_iperf_test_env_init(&env, false);
    h2_iperf_client_params_t params;
    memset(&params, 0, sizeof(params));
    params.server_addr = h2_iperf_test_loopback(0u);
    params.port = 1u;
    params.protocol = H2_IPERF_PROTOCOL_UDP;
    params.bytes = 11u;
    h2_iperf_result_t result;
    assert(h2_iperf_client_run(&env.config, &params, &result) == H2_PAL_ERR_INVALID_ARG);
    h2_iperf_test_env_deinit(&env);
}

int main(void) {
    static const scenario_t scenarios[] = {
        {"tcp", H2_IPERF_PROTOCOL_TCP, false, 1000u, 0u, 0u, 0u},
        {"tcp bytes", H2_IPERF_PROTOCOL_TCP, false, 0u, 4u * 1024u * 1024u, 0u, 0u},
        {"tcp bytes unaligned", H2_IPERF_PROTOCOL_TCP, false, 0u, 4u * 1024u * 1024u + 12345u, 0u, 0u},
        {"tcp one byte", H2_IPERF_PROTOCOL_TCP, false, 0u, 1u, 0u, 0u},
        {"tcp reverse", H2_IPERF_PROTOCOL_TCP, true, 1000u, 0u, 0u, 0u},
        {"udp", H2_IPERF_PROTOCOL_UDP, false, 1000u, 0u, 0u, 2u * 1024u * 1024u},
        {"udp reverse", H2_IPERF_PROTOCOL_UDP, true, 1000u, 0u, 1200u, 2u * 1024u * 1024u},
        {"udp reverse large datagrams", H2_IPERF_PROTOCOL_UDP, true, 1000u, 0u, 16332u, 8u * 1024u * 1024u},
        {"udp large datagrams", H2_IPERF_PROTOCOL_UDP, false, 1000u, 0u, 16332u, 8u * 1024u * 1024u},
        {"udp bytes unaligned", H2_IPERF_PROTOCOL_UDP, false, 0u, 200u * 1000u + 500u, 1000u, 8u * 1024u * 1024u},
        {"udp bytes remainder below header", H2_IPERF_PROTOCOL_UDP, false, 0u, 3u * 1000u + 5u, 1000u, 8u * 1024u * 1024u},
        {"udp bytes exactly one header", H2_IPERF_PROTOCOL_UDP, false, 0u, 12u, 1000u, 8u * 1024u * 1024u},
        {"sctp", H2_IPERF_PROTOCOL_SCTP, false, 1000u, 0u, 16u * 1024u, 0u},
        /* The byte budget must be several association windows: the sender
         * counts what it queued, so a transfer no larger than the window ends
         * with most of it still in flight and the two counts disagree. */
        {"sctp bytes", H2_IPERF_PROTOCOL_SCTP, false, 0u, 4u * 1024u * 1024u, 8u * 1024u, 0u},
        {"sctp bytes unaligned", H2_IPERF_PROTOCOL_SCTP, false, 0u, 4u * 1024u * 1024u + 999u, 8u * 1024u, 0u},
        {"sctp reverse", H2_IPERF_PROTOCOL_SCTP, true, 1000u, 0u, 16u * 1024u, 0u},
        {"sctp paced", H2_IPERF_PROTOCOL_SCTP, false, 1000u, 0u, 1024u, 4u * 1024u * 1024u},
    };
    for (size_t i = 0u; i < sizeof(scenarios) / sizeof(scenarios[0]); ++i) {
      run_scenario(&scenarios[i]);
    }
    const scenario_t recovered = {"sctp WB recovery",
                                  H2_IPERF_PROTOCOL_SCTP,
                                  false,
                                  0u,
                                  4u * 1024u * 1024u,
                                  8u * 1024u,
                                  0u};
    memset(&pressure, 0, sizeof(pressure));
    pressure.left = 3u;
    pressure.error = H2_PAL_ERR_WOULD_BLOCK;
    pressure.forward = true;
    run_scenario(&recovered);
    test_sctp_backpressure_bounds();
    test_udp_limit_below_header_rejected();
    return 0;
}
