#include "h2_windows_platform.h"
#include "../src/h2_windows_internal.h"

#include <assert.h>
#include <limits.h>

int main(void) {
    const h2_windows_platform_config_t config = {0};
    h2_windows_platform_t *platform = NULL;
    assert(h2_windows_platform_create(&config, &platform) == H2_PAL_OK);
    const h2_pal_net_api_t *net = h2_windows_net_api(platform);
    h2_pal_net_socket_t first = -1;
    assert(h2_pal_net_tcp_open_bound(net, H2_PAL_NET_FAMILY_IPV4, NULL,
                                     &first) == H2_PAL_OK);
    h2_pal_net_close(net, first);
    h2_pal_net_socket_t second = -1;
    assert(h2_pal_net_tcp_open_bound(net, H2_PAL_NET_FAMILY_IPV4, NULL,
                                     &second) == H2_PAL_OK);
    assert(second != first);
    h2_pal_net_close(net, first);
    h2_pal_net_close(net, second);

    h2_pal_net_socket_t udp = -1;
    h2_pal_net_addr_t bound;
    assert(h2_pal_net_udp_open_bound(net, H2_PAL_NET_FAMILY_IPV4, 0u, NULL,
                                     &udp, &bound) == H2_PAL_OK);
    uint8_t byte = 0u;
    h2_pal_net_addr_t peer;
    assert(h2_pal_net_udp_recvfrom(net, udp, &peer, &byte, sizeof(byte), 0u) ==
           H2_PAL_ERR_WOULD_BLOCK);
    const h2_pal_net_addr_t multicast = {
        .family = H2_PAL_NET_FAMILY_IPV4,
        .ip = {239u, 255u, 0u, 1u},
    };
    assert(h2_pal_net_udp_join_multicast(net, udp, &multicast) == H2_PAL_OK);
    h2_pal_net_close(net, udp);

    const h2_pal_net_addr_t loopback = {
        .family = H2_PAL_NET_FAMILY_IPV4,
        .ip = {127u, 0u, 0u, 1u},
    };
    h2_pal_net_icmp_echo_result_t echo;
    assert(h2_pal_net_icmp_echo(net, &loopback, NULL, 2000u, &echo) ==
           H2_PAL_OK);
    assert(echo.transmitted == 1u && echo.received == 1u);

    const h2_pal_net_bind_t source_binding = {
        .type = H2_PAL_NET_BIND_SOURCE_ADDR,
        .source_addr = loopback,
    };
    assert(h2_pal_net_udp_open_bound(net, H2_PAL_NET_FAMILY_IPV4, 0u,
                                     &source_binding, &udp, &bound) ==
           H2_PAL_OK);
    assert(bound.ip[0] == 127u && bound.ip[3] == 1u);
    h2_pal_net_close(net, udp);

    h2_pal_net_resolver_t *resolver = NULL;
    assert(h2_pal_net_resolve_start(net, "localhost", &resolver) == H2_PAL_OK);
    h2_pal_net_resolve_close(net, resolver);

    const uint32_t max_generation =
        (uint32_t)INT_MAX / H2_WINDOWS_SOCKET_CAPACITY;
    for (size_t index = 0u; index < H2_WINDOWS_SOCKET_CAPACITY; ++index) {
        platform->sockets[index].generation = max_generation;
    }
    h2_pal_net_socket_t exhausted = -1;
    assert(h2_pal_net_tcp_open_bound(net, H2_PAL_NET_FAMILY_IPV4, NULL,
                                     &exhausted) == H2_PAL_ERR_NO_SPACE);
    assert(exhausted == -1);
    assert(h2_windows_platform_destroy(&platform) == H2_PAL_OK);
    return 0;
}
