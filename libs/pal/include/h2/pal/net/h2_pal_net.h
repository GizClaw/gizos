#ifndef H2_PAL_NET_H
#define H2_PAL_NET_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int h2_pal_net_socket_t;
typedef struct h2_pal_net_resolver h2_pal_net_resolver_t;

typedef enum h2_pal_net_family {
    H2_PAL_NET_FAMILY_IPV4 = 4,
    H2_PAL_NET_FAMILY_IPV6 = 6,
} h2_pal_net_family_t;

typedef struct h2_pal_net_addr {
    h2_pal_net_family_t family;
    uint16_t port;
    uint8_t ip[16];
} h2_pal_net_addr_t;

struct h2_pal_netif_ref;

typedef enum h2_pal_net_bind_type {
    H2_PAL_NET_BIND_DEFAULT = 0,
    H2_PAL_NET_BIND_SOURCE_ADDR = 1,
    H2_PAL_NET_BIND_NETIF = 2,
} h2_pal_net_bind_type_t;

typedef struct h2_pal_net_bind {
    h2_pal_net_bind_type_t type;
    h2_pal_net_addr_t source_addr;
    const struct h2_pal_netif_ref *netif;
} h2_pal_net_bind_t;

typedef enum h2_pal_net_tls_verify {
    H2_PAL_NET_TLS_VERIFY_DEFAULT = 0,
    H2_PAL_NET_TLS_VERIFY_REQUIRED = 1,
    H2_PAL_NET_TLS_VERIFY_INSECURE_TEST_ONLY = 2,
} h2_pal_net_tls_verify_t;

typedef struct h2_pal_net_tls_config {
    const char *server_name;
    const char *alpn;
    const uint8_t *root_ca_pem;
    size_t root_ca_pem_len;
    h2_pal_net_tls_verify_t verify;
} h2_pal_net_tls_config_t;

typedef struct h2_pal_net_icmp_echo_result {
    uint32_t elapsed_ms;
    uint32_t transmitted;
    uint32_t received;
} h2_pal_net_icmp_echo_result_t;

typedef struct h2_pal_net_vtable {
    int (*resolve_addr)(void *user, const char *host, h2_pal_net_addr_t *out_addr);
    /**
     * Start an asynchronous address lookup. The backend must copy `host` and
     * return `H2_PAL_ERR_NO_SPACE` when its bounded lookup capacity is full.
     */
    h2_pal_result_t (*resolve_start)(
        void *user,
        const char *host,
        h2_pal_net_resolver_t **out_resolver);
    /**
     * Wait for an address lookup without exceeding `timeout_ms`.
     *
     * `H2_PAL_ERR_TIMEOUT` and `H2_PAL_ERR_WOULD_BLOCK` leave the resolver
     * pending. All other results are terminal. The caller must close the
     * resolver exactly once after any result or cancellation.
     */
    h2_pal_result_t (*resolve_poll)(
        void *user,
        h2_pal_net_resolver_t *resolver,
        h2_pal_net_addr_t *out_addr,
        uint32_t timeout_ms);
    /**
     * Release caller ownership without waiting for a pending lookup. A lookup
     * that cannot be canceled becomes backend-owned until it self-reclaims.
     */
    void (*resolve_close)(void *user, h2_pal_net_resolver_t *resolver);
    int (*get_host_addr)(void *user, const char *iface_prefix, h2_pal_net_addr_t *out_addr);
    int (*udp_open)(
        void *user,
        h2_pal_net_family_t family,
        uint16_t port,
        h2_pal_net_socket_t *out_socket,
        h2_pal_net_addr_t *out_bind_addr);
    int (*udp_sendto)(
        void *user,
        h2_pal_net_socket_t socket,
        const h2_pal_net_addr_t *addr,
        const uint8_t *data,
        size_t len);
    int (*udp_recvfrom)(
        void *user,
        h2_pal_net_socket_t socket,
        h2_pal_net_addr_t *out_addr,
        uint8_t *data,
        size_t len,
        uint32_t timeout_ms);
    int (*udp_open_bound)(
        void *user,
        h2_pal_net_family_t family,
        uint16_t port,
        const h2_pal_net_bind_t *bind,
        h2_pal_net_socket_t *out_socket,
        h2_pal_net_addr_t *out_bind_addr);
    int (*udp_join_multicast)(
        void *user,
        h2_pal_net_socket_t socket,
        const h2_pal_net_addr_t *addr);
    int (*tcp_open)(
        void *user,
        h2_pal_net_family_t family,
        h2_pal_net_socket_t *out_socket);
    int (*tcp_open_bound)(
        void *user,
        h2_pal_net_family_t family,
        const h2_pal_net_bind_t *bind,
        h2_pal_net_socket_t *out_socket);
    /**
     * @brief Start or continue a bounded TCP connection attempt.
     *
     * `H2_PAL_ERR_TIMEOUT` and `H2_PAL_ERR_WOULD_BLOCK` leave the socket open
     * with the same connection attempt pending; the caller may invoke this
     * operation again with the same address. `H2_PAL_OK` means connected.
     * Any other error makes the socket unusable for this attempt.
     */
    h2_pal_result_t (*tcp_connect)(
        void *user,
        h2_pal_net_socket_t socket,
        const h2_pal_net_addr_t *addr,
        uint32_t timeout_ms);
    int (*tcp_send)(void *user, h2_pal_net_socket_t socket, const uint8_t *data, size_t len);
    /**
     * @brief Submit bytes to a TCP stream within a bounded time budget.
     *
     * A positive result is the number of bytes consumed and may be smaller
     * than `len`. No bytes are consumed when an error is returned. A zero
     * timeout returns `H2_PAL_ERR_WOULD_BLOCK` when no progress is possible;
     * an expired positive timeout returns `H2_PAL_ERR_TIMEOUT`. An orderly
     * close or reset returns `H2_PAL_ERR_CLOSED` and other socket failures
     * return `H2_PAL_ERR_IO`.
     */
    int (*tcp_send_timeout)(
        void *user,
        h2_pal_net_socket_t socket,
        const uint8_t *data,
        size_t len,
        uint32_t timeout_ms);
    int (*tcp_recv)(void *user, h2_pal_net_socket_t socket, uint8_t *data, size_t len, uint32_t timeout_ms);
    h2_pal_result_t (*tls_wrap)(
        void *user,
        h2_pal_net_socket_t tcp_socket,
        const h2_pal_net_tls_config_t *config,
        uint32_t timeout_ms,
        h2_pal_net_socket_t *out_tls_socket);
    h2_pal_result_t (*icmp_echo)(
        void *user,
        const h2_pal_net_addr_t *addr,
        const h2_pal_net_bind_t *bind,
        uint32_t timeout_ms,
        h2_pal_net_icmp_echo_result_t *out_result);
    void (*close)(void *user, h2_pal_net_socket_t socket);
} h2_pal_net_vtable_t;

typedef struct h2_pal_net_api {
    void *user;
    const h2_pal_net_vtable_t *vtable;
} h2_pal_net_api_t;

static inline int h2_pal_net_resolve_addr(
    const h2_pal_net_api_t *api,
    const char *host,
    h2_pal_net_addr_t *out_addr) {
    if (api == NULL || api->vtable == NULL || api->vtable->resolve_addr == NULL || host == NULL || out_addr == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return api->vtable->resolve_addr(api->user, host, out_addr);
}

static inline h2_pal_result_t h2_pal_net_resolve_start(
    const h2_pal_net_api_t *api,
    const char *host,
    h2_pal_net_resolver_t **out_resolver) {
    if (host == NULL || out_resolver == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_resolver = NULL;
    if (api == NULL || api->vtable == NULL ||
        api->vtable->resolve_start == NULL ||
        api->vtable->resolve_poll == NULL ||
        api->vtable->resolve_close == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->resolve_start(api->user, host, out_resolver);
}

static inline h2_pal_result_t h2_pal_net_resolve_poll(
    const h2_pal_net_api_t *api,
    h2_pal_net_resolver_t *resolver,
    h2_pal_net_addr_t *out_addr,
    uint32_t timeout_ms) {
    if (resolver == NULL || out_addr == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL ||
        api->vtable->resolve_poll == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->resolve_poll(
        api->user, resolver, out_addr, timeout_ms);
}

static inline void h2_pal_net_resolve_close(
    const h2_pal_net_api_t *api,
    h2_pal_net_resolver_t *resolver) {
    if (api != NULL && api->vtable != NULL &&
        api->vtable->resolve_close != NULL && resolver != NULL) {
        api->vtable->resolve_close(api->user, resolver);
    }
}

static inline int h2_pal_net_get_host_addr(
    const h2_pal_net_api_t *api,
    const char *iface_prefix,
    h2_pal_net_addr_t *out_addr) {
    if (api == NULL || api->vtable == NULL || api->vtable->get_host_addr == NULL || out_addr == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return api->vtable->get_host_addr(api->user, iface_prefix, out_addr);
}

static inline int h2_pal_net_udp_open_bound(
    const h2_pal_net_api_t *api,
    h2_pal_net_family_t family,
    uint16_t port,
    const h2_pal_net_bind_t *bind,
    h2_pal_net_socket_t *out_socket,
    h2_pal_net_addr_t *out_bind_addr) {
    if (api == NULL || out_socket == NULL || out_bind_addr == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (bind == NULL || bind->type == H2_PAL_NET_BIND_DEFAULT) {
        if (api->vtable == NULL || api->vtable->udp_open == NULL) {
            return H2_PAL_ERR_UNSUPPORTED;
        }
        return api->vtable->udp_open(api->user, family, port, out_socket, out_bind_addr);
    }
    if (api->vtable == NULL || api->vtable->udp_open_bound == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->udp_open_bound(api->user, family, port, bind, out_socket, out_bind_addr);
}

static inline int h2_pal_net_udp_sendto(
    const h2_pal_net_api_t *api,
    h2_pal_net_socket_t socket,
    const h2_pal_net_addr_t *addr,
    const uint8_t *data,
    size_t len) {
    if (api == NULL || api->vtable == NULL || api->vtable->udp_sendto == NULL ||
        socket < 0 || addr == NULL || (data == NULL && len != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return api->vtable->udp_sendto(api->user, socket, addr, data, len);
}

static inline int h2_pal_net_udp_recvfrom(
    const h2_pal_net_api_t *api,
    h2_pal_net_socket_t socket,
    h2_pal_net_addr_t *out_addr,
    uint8_t *data,
    size_t len,
    uint32_t timeout_ms) {
    if (api == NULL || api->vtable == NULL || api->vtable->udp_recvfrom == NULL ||
        socket < 0 || out_addr == NULL || (data == NULL && len != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return api->vtable->udp_recvfrom(api->user, socket, out_addr, data, len, timeout_ms);
}

static inline int h2_pal_net_udp_join_multicast(
    const h2_pal_net_api_t *api,
    h2_pal_net_socket_t socket,
    const h2_pal_net_addr_t *addr) {
    if (api == NULL || api->vtable == NULL || api->vtable->udp_join_multicast == NULL ||
        socket < 0 || addr == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->udp_join_multicast(api->user, socket, addr);
}

static inline int h2_pal_net_tcp_open_bound(
    const h2_pal_net_api_t *api,
    h2_pal_net_family_t family,
    const h2_pal_net_bind_t *bind,
    h2_pal_net_socket_t *out_socket) {
    if (api == NULL || out_socket == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (bind == NULL || bind->type == H2_PAL_NET_BIND_DEFAULT) {
        if (api->vtable == NULL || api->vtable->tcp_open == NULL) {
            return H2_PAL_ERR_UNSUPPORTED;
        }
        return api->vtable->tcp_open(api->user, family, out_socket);
    }
    if (api->vtable == NULL || api->vtable->tcp_open_bound == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->tcp_open_bound(api->user, family, bind, out_socket);
}

static inline int h2_pal_net_tcp_send(
    const h2_pal_net_api_t *api,
    h2_pal_net_socket_t socket,
    const uint8_t *data,
    size_t len) {
    if (api == NULL || api->vtable == NULL || api->vtable->tcp_send == NULL ||
        socket < 0 || (data == NULL && len != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return api->vtable->tcp_send(api->user, socket, data, len);
}

static inline int h2_pal_net_tcp_send_timeout(
    const h2_pal_net_api_t *api,
    h2_pal_net_socket_t socket,
    const uint8_t *data,
    size_t len,
    uint32_t timeout_ms) {
    if (api == NULL || socket < 0 || (data == NULL && len != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api->vtable == NULL || api->vtable->tcp_send_timeout == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->tcp_send_timeout(
        api->user, socket, data, len, timeout_ms);
}

static inline int h2_pal_net_tcp_recv(
    const h2_pal_net_api_t *api,
    h2_pal_net_socket_t socket,
    uint8_t *data,
    size_t len,
    uint32_t timeout_ms) {
    if (api == NULL || api->vtable == NULL || api->vtable->tcp_recv == NULL ||
        socket < 0 || (data == NULL && len != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return api->vtable->tcp_recv(api->user, socket, data, len, timeout_ms);
}

static inline h2_pal_result_t h2_pal_net_tcp_connect(
    const h2_pal_net_api_t *api,
    h2_pal_net_socket_t socket,
    const h2_pal_net_addr_t *addr,
    uint32_t timeout_ms) {
    if (socket < 0 || addr == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->tcp_connect == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->tcp_connect(api->user, socket, addr, timeout_ms);
}

static inline h2_pal_result_t h2_pal_net_tls_wrap(
    const h2_pal_net_api_t *api,
    h2_pal_net_socket_t tcp_socket,
    const h2_pal_net_tls_config_t *config,
    uint32_t timeout_ms,
    h2_pal_net_socket_t *out_tls_socket) {
    if (api == NULL || config == NULL || out_tls_socket == NULL || tcp_socket < 0) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api->vtable == NULL || api->vtable->tls_wrap == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->tls_wrap(api->user, tcp_socket, config, timeout_ms, out_tls_socket);
}

static inline h2_pal_result_t h2_pal_net_icmp_echo(
    const h2_pal_net_api_t *api,
    const h2_pal_net_addr_t *addr,
    const h2_pal_net_bind_t *bind,
    uint32_t timeout_ms,
    h2_pal_net_icmp_echo_result_t *out_result) {
    if (api == NULL || addr == NULL || timeout_ms == 0u || out_result == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api->vtable == NULL || api->vtable->icmp_echo == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->icmp_echo(api->user, addr, bind, timeout_ms, out_result);
}

static inline void h2_pal_net_close(const h2_pal_net_api_t *api, h2_pal_net_socket_t socket) {
    if (api != NULL && api->vtable != NULL && api->vtable->close != NULL && socket >= 0) {
        api->vtable->close(api->user, socket);
    }
}

#ifdef __cplusplus
}
#endif

#endif
