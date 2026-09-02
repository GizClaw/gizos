#include "h2_esp_platform_net_socket.h"

#include "h2/pal/core/h2_pal_errors.h"

#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
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

static void test_zero_timeout_udp_probe(void) {
    assert(h2_esp_net_wait_fd(-1, 0, 0u) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_esp_net_wait_fd(0, 2, 0u) == H2_PAL_ERR_INVALID_ARG);
    int receiver = socket(AF_INET, SOCK_DGRAM, 0);
    int sender = socket(AF_INET, SOCK_DGRAM, 0);
    assert(receiver >= 0 && sender >= 0);
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(receiver, (const struct sockaddr *)&address,
                sizeof(address)) == 0);
    socklen_t address_len = sizeof(address);
    assert(getsockname(receiver, (struct sockaddr *)&address,
                       &address_len) == 0);

    uint64_t started_ms = monotonic_ms();
    assert(h2_esp_net_wait_fd(receiver, 0, 0u) == H2_PAL_ERR_TIMEOUT);
    assert(monotonic_ms() - started_ms < 100u);

    const uint8_t byte = 0x5au;
    assert(sendto(sender, &byte, sizeof(byte), 0,
                  (const struct sockaddr *)&address, sizeof(address)) ==
           (ssize_t)sizeof(byte));
    assert(h2_esp_net_wait_fd(receiver, 0, 100u) == H2_PAL_OK);
    close(sender);
    close(receiver);
}

static void test_udp_send_result_mapping(void) {
    assert(h2_esp_net_udp_send_result(17, 0) == 17);
    assert(h2_esp_net_udp_send_result(-1, EAGAIN) ==
           H2_PAL_ERR_WOULD_BLOCK);
    assert(h2_esp_net_udp_send_result(-1, EWOULDBLOCK) ==
           H2_PAL_ERR_WOULD_BLOCK);
    assert(h2_esp_net_udp_send_result(-1, ENOMEM) ==
           H2_PAL_ERR_WOULD_BLOCK);
    assert(h2_esp_net_udp_send_result(-1, ENOBUFS) ==
           H2_PAL_ERR_WOULD_BLOCK);
    assert(h2_esp_net_udp_send_result(-1, EINVAL) == H2_PAL_ERR_IO);
}

static void make_loopback_tcp_pair(int *out_client, int *out_server) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(listener, (const struct sockaddr *)&address, sizeof(address)) == 0);
    socklen_t address_len = sizeof(address);
    assert(getsockname(listener, (struct sockaddr *)&address, &address_len) == 0);
    assert(listen(listener, 1) == 0);
    int client = socket(AF_INET, SOCK_STREAM, 0);
    assert(client >= 0);
    assert(connect(client, (const struct sockaddr *)&address, sizeof(address)) == 0);
    int server = accept(listener, NULL, NULL);
    assert(server >= 0);
    close(listener);
    *out_client = client;
    *out_server = server;
}

/* The regression that stalled every ESP receiver: a zero PAL timeout must
 * return at once, a bounded timeout must expire, and queued data must be
 * read without waiting for the timeout. */
static void test_stream_recv_timeout_contract(void) {
    uint8_t buffer[8];
    assert(h2_esp_net_stream_recv(-1, buffer, sizeof(buffer), 0u) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_esp_net_set_recv_timeout(0, 0u) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_esp_net_set_recv_timeout(-1, 10u) == H2_PAL_ERR_INVALID_ARG);
    int client;
    int server;
    make_loopback_tcp_pair(&client, &server);

    uint64_t started_ms = monotonic_ms();
    assert(h2_esp_net_stream_recv(server, buffer, sizeof(buffer), 0u) == H2_PAL_ERR_WOULD_BLOCK);
    assert(monotonic_ms() - started_ms < 50u);

    started_ms = monotonic_ms();
    assert(h2_esp_net_stream_recv(server, buffer, sizeof(buffer), 100u) == H2_PAL_ERR_TIMEOUT);
    uint64_t waited_ms = monotonic_ms() - started_ms;
    assert(waited_ms >= 50u && waited_ms < 1000u);

    const uint8_t payload[3] = {1u, 2u, 3u};
    assert(send(client, payload, sizeof(payload), 0) == (ssize_t)sizeof(payload));
    started_ms = monotonic_ms();
    assert(h2_esp_net_stream_recv(server, buffer, sizeof(buffer), 5000u) == 3);
    assert(monotonic_ms() - started_ms < 1000u);
    assert(memcmp(buffer, payload, sizeof(payload)) == 0);
    assert(send(client, payload, 1u, 0) == 1);
    /* Once the byte is queued, a zero timeout must return it without waiting. */
    assert(h2_esp_net_wait_fd(server, 0, 1000u) == H2_PAL_OK);
    assert(h2_esp_net_stream_recv(server, buffer, sizeof(buffer), 0u) == 1);

    close(client);
    assert(h2_esp_net_stream_recv(server, buffer, sizeof(buffer), 100u) == H2_PAL_ERR_CLOSED);
    close(server);
}

static void test_datagram_recv_timeout_contract(void) {
    int receiver = socket(AF_INET, SOCK_DGRAM, 0);
    int sender = socket(AF_INET, SOCK_DGRAM, 0);
    assert(receiver >= 0 && sender >= 0);
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(receiver, (const struct sockaddr *)&address, sizeof(address)) == 0);
    socklen_t address_len = sizeof(address);
    assert(getsockname(receiver, (struct sockaddr *)&address, &address_len) == 0);
    uint8_t buffer[16];
    uint64_t started_ms = monotonic_ms();
    assert(h2_esp_net_datagram_recv(receiver, buffer, sizeof(buffer), 0u, NULL, NULL) == H2_PAL_ERR_WOULD_BLOCK);
    assert(monotonic_ms() - started_ms < 50u);
    started_ms = monotonic_ms();
    assert(h2_esp_net_datagram_recv(receiver, buffer, sizeof(buffer), 100u, NULL, NULL) == H2_PAL_ERR_TIMEOUT);
    uint64_t waited_ms = monotonic_ms() - started_ms;
    assert(waited_ms >= 50u && waited_ms < 1000u);
    const uint8_t payload[4] = {9u, 8u, 7u, 6u};
    assert(sendto(sender, payload, sizeof(payload), 0, (const struct sockaddr *)&address, sizeof(address)) == (ssize_t)sizeof(payload));
    struct sockaddr_storage from;
    socklen_t from_len = sizeof(from);
    assert(h2_esp_net_datagram_recv(receiver, buffer, sizeof(buffer), 5000u, (struct sockaddr *)&from, &from_len) == 4);
    assert(memcmp(buffer, payload, sizeof(payload)) == 0);
    assert(from_len > 0u);
    close(sender);
    close(receiver);
}

int main(void) {
    test_zero_timeout_udp_probe();
    test_udp_send_result_mapping();
    test_stream_recv_timeout_contract();
    test_datagram_recv_timeout_contract();
    return 0;
}
