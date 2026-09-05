/*
 * PAL SCTP client against an external kernel-SCTP iperf3 server reached via
 * RFC 6951 UDP encapsulation. Manual: needs a Linux host running
 *   sysctl -w net.sctp.udp_port=9899
 *   iperf3 -s
 * and H2_IPERF_SCTP_SERVER=<ipv4>[:<port>[:<udp-port>]] in the environment.
 */
#include "h2_iperf_test_support.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool parse_target(const char *text, h2_pal_net_addr_t *addr, uint16_t *port, uint16_t *udp_port) {
    unsigned a = 0u;
    unsigned b = 0u;
    unsigned c = 0u;
    unsigned d = 0u;
    unsigned p = H2_IPERF_DEFAULT_PORT;
    unsigned u = H2_IPERF_DEFAULT_SCTP_UDP_PORT;
    int fields = sscanf(text, "%u.%u.%u.%u:%u:%u", &a, &b, &c, &d, &p, &u);
    if (fields < 4 || a > 255u || b > 255u || c > 255u || d > 255u ||
        p == 0u || p > 65535u || u == 0u || u > 65535u) {
        return false;
    }
    memset(addr, 0, sizeof(*addr));
    addr->family = H2_PAL_NET_FAMILY_IPV4;
    addr->ip[0] = (uint8_t)a;
    addr->ip[1] = (uint8_t)b;
    addr->ip[2] = (uint8_t)c;
    addr->ip[3] = (uint8_t)d;
    *port = (uint16_t)p;
    *udp_port = (uint16_t)u;
    return true;
}

int main(void) {
    const char *target = getenv("H2_IPERF_SCTP_SERVER");
    if (target == NULL || target[0] == '\0') {
        printf("H2_IPERF_SCTP_SERVER is not set; nothing to test\n");
        return 0;
    }
    h2_iperf_client_params_t params;
    memset(&params, 0, sizeof(params));
    assert(parse_target(target, &params.server_addr, &params.port, &params.sctp_udp_port));
    params.protocol = H2_IPERF_PROTOCOL_SCTP;
    params.duration_ms = 2000u;
    params.block_len = 16u * 1024u;
    params.control_timeout_ms = 15000u;

    h2_iperf_test_env_t env;
    h2_iperf_test_env_init(&env, true);
    h2_iperf_result_t result;
    h2_pal_result_t status = h2_iperf_client_run(&env.config, &params, &result);
    printf("client status %d\n", (int)status);
    h2_iperf_test_print_result("client", &result);
    assert(status == H2_PAL_OK);
    assert(result.local.bytes > 0u);
    assert(result.remote.bytes > 0u);
    assert(result.remote.bytes <= result.local.bytes);

    params.reverse = true;
    status = h2_iperf_client_run(&env.config, &params, &result);
    printf("client status %d\n", (int)status);
    h2_iperf_test_print_result("client reverse", &result);
    assert(status == H2_PAL_OK);
    assert(result.local.bytes > 0u);
    assert(result.remote.bytes >= result.local.bytes);
    h2_iperf_test_env_deinit(&env);
    return 0;
}
