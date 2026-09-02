#include "h2_esp_platform_net_socket.h"

#include "h2/pal/core/h2_pal_errors.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <sys/select.h>
#include <sys/time.h>

int h2_esp_net_wait_fd(int fd, int write_ready, uint32_t timeout_ms) {
    if (fd < 0 || (write_ready != 0 && write_ready != 1)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval timeout;
    timeout.tv_sec = (long)(timeout_ms / 1000u);
    timeout.tv_usec = (long)((timeout_ms % 1000u) * 1000u);
    int result = select(
        fd + 1,
        write_ready ? NULL : &fds,
        write_ready ? &fds : NULL,
        NULL,
        &timeout);
    if (result == 0) {
        return H2_PAL_ERR_TIMEOUT;
    }
    if (result < 0) {
        return errno == EINTR ? H2_PAL_ERR_WOULD_BLOCK : H2_PAL_ERR_IO;
    }
    return H2_PAL_OK;
}

int h2_esp_net_udp_send_result(int sent, int error_code) {
    if (sent >= 0) {
        return sent;
    }
    return error_code == EAGAIN || error_code == EWOULDBLOCK ||
                   error_code == ENOMEM || error_code == ENOBUFS
               ? H2_PAL_ERR_WOULD_BLOCK
               : H2_PAL_ERR_IO;
}

int h2_esp_net_set_recv_timeout(int fd, uint32_t timeout_ms) {
    if (fd < 0 || timeout_ms == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    struct timeval timeout;
    timeout.tv_sec = (long)(timeout_ms / 1000u);
    timeout.tv_usec = (long)((timeout_ms % 1000u) * 1000u);
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
        return H2_PAL_ERR_IO;
    }
    return H2_PAL_OK;
}

static int h2_esp_net_recv_failure(uint32_t timeout_ms, int error_code) {
    if (error_code == EAGAIN || error_code == EWOULDBLOCK || error_code == EINTR) {
        return timeout_ms == 0u ? H2_PAL_ERR_WOULD_BLOCK : H2_PAL_ERR_TIMEOUT;
    }
    if (error_code == EPIPE || error_code == ECONNRESET || error_code == ENOTCONN) {
        return H2_PAL_ERR_CLOSED;
    }
    return H2_PAL_ERR_IO;
}

int h2_esp_net_stream_recv(int fd, uint8_t *data, size_t len, uint32_t timeout_ms) {
    if (fd < 0 || data == NULL || len == 0u || len > (size_t)INT_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    ssize_t received;
    if (timeout_ms == 0u) {
        received = recv(fd, data, len, MSG_DONTWAIT);
    } else {
        int rc = h2_esp_net_set_recv_timeout(fd, timeout_ms);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        received = recv(fd, data, len, 0);
    }
    if (received < 0) {
        return h2_esp_net_recv_failure(timeout_ms, errno);
    }
    return received == 0 ? H2_PAL_ERR_CLOSED : (int)received;
}

int h2_esp_net_datagram_recv(
    int fd,
    uint8_t *data,
    size_t len,
    uint32_t timeout_ms,
    struct sockaddr *addr,
    socklen_t *addr_len) {
    if (fd < 0 || data == NULL || len == 0u || len > (size_t)INT_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    ssize_t got;
    if (timeout_ms == 0u) {
        got = recvfrom(fd, data, len, MSG_DONTWAIT, addr, addr_len);
    } else {
        int rc = h2_esp_net_set_recv_timeout(fd, timeout_ms);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        got = recvfrom(fd, data, len, 0, addr, addr_len);
    }
    if (got < 0) {
        int mapped = h2_esp_net_recv_failure(timeout_ms, errno);
        return mapped == H2_PAL_ERR_CLOSED ? H2_PAL_ERR_IO : mapped;
    }
    return (int)got;
}
