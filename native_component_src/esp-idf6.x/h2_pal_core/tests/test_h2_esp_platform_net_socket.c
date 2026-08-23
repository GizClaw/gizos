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

int main(void) {
    test_zero_timeout_udp_probe();
    test_udp_send_result_mapping();
    return 0;
}
