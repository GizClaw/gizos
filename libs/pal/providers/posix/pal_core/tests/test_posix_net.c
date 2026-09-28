#include "h2_posix_pal_core.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static uint64_t monotonic_ms(void) {
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (uint64_t)now.tv_sec * 1000u +
           (uint64_t)now.tv_nsec / 1000000u;
}

static void make_stream_pair(int pair[2]) {
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    int size = 4096;
    assert(setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0);
}

static void fill_send_buffer(
    const h2_pal_net_api_t *net,
    h2_pal_net_socket_t socket_fd) {
    uint8_t data[4096] = {0};
    for (;;) {
        int result = h2_pal_net_tcp_send_timeout(
            net, socket_fd, data, sizeof(data), 0u);
        if (result == H2_PAL_ERR_WOULD_BLOCK) {
            return;
        }
        assert(result > 0);
    }
}

int main(void) {
    const h2_pal_net_api_t *net = h2_posix_net_api();
    assert(net != NULL && net->vtable != NULL &&
           net->vtable->tcp_send_timeout != NULL);

    int pair[2];
    make_stream_pair(pair);
    size_t payload_len = 1024u * 1024u;
    uint8_t *payload = malloc(payload_len);
    assert(payload != NULL);
    memset(payload, 0x5a, payload_len);
    int partial = h2_pal_net_tcp_send_timeout(
        net, pair[0], payload, payload_len, 100u);
    assert(partial > 0 && (size_t)partial < payload_len);
    fill_send_buffer(net, pair[0]);
    assert(h2_pal_net_tcp_send_timeout(
               net, pair[0], payload, 1u, 0u) ==
           H2_PAL_ERR_WOULD_BLOCK);
    uint64_t started_ms = monotonic_ms();
    assert(h2_pal_net_tcp_send_timeout(
               net, pair[0], payload, 1u, 30u) ==
           H2_PAL_ERR_TIMEOUT);
    uint64_t elapsed_ms = monotonic_ms() - started_ms;
    assert(elapsed_ms >= 20u && elapsed_ms < 500u);
    close(pair[1]);
    assert(h2_pal_net_tcp_send_timeout(
               net, pair[0], payload, 1u, 100u) ==
           H2_PAL_ERR_CLOSED);
    close(pair[0]);

    make_stream_pair(pair);
    close(pair[1]);
    uint8_t byte = 0u;
    assert(h2_pal_net_tcp_recv(net, pair[0], &byte, 1u, 100u) ==
           H2_PAL_ERR_CLOSED);
    close(pair[0]);
    free(payload);
    return 0;
}
