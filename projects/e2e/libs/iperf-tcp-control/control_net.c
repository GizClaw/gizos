#include "control_net.h"
#if defined(ESP_PLATFORM)
#include "lwip/sockets.h"
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#endif

static h2_pal_net_api_t original, view;
static h2_pal_net_vtable_t options;
static h2_pal_net_socket_t control = -1;

static int configure(h2_pal_net_socket_t *socket) {
    if (control >= 0) return H2_PAL_OK;
    const int enabled = 1;
    int actual = 0;
    socklen_t actual_size = sizeof(actual);
    if (setsockopt(*socket, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) != 0 ||
        getsockopt(*socket, IPPROTO_TCP, TCP_NODELAY, &actual, &actual_size) != 0 ||
        actual == 0) {
        original.vtable->close(original.user, *socket);
        *socket = -1;
        return H2_PAL_ERR_IO;
    }
    control = *socket;
    return H2_PAL_OK;
}
static int open_tcp(void *user, h2_pal_net_family_t family,
                    h2_pal_net_socket_t *socket) {
    if (socket == NULL) return H2_PAL_ERR_INVALID_ARG;
    int rc = original.vtable->tcp_open(user, family, socket);
    return rc == H2_PAL_OK ? configure(socket) : rc;
}
static int open_bound(void *user, h2_pal_net_family_t family,
                      const h2_pal_net_bind_t *bind, h2_pal_net_socket_t *socket) {
    if (socket == NULL) return H2_PAL_ERR_INVALID_ARG;
    int rc = original.vtable->tcp_open_bound(user, family, bind, socket);
    return rc == H2_PAL_OK ? configure(socket) : rc;
}
static h2_pal_result_t accept_tcp(void *user, h2_pal_net_socket_t listener,
                                  h2_pal_net_socket_t *socket,
                                  h2_pal_net_addr_t *address, uint32_t timeout_ms) {
    if (socket == NULL) return H2_PAL_ERR_INVALID_ARG;
    h2_pal_result_t rc = original.vtable->tcp_accept(user, listener, socket, address, timeout_ms);
    return rc == H2_PAL_OK ? (h2_pal_result_t)configure(socket) : rc;
}
static void close_socket(void *user, h2_pal_net_socket_t socket) {
    if (socket == control) control = -1;
    original.vtable->close(user, socket);
}
const h2_pal_net_api_t *h2_iperf_e2e_control_net(const h2_pal_net_api_t *base) {
    if (base == NULL || base->vtable == NULL || base->vtable->close == NULL)
        return NULL;
    if (original.vtable != NULL) {
        return original.vtable == base->vtable && original.user == base->user ? &view : NULL;
    }
    original = *base;
    options = *base->vtable;
    if (options.tcp_open != NULL) options.tcp_open = open_tcp;
    if (options.tcp_open_bound != NULL) options.tcp_open_bound = open_bound;
    if (options.tcp_accept != NULL) options.tcp_accept = accept_tcp;
    options.close = close_socket;
    view = (h2_pal_net_api_t){.user = base->user, .vtable = &options};
    return &view;
}
