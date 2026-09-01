/*
 * Official iperf3 client (`iperf3 -c`) against the PAL server. Covers TCP and
 * UDP in both directions plus a request the PAL server must reject cleanly.
 */
#include "h2_iperf_test_support.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define CLIENT_EXIT_TIMEOUT_MS 15000u

typedef struct server_thread {
    h2_iperf_server_t *server;
    h2_iperf_result_t result;
    h2_pal_result_t status;
} server_thread_t;

static void *server_main(void *arg) {
    server_thread_t *thread = arg;
    thread->status = h2_iperf_server_run_once(thread->server, 10000u, &thread->result);
    return NULL;
}

typedef struct scenario {
    const char *name;
    const char *protocol_flag; /* NULL for TCP */
    bool reverse;
    bool expect_reject;
    const char *extra_flag;
    const char *extra_value;
} scenario_t;

static void run_scenario(const char *iperf3, const scenario_t *scenario) {
    h2_iperf_test_env_t env;
    h2_iperf_test_env_init(&env, false);
    const h2_iperf_server_params_t server_params = {
        .family = H2_PAL_NET_FAMILY_IPV4,
        .ephemeral_port = true,
        .control_timeout_ms = 10000u,
    };
    server_thread_t thread;
    memset(&thread, 0, sizeof(thread));
    assert(h2_iperf_server_create(&env.config, &server_params, &thread.server) == H2_PAL_OK);
    char port_text[8];
    snprintf(port_text, sizeof(port_text), "%u", (unsigned)h2_iperf_server_port(thread.server));
    pthread_t handle;
    assert(pthread_create(&handle, NULL, server_main, &thread) == 0);

    char *argv[16];
    int argc = 0;
    argv[argc++] = (char *)iperf3;
    argv[argc++] = "-c";
    argv[argc++] = "127.0.0.1";
    argv[argc++] = "-p";
    argv[argc++] = port_text;
    argv[argc++] = "-t";
    argv[argc++] = "1";
    argv[argc++] = "-i";
    argv[argc++] = "0";
    if (scenario->protocol_flag != NULL) {
        argv[argc++] = (char *)scenario->protocol_flag;
    }
    if (scenario->reverse) {
        argv[argc++] = "-R";
    }
    if (scenario->extra_flag != NULL) {
        argv[argc++] = (char *)scenario->extra_flag;
        if (scenario->extra_value != NULL) {
            argv[argc++] = (char *)scenario->extra_value;
        }
    }
    argv[argc] = NULL;
    pid_t client = h2_iperf_test_spawn(argv);
    int exit_code = h2_iperf_test_wait(client, CLIENT_EXIT_TIMEOUT_MS);
    assert(pthread_join(handle, NULL) == 0);

    printf("== %s\n", scenario->name);
    printf("iperf3 exit %d, server status %d\n", exit_code, (int)thread.status);
    h2_iperf_test_print_result("server", &thread.result);
    if (scenario->expect_reject) {
        assert(exit_code != 0);
        assert(thread.status == H2_PAL_ERR_UNSUPPORTED);
    } else {
        assert(exit_code == 0);
        assert(thread.status == H2_PAL_OK);
        assert(thread.result.reverse == scenario->reverse);
        const h2_iperf_stream_stats_t *sender = scenario->reverse ? &thread.result.local : &thread.result.remote;
        const h2_iperf_stream_stats_t *receiver = scenario->reverse ? &thread.result.remote : &thread.result.local;
        assert(sender->bytes > 0u);
        assert(receiver->bytes > 0u);
        /* iperf3 cancels its sender thread mid-write, so the last block can be
         * on the wire without being counted: allow one block of slack. */
        assert(receiver->bytes <= sender->bytes + H2_IPERF_DEFAULT_TCP_BLOCK_LEN);
        /* Loopback keeps almost everything for reliable transports; UDP on a
         * loaded CI host can drop a large share, so only a coarse bound applies. */
        if (scenario->protocol_flag != NULL) {
            assert(receiver->bytes * 2u >= sender->bytes);
        } else {
            assert(receiver->bytes * 10u >= sender->bytes * 9u);
        }
        if (scenario->protocol_flag != NULL) {
            assert(sender->packets > 0u);
            assert(receiver->packets > 0u);
            assert(receiver->lost_packets >= 0);
            assert(receiver->lost_packets * 2 <= (int64_t)sender->packets);
        }
    }
    h2_iperf_server_destroy(&thread.server);
    h2_iperf_test_env_deinit(&env);
}

int main(int argc, char **argv) {
    assert(argc >= 2);
    static const scenario_t scenarios[] = {
        {"iperf3 -c tcp", NULL, false, false, NULL, NULL},
        {"iperf3 -c tcp -R", NULL, true, false, NULL, NULL},
        {"iperf3 -c udp", "-u", false, false, "-b", "4M"},
        {"iperf3 -c udp -R", "-u", true, false, "-b", "4M"},
        {"iperf3 -c udp 64-bit counters", "-u", false, false, "--udp-counters-64bit", NULL},
        {"iperf3 -c -P 2 rejected", NULL, false, true, "-P", "2"},
    };
    for (size_t i = 0u; i < sizeof(scenarios) / sizeof(scenarios[0]); ++i) {
        run_scenario(argv[1], &scenarios[i]);
    }
    return 0;
}
