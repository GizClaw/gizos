#include "control_net.h"
#include "h2_iperf_test_support.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

static int nodelay(h2_pal_net_socket_t socket) {
    int value = -1;
    socklen_t size = sizeof(value);
    assert(getsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &value, &size) == 0);
    return value;
}
typedef struct fault_fixture {
    int unsupported;
    int last_socket;
    unsigned closes;
} fault_fixture_t;
static int fault_open(void *user, h2_pal_net_family_t family, h2_pal_net_socket_t *out) {
    fault_fixture_t *f = user;
    assert(family == H2_PAL_NET_FAMILY_IPV4);
    /* A valid socket without TCP options exercises set-option failure. */
    f->last_socket = socket(f->unsupported ? AF_UNIX : AF_INET, SOCK_STREAM, 0);
    assert(f->last_socket >= 0);
    *out = f->last_socket;
    return H2_PAL_OK;
}
static void fault_close(void *user, h2_pal_net_socket_t fd) {
    fault_fixture_t *f = user;
    ++f->closes;
    assert(close(fd) == 0);
}
static int test_option_failure(void) {
    fault_fixture_t f = {.unsupported = 1};
    const h2_pal_net_vtable_t vtable = {.tcp_open = fault_open, .close = fault_close};
    const h2_pal_net_api_t base = {.user = &f, .vtable = &vtable};
    const h2_pal_net_api_t *net = h2_iperf_e2e_control_net(&base);
    assert(net != NULL);
    h2_pal_net_socket_t fd = -1;
    assert(h2_pal_net_tcp_open_bound(net, H2_PAL_NET_FAMILY_IPV4, NULL, &fd) == H2_PAL_ERR_IO);
    assert(fd == -1 && f.closes == 1u);
    assert(fcntl(f.last_socket, F_GETFD) == -1 && errno == EBADF);
    f.unsupported = 0;
    assert(h2_pal_net_tcp_open_bound(net, H2_PAL_NET_FAMILY_IPV4, NULL, &fd) == H2_PAL_OK);
    assert(nodelay(fd) != 0);
    h2_pal_net_close(net, fd);
    assert(f.closes == 2u);
    return 0;
}
int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "fault") == 0) return test_option_failure();
    h2_iperf_test_env_t env;
    h2_iperf_test_env_init(&env, false);
    assert(h2_iperf_e2e_control_net(NULL) == NULL);
    const h2_pal_net_api_t *base = env.config.net;
    const h2_pal_net_api_t *net = h2_iperf_e2e_control_net(base);
    assert(net != NULL && net->user == base->user);
    assert(h2_iperf_e2e_control_net(base) == net);
    assert(net->vtable->udp_sendto == base->vtable->udp_sendto);
    assert(net->vtable->tls_wrap == base->vtable->tls_wrap);
    h2_pal_net_api_t other = {.user = &env, .vtable = base->vtable};
    assert(h2_iperf_e2e_control_net(&other) == NULL);
    h2_pal_net_socket_t control, data;
    assert(h2_pal_net_tcp_open_bound(net, H2_PAL_NET_FAMILY_IPV4, NULL, &control) == H2_PAL_OK);
    assert(nodelay(control) != 0);
    assert(h2_pal_net_tcp_open_bound(net, H2_PAL_NET_FAMILY_IPV4, NULL, &data) == H2_PAL_OK);
    assert(nodelay(data) == 0);
    h2_pal_net_close(net, data);
    h2_pal_net_close(net, control);
    h2_pal_net_bind_t bind = {
        .type = H2_PAL_NET_BIND_SOURCE_ADDR,
        .source_addr = h2_iperf_test_loopback(0),
    };
    assert(h2_pal_net_tcp_open_bound(net, H2_PAL_NET_FAMILY_IPV4, &bind, &control) == H2_PAL_OK);
    assert(nodelay(control) != 0);
    h2_pal_net_close(net, control);
    h2_pal_net_socket_t listener, client;
    h2_pal_net_addr_t address;
    assert(h2_pal_net_tcp_listen(base, H2_PAL_NET_FAMILY_IPV4, 0, NULL, &listener, &address) == H2_PAL_OK);
    address = h2_iperf_test_loopback(address.port);
    assert(h2_pal_net_tcp_open_bound(base, H2_PAL_NET_FAMILY_IPV4, NULL, &client) == H2_PAL_OK);
    assert(h2_pal_net_tcp_connect(base, client, &address, 1000) == H2_PAL_OK);
    assert(h2_pal_net_tcp_accept(net, listener, &control, NULL, 1000) == H2_PAL_OK);
    assert(nodelay(control) != 0 && nodelay(client) == 0);
    h2_pal_net_socket_t second_client;
    assert(h2_pal_net_tcp_open_bound(base, H2_PAL_NET_FAMILY_IPV4, NULL, &second_client) == H2_PAL_OK);
    assert(h2_pal_net_tcp_connect(base, second_client, &address, 1000) == H2_PAL_OK);
    assert(h2_pal_net_tcp_accept(net, listener, &data, NULL, 1000) == H2_PAL_OK);
    assert(nodelay(data) == 0);
    h2_pal_net_close(net, data);
    h2_pal_net_close(net, control);
    h2_pal_net_close(base, client);
    h2_pal_net_close(base, second_client);
    h2_pal_net_close(base, listener);
    h2_iperf_test_env_deinit(&env);
    return 0;
}
