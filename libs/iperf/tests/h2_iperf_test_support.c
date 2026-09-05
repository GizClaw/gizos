#include "h2_iperf_test_support.h"

#include "h2_desktop_platform.h"
#if defined(__APPLE__)
#include "h2_darwin_platform.h"
#else
#include "h2_linux_platform.h"
#endif

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static h2_pal_result_t test_random(void *user, uint8_t *out, size_t len) {
    (void)user;
#if defined(__APPLE__)
    return h2_darwin_entropy(NULL, out, len) == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
#else
    return h2_linux_entropy(NULL, out, len) == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
#endif
}

static const h2_pal_crypto_vtable_t s_crypto_vtable = {
    .random = test_random,
};

void h2_iperf_test_env_init(h2_iperf_test_env_t *env, bool with_sctp) {
    memset(env, 0, sizeof(*env));
    env->crypto_api.user = NULL;
    env->crypto_api.vtable = &s_crypto_vtable;
    env->config.mem = h2_desktop_platform_default_allocator();
    env->config.time = h2_desktop_platform_time_api();
    env->config.log = h2_desktop_platform_log_api();
#if defined(__APPLE__)
    env->config.net = h2_darwin_net_api();
#else
    env->config.net = h2_linux_net_api();
#endif
    env->config.crypto = &env->crypto_api;
    assert(env->config.mem != NULL);
    assert(env->config.time != NULL);
    assert(env->config.net != NULL);
    if (with_sctp) {
        const h2_sctp_config_t sctp_config = {
            .mem = env->config.mem,
            .crypto = &env->crypto_api,
        };
        assert(h2_sctp_create(&sctp_config, &env->sctp_provider) == H2_PAL_OK);
        env->config.sctp = h2_sctp_api(env->sctp_provider);
        assert(env->config.sctp != NULL);
    }
}

void h2_iperf_test_env_deinit(h2_iperf_test_env_t *env) {
    if (env->sctp_provider != NULL) {
        assert(h2_sctp_destroy(&env->sctp_provider) == H2_PAL_OK);
    }
    memset(env, 0, sizeof(*env));
}

h2_pal_net_addr_t h2_iperf_test_loopback(uint16_t port) {
    h2_pal_net_addr_t addr;
    memset(&addr, 0, sizeof(addr));
    addr.family = H2_PAL_NET_FAMILY_IPV4;
    addr.port = port;
    addr.ip[0] = 127u;
    addr.ip[3] = 1u;
    return addr;
}

uint16_t h2_iperf_test_free_port(const h2_pal_net_api_t *net) {
    h2_pal_net_socket_t sock = -1;
    h2_pal_net_addr_t bound;
    assert(h2_pal_net_tcp_listen(net, H2_PAL_NET_FAMILY_IPV4, 0u, NULL, &sock, &bound) == H2_PAL_OK);
    assert(bound.port != 0u);
    h2_pal_net_close(net, sock);
    return bound.port;
}

pid_t h2_iperf_test_spawn(char *const argv[]) {
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        execv(argv[0], argv);
        perror("execv");
        _exit(127);
    }
    return pid;
}

int h2_iperf_test_wait(pid_t pid, uint32_t timeout_ms) {
    uint32_t waited = 0u;
    for (;;) {
        int status = 0;
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            if (WIFEXITED(status)) {
                return WEXITSTATUS(status);
            }
            return -1;
        }
        if (done < 0 && errno != EINTR) {
            return -1;
        }
        if (waited >= timeout_ms) {
            (void)kill(pid, SIGKILL);
            (void)waitpid(pid, &status, 0);
            return -1;
        }
        h2_iperf_test_sleep_ms(20u);
        waited += 20u;
    }
}

void h2_iperf_test_sleep_ms(uint32_t ms) {
    struct timespec spec;
    spec.tv_sec = (time_t)(ms / 1000u);
    spec.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&spec, &spec) != 0 && errno == EINTR) {
    }
}

static void print_stats(const char *label, const h2_iperf_stream_stats_t *stats) {
    printf("  %s: bytes=%llu packets=%llu lost=%lld out_of_order=%llu "
           "jitter_ms=%.3f duration_ms=%u throughput=%llu bps\n",
           label,
           (unsigned long long)stats->bytes,
           (unsigned long long)stats->packets,
           (long long)stats->lost_packets,
           (unsigned long long)stats->out_of_order,
           stats->jitter_ms,
           stats->duration_ms,
           (unsigned long long)h2_iperf_stats_bits_per_second(stats));
}

void h2_iperf_test_print_result(const char *label, const h2_iperf_result_t *result) {
    static const char *const names[] = {"tcp", "udp", "sctp"};
    printf("%s (%s%s):\n", label,
           result->protocol <= H2_IPERF_PROTOCOL_SCTP ? names[result->protocol] : "?",
           result->reverse ? ", reverse" : "");
    print_stats("local ", &result->local);
    print_stats("remote", &result->remote);
    fflush(stdout);
}
