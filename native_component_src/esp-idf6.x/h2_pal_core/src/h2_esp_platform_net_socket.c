#include "h2_esp_platform_net_socket.h"

#include "h2/pal/core/h2_pal_errors.h"

#include <errno.h>
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
