#include "address.h"
#include "socket.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

typedef struct test_net_state {
    h2_pal_net_family_t open_family;
    uint16_t open_port;
    h2_pal_net_addr_t send_addr;
    size_t send_len;
    h2_pal_net_addr_t receive_addr;
    int receive_result;
    int close_count;
} test_net_state_t;

static int test_udp_open(
    void *user,
    h2_pal_net_family_t family,
    uint16_t port,
    h2_pal_net_socket_t *out_socket,
    h2_pal_net_addr_t *out_bind_addr) {
    test_net_state_t *state = user;
    state->open_family = family;
    state->open_port = port;
    *out_socket = 7;
    memset(out_bind_addr, 0, sizeof(*out_bind_addr));
    out_bind_addr->family = family;
    out_bind_addr->port = port == 0u ? 50000u : port;
    out_bind_addr->ip[0] = family == H2_PAL_NET_FAMILY_IPV4 ? 127u : 0x20u;
    return H2_PAL_OK;
}

static int test_udp_sendto(
    void *user,
    h2_pal_net_socket_t socket,
    const h2_pal_net_addr_t *addr,
    const uint8_t *data,
    size_t len) {
    test_net_state_t *state = user;
    assert(socket == 7);
    assert(data != NULL);
    state->send_addr = *addr;
    state->send_len = len;
    return (int)len;
}

static int test_udp_recvfrom(
    void *user,
    h2_pal_net_socket_t socket,
    h2_pal_net_addr_t *out_addr,
    uint8_t *data,
    size_t len,
    uint32_t timeout_ms) {
    test_net_state_t *state = user;
    assert(socket == 7);
    assert(data != NULL);
    assert(len >= 2u);
    assert(timeout_ms == 10u);
    *out_addr = state->receive_addr;
    data[0] = 1u;
    data[1] = 2u;
    return state->receive_result;
}

static void test_close(void *user, h2_pal_net_socket_t socket) {
    test_net_state_t *state = user;
    assert(socket == 7);
    state->close_count++;
}

static const h2_pal_net_vtable_t test_net_vtable = {
    .udp_open = test_udp_open,
    .udp_sendto = test_udp_sendto,
    .udp_recvfrom = test_udp_recvfrom,
    .close = test_close,
};

static void test_ipv4_format(void) {
    h2_pal_net_addr_t address = {
        .family = H2_PAL_NET_FAMILY_IPV4,
        .port = 3478u,
        .ip = {192u, 0u, 2u, 10u},
    };
    char text[H2_PEER_NET_ADDR_STRING_SIZE];
    assert(h2_peer_net_addr_family_is_valid(address.family));
    assert(h2_peer_net_addr_format(&address, text, sizeof(text)));
    assert(strcmp(text, "192.0.2.10") == 0);
    assert(address.port == 3478u);
}

static void test_ipv6_format(void) {
    h2_pal_net_addr_t address = {
        .family = H2_PAL_NET_FAMILY_IPV6,
        .port = 65535u,
        .ip = {
            0x20u, 0x01u, 0x0du, 0xb8u,
            0x00u, 0x01u, 0x00u, 0x02u,
            0x00u, 0x03u, 0x00u, 0x04u,
            0x00u, 0x05u, 0x00u, 0x06u,
        },
    };
    char text[H2_PEER_NET_ADDR_STRING_SIZE];
    assert(h2_peer_net_addr_family_is_valid(address.family));
    assert(h2_peer_net_addr_format(&address, text, sizeof(text)));
    assert(strcmp(
               text,
               "2001:0db8:0001:0002:0003:0004:0005:0006") == 0);
    assert(address.ip[15] == 0x06u);
    assert(address.port == 65535u);
}

static void test_invalid_and_truncated_output(void) {
    h2_pal_net_addr_t address = {
        .family = (h2_pal_net_family_t)5,
    };
    char text[8] = "stale";
    assert(!h2_peer_net_addr_family_is_valid(address.family));
    assert(!h2_peer_net_addr_format(&address, text, sizeof(text)));
    assert(text[0] == '\0');

    address.family = H2_PAL_NET_FAMILY_IPV4;
    address.ip[0] = 255u;
    address.ip[1] = 255u;
    address.ip[2] = 255u;
    address.ip[3] = 255u;
    memset(text, 'x', sizeof(text));
    assert(!h2_peer_net_addr_format(&address, text, sizeof(text)));
    assert(text[0] == '\0');
    assert(!h2_peer_net_addr_format(&address, NULL, 0u));
}

static void test_socket_preserves_pal_address(void) {
    test_net_state_t state;
    memset(&state, 0, sizeof(state));
    h2_pal_net_api_t net = {
        .user = &state,
        .vtable = &test_net_vtable,
    };
    UdpSocket socket;
    memset(&socket, 0, sizeof(socket));
    assert(udp_socket_open(
               &socket, &net, H2_PAL_NET_FAMILY_IPV4, 0u) == 0);
    assert(state.open_family == H2_PAL_NET_FAMILY_IPV4);
    assert(state.open_port == 0u);
    assert(socket.bind_addr.family == H2_PAL_NET_FAMILY_IPV4);
    assert(socket.bind_addr.port == 50000u);

    h2_pal_net_addr_t peer = {
        .family = H2_PAL_NET_FAMILY_IPV6,
        .port = 3478u,
        .ip = {0x20u, 0x01u, 0x0du, 0xb8u, 0u, 0u, 0u, 0u,
               0u, 0u, 0u, 0u, 0u, 0u, 0u, 1u},
    };
    const uint8_t payload[] = {3u, 4u};
    assert(udp_socket_sendto(
               &socket, &peer, payload, sizeof(payload)) ==
           (int)sizeof(payload));
    assert(state.send_len == sizeof(payload));
    assert(memcmp(&state.send_addr, &peer, sizeof(peer)) == 0);

    state.receive_addr = peer;
    state.receive_result = 2;
    h2_pal_net_addr_t received;
    uint8_t received_payload[2];
    memset(&received, 0, sizeof(received));
    assert(udp_socket_recvfrom(
               &socket,
               &received,
               received_payload,
               sizeof(received_payload),
               10u) == 2);
    assert(memcmp(&received, &peer, sizeof(peer)) == 0);
    assert(received_payload[0] == 1u && received_payload[1] == 2u);

    state.receive_addr.family = (h2_pal_net_family_t)5;
    memset(&received, 0xa5, sizeof(received));
    assert(udp_socket_recvfrom(
               &socket,
               &received,
               received_payload,
               sizeof(received_payload),
               10u) != 0);
    assert(received.family == 0);

    udp_socket_close(&socket);
    udp_socket_close(&socket);
    assert(state.close_count == 1);
}

int main(void) {
    test_ipv4_format();
    test_ipv6_format();
    test_invalid_and_truncated_output();
    test_socket_preserves_pal_address();
    return 0;
}
