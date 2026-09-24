/*
 * PAL client against the official iperf3 server (`iperf3 -s -1`) built from
 * the vendored upstream sources. Covers TCP and UDP in both directions; the
 * official binary has no kernel SCTP on the hosts this test runs on.
 *
 * The server port comes from a probe that is released before iperf3 binds, so
 * another socket can take it in between. Each server is therefore started on
 * 127.0.0.1 only and the client waits for iperf3's listener banner. A control
 * listener bind failure restarts iperf3 on a fresh port. A UDP stream listener
 * bind failure, which iperf3 hits only after the client connected, reruns the
 * scenario on a fresh port. Both retries share one bounded budget.
 */
#include "h2_iperf_test_support.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <arpa/inet.h>
#include <assert.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define SERVER_EXIT_TIMEOUT_MS 15000u
#define SERVER_READY_TIMEOUT_MS 10000u
#define SERVER_READY_POLL_MS 10u
#define SERVER_START_ATTEMPTS 5u
#define SERVER_LOG_CAP 4096u

typedef enum busy_port {
    BUSY_PORT_NONE = 0,
    /* TCP taken: iperf3 cannot start its control listener. */
    BUSY_PORT_TCP,
    /* Only UDP taken: iperf3 listens, then fails its UDP stream listener. */
    BUSY_PORT_UDP,
} busy_port_t;

typedef struct scenario {
    const char *name;
    h2_iperf_protocol_t protocol;
    bool reverse;
    uint64_t bytes;
    uint64_t bitrate_bps;
    /* Holds the first candidate port so iperf3 must fail and retry. */
    busy_port_t busy_first_port;
} scenario_t;

typedef struct official_server {
    pid_t pid;
    uint16_t port;
    unsigned attempts;
    /* Set by official_server_finish(). */
    bool stream_listener_failed;
    int log_fd;
    char log_path[PATH_MAX];
} official_server_t;

static void server_log_open(official_server_t *server) {
    const char *dir = getenv("TEST_TMPDIR");
    if (dir == NULL || dir[0] == '\0') {
        dir = "/tmp";
    }
    int len = snprintf(server->log_path, sizeof(server->log_path), "%s/iperf3-server-XXXXXX", dir);
    assert(len > 0 && (size_t)len < sizeof(server->log_path));
    server->log_fd = mkstemp(server->log_path);
    assert(server->log_fd >= 0);
}

/* Reads the start of the iperf3 log as a NUL-terminated string. */
static void server_log_read(const official_server_t *server, char *buf, size_t cap) {
    size_t len = 0u;
    while (len + 1u < cap) {
        ssize_t got = pread(server->log_fd, buf + len, cap - 1u - len, (off_t)len);
        if (got <= 0) {
            break;
        }
        len += (size_t)got;
    }
    buf[len] = '\0';
}

/* Copies the whole iperf3 log to the test log, then removes it. */
static void server_log_close(official_server_t *server) {
    char buf[SERVER_LOG_CAP];
    off_t offset = 0;
    for (;;) {
        ssize_t got = pread(server->log_fd, buf, sizeof(buf), offset);
        if (got <= 0) {
            break;
        }
        fwrite(buf, 1u, (size_t)got, stdout);
        offset += got;
    }
    fflush(stdout);
    close(server->log_fd);
    unlink(server->log_path);
    server->log_fd = -1;
}

/* True once iperf3 printed its listener banner; false when it exited first. */
static bool server_wait_listening(const official_server_t *server) {
    char log[SERVER_LOG_CAP];
    for (uint32_t waited = 0u;; waited += SERVER_READY_POLL_MS) {
        server_log_read(server, log, sizeof(log));
        if (strstr(log, "Server listening on") != NULL) {
            return true;
        }
        int status = 0;
        if (waitpid(server->pid, &status, WNOHANG) == server->pid) {
            return false;
        }
        if (waited >= SERVER_READY_TIMEOUT_MS) {
            (void)kill(server->pid, SIGKILL);
            (void)waitpid(server->pid, &status, 0);
            return false;
        }
        h2_iperf_test_sleep_ms(SERVER_READY_POLL_MS);
    }
}

/*
 * Starts `iperf3 -s -1` on 127.0.0.1 and returns once it is listening. The
 * first attempt uses `first_port` when non-zero; every other attempt probes a
 * fresh port. Only a listener bind failure is retried, up to `max_attempts`.
 */
static void official_server_start(
    const h2_pal_net_api_t *net,
    const char *iperf3,
    uint16_t first_port,
    unsigned max_attempts,
    official_server_t *server) {
    memset(server, 0, sizeof(*server));
    server->log_fd = -1;
    for (unsigned attempt = 0u; attempt < max_attempts; ++attempt) {
        server->attempts = attempt + 1u;
        server->port = (attempt == 0u && first_port != 0u) ? first_port : h2_iperf_test_free_port(net);
        char port_text[8];
        snprintf(port_text, sizeof(port_text), "%u", (unsigned)server->port);
        char *const argv[] = {
            (char *)iperf3, "-s", "-1", "-B", "127.0.0.1", "-p", port_text, "-i", "0", "--forceflush", NULL,
        };
        server_log_open(server);
        server->pid = h2_iperf_test_spawn_redirected(argv, server->log_fd);
        if (server_wait_listening(server)) {
            return;
        }
        char log[SERVER_LOG_CAP];
        server_log_read(server, log, sizeof(log));
        server_log_close(server);
        bool bind_failed = strstr(log, "unable to start listener") != NULL;
        printf("iperf3 -s on port %u did not start (attempt %u)%s\n",
               (unsigned)server->port, server->attempts, bind_failed ? "; retrying on a fresh port" : "");
        fflush(stdout);
        assert(bind_failed);
    }
    assert(!"iperf3 -s could not bind a listener port");
}

/* Waits for the one-shot server to exit and copies its output to the test log. */
static int official_server_finish(official_server_t *server) {
    int exit_code = h2_iperf_test_wait(server->pid, SERVER_EXIT_TIMEOUT_MS);
    char log[SERVER_LOG_CAP];
    server_log_read(server, log, sizeof(log));
    server->stream_listener_failed = strstr(log, "unable to start stream listener") != NULL;
    server_log_close(server);
    return exit_code;
}

/* The client leaves params.block_len at 0, so iperf3 sends blocks of the
 * library default for the protocol. */
static uint64_t scenario_block_len(const scenario_t *scenario) {
    return scenario->protocol == H2_IPERF_PROTOCOL_UDP
        ? H2_IPERF_DEFAULT_UDP_BLOCK_LEN
        : H2_IPERF_DEFAULT_TCP_BLOCK_LEN;
}

/* Binds 127.0.0.1:port for UDP without SO_REUSEADDR, so iperf3 cannot share it. */
static int hold_udp_port(uint16_t port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(fd >= 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    return fd;
}

static void run_scenario(const char *iperf3, const scenario_t *scenario) {
    h2_iperf_test_env_t env;
    h2_iperf_test_env_init(&env, false);
    h2_pal_net_socket_t busy_tcp = -1;
    int busy_udp = -1;
    uint16_t first_port = 0u;
    if (scenario->busy_first_port == BUSY_PORT_TCP) {
        const h2_pal_net_bind_t loopback = {
            .type = H2_PAL_NET_BIND_SOURCE_ADDR,
            .source_addr = h2_iperf_test_loopback(0u),
        };
        h2_pal_net_addr_t bound;
        assert(h2_pal_net_tcp_listen(env.config.net, H2_PAL_NET_FAMILY_IPV4, 0u, &loopback, &busy_tcp, &bound) ==
               H2_PAL_OK);
        first_port = bound.port;
    } else if (scenario->busy_first_port == BUSY_PORT_UDP) {
        first_port = h2_iperf_test_free_port(env.config.net);
        busy_udp = hold_udp_port(first_port);
    }

    h2_iperf_client_params_t params;
    memset(&params, 0, sizeof(params));
    params.server_addr = h2_iperf_test_loopback(0u);
    params.protocol = scenario->protocol;
    params.reverse = scenario->reverse;
    params.duration_ms = scenario->bytes != 0u ? 0u : 1000u;
    params.bytes = scenario->bytes;
    params.bitrate_bps = scenario->bitrate_bps;
    params.connect_timeout_ms = 5000u;
    params.control_timeout_ms = 10000u;
    h2_iperf_result_t result;
    h2_pal_result_t status = H2_PAL_ERR_INVALID_STATE;
    int exit_code = -1;
    official_server_t server;
    unsigned first_start_attempts = 0u;
    unsigned attempts = 0u;
    for (;;) {
        official_server_start(
            env.config.net, iperf3, attempts == 0u ? first_port : 0u, SERVER_START_ATTEMPTS - attempts, &server);
        if (attempts == 0u) {
            first_start_attempts = server.attempts;
        }
        attempts += server.attempts;
        params.port = server.port;
        status = h2_iperf_client_run(&env.config, &params, &result);
        exit_code = official_server_finish(&server);
        if (status == H2_PAL_OK || !server.stream_listener_failed || attempts >= SERVER_START_ATTEMPTS) {
            break;
        }
        printf("iperf3 on port %u could not bind its UDP stream listener (attempt %u); "
               "rerunning on a fresh port\n",
               (unsigned)server.port, attempts);
        fflush(stdout);
    }
    if (busy_tcp != -1) {
        assert(first_start_attempts >= 2u);
        h2_pal_net_close(env.config.net, busy_tcp);
    }
    if (busy_udp != -1) {
        assert(attempts >= 2u);
        close(busy_udp);
    }
    assert(server.port != first_port || scenario->busy_first_port == BUSY_PORT_NONE);

    printf("== %s\n", scenario->name);
    printf("client status %d, iperf3 exit %d, attempts %u\n", (int)status, exit_code, attempts);
    h2_iperf_test_print_result("client", &result);
    assert(status == H2_PAL_OK);
    assert(exit_code == 0);

    const h2_iperf_stream_stats_t *sender = scenario->reverse ? &result.remote : &result.local;
    const h2_iperf_stream_stats_t *receiver = scenario->reverse ? &result.local : &result.remote;
    assert(sender->bytes > 0u);
    assert(receiver->bytes > 0u);
    if (scenario->reverse) {
        /* iperf3 3.21 handles TEST_END on its main thread without stopping
         * the sender thread, and adds a block to bytes_sent only after
         * write() returns. On a loaded host that thread can still be waiting
         * for a CPU after the kernel accepted the block, so the reported sender
         * total misses at most that one block while the PAL receiver
         * already counted it. */
        assert(receiver->bytes <= sender->bytes + scenario_block_len(scenario));
    } else {
        assert(receiver->bytes <= sender->bytes);
    }
    /* Loopback keeps almost everything for reliable transports; UDP on a
     * loaded CI host can drop a large share, so only a coarse bound applies.
     * iperf3 stops counting received bytes at TEST_END and drops whatever is
     * still queued in the socket buffers. A 1 s run moves far more than those
     * buffers hold. An 8 MiB run does not: TEST_END follows the last write
     * at once, and a server receive thread that is behind on a loaded host
     * can report MiBs short. Byte mode therefore has no lower bound. */
    if (scenario->protocol == H2_IPERF_PROTOCOL_UDP) {
        assert(receiver->bytes * 2u >= sender->bytes);
    } else if (scenario->bytes == 0u) {
        assert(receiver->bytes * 10u >= sender->bytes * 9u);
    }
    if (scenario->bytes != 0u) {
        assert(sender->bytes == scenario->bytes);
    }
    if (scenario->protocol == H2_IPERF_PROTOCOL_UDP) {
        assert(sender->packets > 0u);
        assert(receiver->packets > 0u);
        assert(receiver->lost_packets >= 0);
        assert(receiver->lost_packets * 2 <= (int64_t)sender->packets);
    }
    h2_iperf_test_env_deinit(&env);
}

int main(int argc, char **argv) {
    assert(argc >= 2);
    static const scenario_t scenarios[] = {
        {"tcp -> iperf3 -s", H2_IPERF_PROTOCOL_TCP, false, 0u, 0u, BUSY_PORT_NONE},
        {"tcp bytes -> iperf3 -s", H2_IPERF_PROTOCOL_TCP, false, 8u * 1024u * 1024u, 0u, BUSY_PORT_NONE},
        {"tcp reverse <- iperf3 -s", H2_IPERF_PROTOCOL_TCP, true, 0u, 0u, BUSY_PORT_NONE},
        {"udp -> iperf3 -s", H2_IPERF_PROTOCOL_UDP, false, 0u, 4u * 1024u * 1024u, BUSY_PORT_NONE},
        {"udp reverse <- iperf3 -s", H2_IPERF_PROTOCOL_UDP, true, 0u, 4u * 1024u * 1024u, BUSY_PORT_NONE},
        {"tcp -> iperf3 -s after busy port", H2_IPERF_PROTOCOL_TCP, false, 0u, 0u, BUSY_PORT_TCP},
        {"udp -> iperf3 -s after busy udp port", H2_IPERF_PROTOCOL_UDP, false, 0u, 4u * 1024u * 1024u, BUSY_PORT_UDP},
    };
    for (size_t i = 0u; i < sizeof(scenarios) / sizeof(scenarios[0]); ++i) {
        run_scenario(argv[1], &scenarios[i]);
    }
    return 0;
}
