/*
 * PAL client against PAL server over loopback for TCP, UDP, and SCTP in both
 * directions. The server runs on a second thread; both sides own independent
 * PAL providers (including separate H2SCTP instances).
 */
#include "h2_iperf_test_support.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define SERVER_ACCEPT_TIMEOUT_MS 10000u

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
    /* Loopback keeps almost everything; TEST_END may race the last block. */
    assert(receiver->bytes * 10u >= sender->bytes * 9u);
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
        assert(receiver->lost_packets * 10 <= (int64_t)sender->packets);
        assert(receiver->packets + (uint64_t)receiver->lost_packets <= sender->packets);
        if (scenario->bitrate_bps != 0u) {
            uint64_t bps = h2_iperf_stats_bits_per_second(sender);
            assert(bps <= scenario->bitrate_bps + scenario->bitrate_bps / 5u);
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

int main(void) {
    static const scenario_t scenarios[] = {
        {"tcp", H2_IPERF_PROTOCOL_TCP, false, 1000u, 0u, 0u, 0u},
        {"tcp bytes", H2_IPERF_PROTOCOL_TCP, false, 0u, 4u * 1024u * 1024u, 0u, 0u},
        {"tcp reverse", H2_IPERF_PROTOCOL_TCP, true, 1000u, 0u, 0u, 0u},
        {"udp", H2_IPERF_PROTOCOL_UDP, false, 1000u, 0u, 0u, 2u * 1024u * 1024u},
        {"udp reverse", H2_IPERF_PROTOCOL_UDP, true, 1000u, 0u, 1200u, 2u * 1024u * 1024u},
        {"udp reverse large datagrams", H2_IPERF_PROTOCOL_UDP, true, 1000u, 0u, 16332u, 8u * 1024u * 1024u},
        {"udp large datagrams", H2_IPERF_PROTOCOL_UDP, false, 1000u, 0u, 16332u, 8u * 1024u * 1024u},
        {"sctp", H2_IPERF_PROTOCOL_SCTP, false, 1000u, 0u, 16u * 1024u, 0u},
        {"sctp bytes", H2_IPERF_PROTOCOL_SCTP, false, 0u, 512u * 1024u, 8u * 1024u, 0u},
        {"sctp reverse", H2_IPERF_PROTOCOL_SCTP, true, 1000u, 0u, 16u * 1024u, 0u},
        {"sctp paced", H2_IPERF_PROTOCOL_SCTP, false, 1000u, 0u, 1024u, 4u * 1024u * 1024u},
    };
    for (size_t i = 0u; i < sizeof(scenarios) / sizeof(scenarios[0]); ++i) {
        run_scenario(&scenarios[i]);
    }
    return 0;
}
