#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include "h2_bk_udp_receive.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

static uint64_t now_ms(void) {
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}
int main(void) {
    alarm(5u); /* An accidentally infinite socket wait must fail the test. */
    int receiver = socket(AF_INET, SOCK_DGRAM, 0);
    int sender = socket(AF_INET, SOCK_DGRAM, 0);
    assert(receiver >= 0 && sender >= 0);
    struct sockaddr_in destination = {.sin_family = AF_INET};
    destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(receiver, (struct sockaddr *)&destination, sizeof(destination)) == 0);
    socklen_t length = sizeof(destination);
    assert(getsockname(receiver, (struct sockaddr *)&destination, &length) == 0);
    struct timeval original = {.tv_sec = 2};
    assert(setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, &original, sizeof(original)) == 0);

    uint8_t bytes[8] = {0};
    uint64_t start = now_ms();
    assert(h2_bk_udp_receive(receiver, bytes, sizeof(bytes), NULL, NULL, 0u) ==
           H2_PAL_ERR_WOULD_BLOCK);
    assert(now_ms() - start < 250u);
    start = now_ms();
    assert(h2_bk_udp_receive(receiver, bytes, sizeof(bytes), NULL, NULL, 30u) ==
           H2_PAL_ERR_TIMEOUT);
    assert(now_ms() - start >= 10u && now_ms() - start < 500u);
    struct timeval unchanged = {0};
    length = sizeof(unchanged);
    assert(getsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, &unchanged, &length) == 0);
    assert(unchanged.tv_sec == original.tv_sec && unchanged.tv_usec == original.tv_usec);

    /* A zero-length UDP datagram is a valid packet, not end-of-stream. */
    assert(sendto(sender, bytes, 0, 0, (struct sockaddr *)&destination,
                  sizeof(destination)) == 0);
    assert(h2_bk_udp_receive(receiver, bytes, sizeof(bytes), NULL, NULL, 100u) == 0);
    const uint8_t payload[] = {0, 255, 128, 1};
    assert(sendto(sender, payload, sizeof(payload), 0, (struct sockaddr *)&destination,
                  sizeof(destination)) == (int)sizeof(payload));
    struct sockaddr_in source = {0};
    length = sizeof(source);
    assert(h2_bk_udp_receive(receiver, bytes, sizeof(bytes),
        (struct sockaddr *)&source, &length, 100u) == (int)sizeof(payload));
    assert(source.sin_family == AF_INET && source.sin_port != 0);
    assert(memcmp(bytes, payload, sizeof(payload)) == 0);
    assert(h2_bk_udp_receive(receiver, bytes, sizeof(bytes), NULL, NULL, 0u) ==
           H2_PAL_ERR_WOULD_BLOCK);
    close(sender);
    close(receiver);
    return 0;
}
