#ifndef H2_BK_UDP_RECEIVE_H
#define H2_BK_UDP_RECEIVE_H

#include "h2/pal/core/h2_pal_errors.h"
#include <errno.h>
#include <limits.h>
#include <stdint.h>

/* Include the platform's BSD socket declarations before this private helper.
 * SO_RCVTIMEO=0 disables the timeout in lwIP; PAL timeout=0 means no waiting.
 * Readiness plus MSG_DONTWAIT also avoids changing shared socket timeout state
 * and remains bounded if another reader consumes a ready datagram first. */
static inline int h2_bk_udp_receive(int fd, void *data, size_t length,
                                  struct sockaddr *address, socklen_t *address_length,
                                  uint32_t timeout_ms) {
    if (fd < 0 || fd >= FD_SETSIZE || length > INT_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (timeout_ms != 0u) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(fd, &readable);
        struct timeval wait = {
            .tv_sec = (long)(timeout_ms / 1000u),
            .tv_usec = (long)((timeout_ms % 1000u) * 1000u)};
        int ready = select(fd + 1, &readable, NULL, NULL, &wait);
        if (ready == 0) return H2_PAL_ERR_TIMEOUT;
        if (ready < 0) {
            return errno == EINTR ? H2_PAL_ERR_WOULD_BLOCK : H2_PAL_ERR_IO;
        }
    }
    int received = recvfrom(fd, data, (int)length, MSG_DONTWAIT,
                            address, address_length);
    if (received >= 0) return received;
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR
               ? H2_PAL_ERR_WOULD_BLOCK : H2_PAL_ERR_IO;
}
#endif
