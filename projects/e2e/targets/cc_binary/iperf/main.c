#include "h2_desktop_platform.h"
#include "h2_iperf.h"
#include "h2_iperf_e2e.h"
#include "h2_sctp.h"
#if defined(__APPLE__)
#include "h2_darwin_platform.h"
#else
#include "h2_linux_platform.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static h2_pal_result_t host_random(void *user, uint8_t *out, size_t len) {
    (void)user;
#if defined(__APPLE__)
    return h2_darwin_entropy(NULL, out, len) == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
#else
    return h2_linux_entropy(NULL, out, len) == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
#endif
}

static const h2_pal_crypto_vtable_t s_crypto_vtable = {
    .random = host_random,
};

static h2_pal_crypto_api_t s_crypto_api = {
    .user = NULL,
    .vtable = &s_crypto_vtable,
};

static void usage(void) {
    fprintf(stderr,
            "usage:\n"
            "  h2iperf server [--port N] [--sctp-udp-port N]\n"
            "  h2iperf client <host> [--port N] [--sctp-udp-port N]"
            " [--duration-ms N] [--target NAME]\n");
}

static int parse_u32(const char *text, uint32_t *out) {
    char *end = NULL;
    const unsigned long value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value > 0xffffffffUL) {
        return -1;
    }
    *out = (uint32_t)value;
    return 0;
}

static int init_pal(h2_iperf_config_t *pal, h2_sctp_t **out_sctp) {
    memset(pal, 0, sizeof(*pal));
    pal->mem = h2_desktop_platform_default_allocator();
    pal->time = h2_desktop_platform_time_api();
    pal->log = h2_desktop_platform_log_api();
#if defined(__APPLE__)
    pal->net = h2_darwin_net_api();
#else
    pal->net = h2_linux_net_api();
#endif
    pal->crypto = &s_crypto_api;
    if (pal->mem == NULL || pal->time == NULL || pal->net == NULL) {
        return -1;
    }
    const h2_sctp_config_t sctp_config = {
        .mem = pal->mem,
        .crypto = &s_crypto_api,
    };
    if (h2_sctp_create(&sctp_config, out_sctp) != H2_PAL_OK) {
        return -1;
    }
    pal->sctp = h2_sctp_api(*out_sctp);
    return 0;
}

static int run_server(int argc, char **argv) {
    uint32_t port = 0u;
    uint32_t sctp_udp_port = 0u;
    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &port) != 0 || port > 65535u) {
                usage();
                return 2;
            }
        } else if (strcmp(argv[i], "--sctp-udp-port") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &sctp_udp_port) != 0 ||
                sctp_udp_port > 65535u) {
                usage();
                return 2;
            }
        } else {
            usage();
            return 2;
        }
    }
    h2_iperf_config_t pal;
    h2_sctp_t *sctp = NULL;
    if (init_pal(&pal, &sctp) != 0) {
        fprintf(stderr, "h2iperf: PAL initialisation failed\n");
        return 1;
    }
    h2_iperf_server_params_t params;
    memset(&params, 0, sizeof(params));
    params.port = (uint16_t)port;
    params.sctp_udp_port = (uint16_t)sctp_udp_port;
    h2_iperf_server_t *server = NULL;
    const h2_pal_result_t rc = h2_iperf_server_create(&pal, &params, &server);
    if (rc != H2_PAL_OK) {
        fprintf(stderr, "h2iperf: server create failed rc=%d\n", (int)rc);
        return 1;
    }
    printf("H2IPERF_SERVER listening port=%u sctp_udp_port=%u\n",
           (unsigned)h2_iperf_server_port(server),
           (unsigned)h2_iperf_server_sctp_udp_port(server));
    fflush(stdout);
    for (;;) {
        h2_iperf_result_t result;
        const h2_pal_result_t run_rc =
            h2_iperf_server_run_once(server, 60000u, &result);
        if (run_rc == H2_PAL_ERR_TIMEOUT) {
            continue;
        }
        if (run_rc != H2_PAL_OK) {
            printf("H2IPERF_SERVER test rc=%d\n", (int)run_rc);
            fflush(stdout);
            continue;
        }
        const h2_iperf_stream_stats_t *receiver =
            result.reverse ? &result.remote : &result.local;
        printf("H2IPERF_SERVER test protocol=%d reverse=%d receiver_bps=%llu "
               "bytes=%llu packets=%llu lost=%lld jitter_ms=%.3f\n",
               (int)result.protocol, result.reverse ? 1 : 0,
               (unsigned long long)h2_iperf_stats_bits_per_second(receiver),
               (unsigned long long)receiver->bytes,
               (unsigned long long)receiver->packets,
               (long long)receiver->lost_packets, receiver->jitter_ms);
        fflush(stdout);
    }
}

static int run_client(int argc, char **argv) {
    if (argc < 1) {
        usage();
        return 2;
    }
    const char *host = argv[0];
    uint32_t port = 0u;
    uint32_t sctp_udp_port = 0u;
    uint32_t duration_ms = 0u;
    const char *target = "host";
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &port) != 0 || port > 65535u) {
                usage();
                return 2;
            }
        } else if (strcmp(argv[i], "--sctp-udp-port") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &sctp_udp_port) != 0 ||
                sctp_udp_port > 65535u) {
                usage();
                return 2;
            }
        } else if (strcmp(argv[i], "--duration-ms") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &duration_ms) != 0) {
                usage();
                return 2;
            }
        } else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
            target = argv[++i];
        } else {
            usage();
            return 2;
        }
    }
    h2_iperf_config_t pal;
    h2_sctp_t *sctp = NULL;
    if (init_pal(&pal, &sctp) != 0) {
        fprintf(stderr, "h2iperf: PAL initialisation failed\n");
        return 1;
    }
    h2_iperf_e2e_config_t config;
    memset(&config, 0, sizeof(config));
    config.pal = pal;
    config.target = target;
    config.server_host = host;
    config.port = (uint16_t)port;
    config.sctp_udp_port = (uint16_t)sctp_udp_port;
    config.duration_ms = duration_ms;
    h2_iperf_e2e_report_t report;
    const h2_pal_result_t rc = h2_iperf_e2e_run(&config, &report);
    (void)h2_sctp_destroy(&sctp);
    return rc == H2_PAL_OK ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    if (strcmp(argv[1], "server") == 0) {
        return run_server(argc - 2, argv + 2);
    }
    if (strcmp(argv[1], "client") == 0) {
        return run_client(argc - 2, argv + 2);
    }
    usage();
    return 2;
}
