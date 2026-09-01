#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static int unsupported_net_resolve_addr(void *p0, const char *p1, h2_pal_net_addr_t *p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_net_resolve_start(
    void *p0, const char *p1, h2_pal_net_resolver_t **p2) {
    (void)p0;
    (void)p1;
    if (p2 != NULL) {
        *p2 = NULL;
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_net_resolve_poll(
    void *p0, h2_pal_net_resolver_t *p1, h2_pal_net_addr_t *p2,
    uint32_t p3) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    return H2_PAL_ERR_UNSUPPORTED;
}

static void unsupported_net_resolve_close(
    void *p0, h2_pal_net_resolver_t *p1) {
    (void)p0;
    (void)p1;
}

static int unsupported_net_get_host_addr(void *p0, const char *p1, h2_pal_net_addr_t *p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_net_udp_open(void *p0, h2_pal_net_family_t p1, uint16_t p2, h2_pal_net_socket_t *p3, h2_pal_net_addr_t *p4) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    (void)p4;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_net_udp_sendto(void *p0, h2_pal_net_socket_t p1, const h2_pal_net_addr_t *p2, const uint8_t *p3, size_t p4) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    (void)p4;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_net_udp_recvfrom(void *p0, h2_pal_net_socket_t p1, h2_pal_net_addr_t *p2, uint8_t *p3, size_t p4, uint32_t p5) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    (void)p4;
    (void)p5;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_net_udp_open_bound(void *p0, h2_pal_net_family_t p1, uint16_t p2, const h2_pal_net_bind_t *p3, h2_pal_net_socket_t *p4, h2_pal_net_addr_t *p5) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    (void)p4;
    (void)p5;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_net_udp_join_multicast(void *p0, h2_pal_net_socket_t p1, const h2_pal_net_addr_t *p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_net_tcp_open(void *p0, h2_pal_net_family_t p1, h2_pal_net_socket_t *p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_net_tcp_open_bound(void *p0, h2_pal_net_family_t p1, const h2_pal_net_bind_t *p2, h2_pal_net_socket_t *p3) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_net_tcp_connect(void *p0, h2_pal_net_socket_t p1, const h2_pal_net_addr_t *p2, uint32_t p3) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_net_tcp_send(void *p0, h2_pal_net_socket_t p1, const uint8_t *p2, size_t p3) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_net_tcp_send_timeout(void *p0, h2_pal_net_socket_t p1, const uint8_t *p2, size_t p3, uint32_t p4) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    (void)p4;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_net_tcp_recv(void *p0, h2_pal_net_socket_t p1, uint8_t *p2, size_t p3, uint32_t p4) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    (void)p4;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_net_tls_wrap(void *p0, h2_pal_net_socket_t p1, const h2_pal_net_tls_config_t *p2, uint32_t p3, h2_pal_net_socket_t *p4) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    (void)p4;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_net_icmp_echo(void *p0, const h2_pal_net_addr_t *p1, const h2_pal_net_bind_t *p2, uint32_t p3, h2_pal_net_icmp_echo_result_t *p4) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    (void)p4;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_net_tcp_listen(
    void *p0, h2_pal_net_family_t p1, uint16_t p2, const h2_pal_net_bind_t *p3,
    h2_pal_net_socket_t *p4, h2_pal_net_addr_t *p5) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    (void)p5;
    if (p4 != NULL) {
        *p4 = -1;
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_net_tcp_accept(
    void *p0, h2_pal_net_socket_t p1, h2_pal_net_socket_t *p2,
    h2_pal_net_addr_t *p3, uint32_t p4) {
    (void)p0;
    (void)p1;
    (void)p3;
    (void)p4;
    if (p2 != NULL) {
        *p2 = -1;
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static void unsupported_net_close(void *p0, h2_pal_net_socket_t p1) {
    (void)p0;
    (void)p1;
}

static const h2_pal_net_vtable_t unsupported_net_vtable = {
    .resolve_addr = unsupported_net_resolve_addr,
    .resolve_start = unsupported_net_resolve_start,
    .resolve_poll = unsupported_net_resolve_poll,
    .resolve_close = unsupported_net_resolve_close,
    .get_host_addr = unsupported_net_get_host_addr,
    .udp_open = unsupported_net_udp_open,
    .udp_sendto = unsupported_net_udp_sendto,
    .udp_recvfrom = unsupported_net_udp_recvfrom,
    .udp_open_bound = unsupported_net_udp_open_bound,
    .udp_join_multicast = unsupported_net_udp_join_multicast,
    .tcp_open = unsupported_net_tcp_open,
    .tcp_open_bound = unsupported_net_tcp_open_bound,
    .tcp_connect = unsupported_net_tcp_connect,
    .tcp_send = unsupported_net_tcp_send,
    .tcp_send_timeout = unsupported_net_tcp_send_timeout,
    .tcp_recv = unsupported_net_tcp_recv,
    .tls_wrap = unsupported_net_tls_wrap,
    .icmp_echo = unsupported_net_icmp_echo,
    .close = unsupported_net_close,
    .tcp_listen = unsupported_net_tcp_listen,
    .tcp_accept = unsupported_net_tcp_accept,
};
static const h2_pal_net_api_t unsupported_net_api = { .user = NULL, .vtable = &unsupported_net_vtable };
const h2_pal_net_api_t *h2_pal_unsupported_net_api(void) { return &unsupported_net_api; }
