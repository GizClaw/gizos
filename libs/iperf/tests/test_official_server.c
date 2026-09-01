/*
 * PAL client against the official iperf3 server (`iperf3 -s -1`) built from
 * the vendored upstream sources. Covers TCP and UDP in both directions; the
 * official binary has no kernel SCTP on the hosts this test runs on.
 */
#include "h2_iperf_test_support.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define SERVER_EXIT_TIMEOUT_MS 15000u

typedef struct scenario {
    const char *name;
    h2_iperf_protocol_t protocol;
    bool reverse;
    uint64_t bytes;
    uint64_t bitrate_bps;
} scenario_t;

static void run_scenario(const char *iperf3, const scenario_t *scenario) {
    h2_iperf_test_env_t env;
    h2_iperf_test_env_init(&env, false);
    uint16_t port = h2_iperf_test_free_port(env.config.net);
    char port_text[8];
    snprintf(port_text, sizeof(port_text), "%u", (unsigned)port);
    char *const argv[] = {(char *)iperf3, "-s", "-1", "-p", port_text, "-i", "0", NULL};
    pid_t server = h2_iperf_test_spawn(argv);

    h2_iperf_client_params_t params;
    memset(&params, 0, sizeof(params));
    params.server_addr = h2_iperf_test_loopback(0u);
    params.port = port;
    params.protocol = scenario->protocol;
    params.reverse = scenario->reverse;
    params.duration_ms = scenario->bytes != 0u ? 0u : 1000u;
    params.bytes = scenario->bytes;
    params.bitrate_bps = scenario->bitrate_bps;
    params.connect_timeout_ms = 5000u;
    params.control_timeout_ms = 10000u;
    h2_iperf_result_t result;
    h2_pal_result_t status = h2_iperf_client_run(&env.config, &params, &result);
    int exit_code = h2_iperf_test_wait(server, SERVER_EXIT_TIMEOUT_MS);

    printf("== %s\n", scenario->name);
    printf("client status %d, iperf3 exit %d\n", (int)status, exit_code);
    h2_iperf_test_print_result("client", &result);
    assert(status == H2_PAL_OK);
    assert(exit_code == 0);

    const h2_iperf_stream_stats_t *sender = scenario->reverse ? &result.remote : &result.local;
    const h2_iperf_stream_stats_t *receiver = scenario->reverse ? &result.local : &result.remote;
    assert(sender->bytes > 0u);
    assert(receiver->bytes > 0u);
    assert(receiver->bytes <= sender->bytes);
    assert(receiver->bytes * 10u >= sender->bytes * 9u);
    if (scenario->bytes != 0u) {
        assert(sender->bytes == scenario->bytes);
    }
    if (scenario->protocol == H2_IPERF_PROTOCOL_UDP) {
        assert(sender->packets > 0u);
        assert(receiver->packets > 0u);
        assert(receiver->lost_packets >= 0);
        assert(receiver->lost_packets * 10 <= (int64_t)sender->packets);
    }
    h2_iperf_test_env_deinit(&env);
}

int main(int argc, char **argv) {
    assert(argc >= 2);
    static const scenario_t scenarios[] = {
        {"tcp -> iperf3 -s", H2_IPERF_PROTOCOL_TCP, false, 0u, 0u},
        {"tcp bytes -> iperf3 -s", H2_IPERF_PROTOCOL_TCP, false, 8u * 1024u * 1024u, 0u},
        {"tcp reverse <- iperf3 -s", H2_IPERF_PROTOCOL_TCP, true, 0u, 0u},
        {"udp -> iperf3 -s", H2_IPERF_PROTOCOL_UDP, false, 0u, 4u * 1024u * 1024u},
        {"udp reverse <- iperf3 -s", H2_IPERF_PROTOCOL_UDP, true, 0u, 4u * 1024u * 1024u},
    };
    for (size_t i = 0u; i < sizeof(scenarios) / sizeof(scenarios[0]); ++i) {
        run_scenario(argv[1], &scenarios[i]);
    }
    return 0;
}
