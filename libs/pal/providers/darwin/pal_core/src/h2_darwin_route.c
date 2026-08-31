#include "h2_darwin_netif_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

h2_pal_result_t h2_darwin_netif_os_default_name(
    char out_name[H2_PAL_NETIF_NAME_MAX]) {
    int fd = socket(PF_ROUTE, SOCK_RAW, AF_INET);
    if (fd < 0) {
        return H2_PAL_ERR_IO;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        close(fd);
        return H2_PAL_ERR_IO;
    }
    struct {
        struct rt_msghdr header;
        struct sockaddr_in destination;
    } request;
    memset(&request, 0, sizeof(request));
    request.header.rtm_msglen = sizeof(request);
    request.header.rtm_version = RTM_VERSION;
    request.header.rtm_type = RTM_GET;
    request.header.rtm_addrs = RTA_DST;
    request.header.rtm_seq = 1;
    request.header.rtm_pid = getpid();
    request.destination.sin_len = sizeof(request.destination);
    request.destination.sin_family = AF_INET;
    if (write(fd, &request, sizeof(request)) != (ssize_t)sizeof(request)) {
        close(fd);
        return H2_PAL_ERR_IO;
    }
    uint8_t response[2048];
    struct timespec started;
    if (clock_gettime(CLOCK_MONOTONIC, &started) != 0) {
        close(fd);
        return H2_PAL_ERR_IO;
    }
    for (;;) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            close(fd);
            return H2_PAL_ERR_IO;
        }
        int64_t elapsed_ms =
            (int64_t)(now.tv_sec - started.tv_sec) * 1000 +
            (int64_t)(now.tv_nsec - started.tv_nsec) / 1000000;
        if (elapsed_ms >= 1000) {
            close(fd);
            return H2_PAL_ERR_TIMEOUT;
        }
        struct pollfd wait = {.fd = fd, .events = POLLIN};
        int poll_rc;
        do {
            poll_rc = poll(&wait, 1u, (int)(1000 - elapsed_ms));
        } while (poll_rc < 0 && errno == EINTR);
        if (poll_rc == 0) {
            close(fd);
            return H2_PAL_ERR_TIMEOUT;
        }
        if (poll_rc < 0 || (wait.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            close(fd);
            return H2_PAL_ERR_IO;
        }
        ssize_t size = read(fd, response, sizeof(response));
        if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        if (size <= 0) {
            close(fd);
            return H2_PAL_ERR_IO;
        }
        if (size < (ssize_t)sizeof(struct rt_msghdr)) {
            continue;
        }
        const struct rt_msghdr *header =
            (const struct rt_msghdr *)response;
        if (header->rtm_seq != request.header.rtm_seq ||
            header->rtm_pid != request.header.rtm_pid ||
            header->rtm_type != RTM_GET) {
            continue;
        }
        close(fd);
        if (header->rtm_errno != 0 || header->rtm_index == 0 ||
            if_indextoname(header->rtm_index, out_name) == NULL ||
            strnlen(out_name, H2_PAL_NETIF_NAME_MAX) >=
                H2_PAL_NETIF_NAME_MAX) {
            return H2_PAL_ERR_NOT_FOUND;
        }
        return H2_PAL_OK;
    }
}

h2_pal_result_t h2_darwin_netif_os_monitor_open(
    h2_darwin_netif_os_monitor_t *monitor) {
    int wake_pipe[2];
    if (pipe(wake_pipe) != 0) {
        return H2_PAL_ERR_IO;
    }
    monitor->route_fd = socket(PF_ROUTE, SOCK_RAW, AF_INET);
    if (monitor->route_fd < 0) {
        close(wake_pipe[0]);
        close(wake_pipe[1]);
        return H2_PAL_ERR_IO;
    }
    monitor->wake_read_fd = wake_pipe[0];
    monitor->wake_write_fd = wake_pipe[1];
    return H2_PAL_OK;
}

h2_pal_result_t h2_darwin_netif_os_monitor_wait(
    h2_darwin_netif_os_monitor_t *monitor) {
    struct pollfd fds[2] = {
        {.fd = monitor->route_fd, .events = POLLIN},
        {.fd = monitor->wake_read_fd, .events = POLLIN},
    };
    int rc;
    do {
        rc = poll(fds, 2u, -1);
    } while (rc < 0 && errno == EINTR);
    if (rc <= 0 || (fds[1].revents & POLLIN) != 0) {
        return H2_PAL_ERR_CLOSED;
    }
    uint8_t message[2048];
    return read(monitor->route_fd, message, sizeof(message)) > 0
        ? H2_PAL_OK : H2_PAL_ERR_IO;
}

void h2_darwin_netif_os_monitor_wake(
    h2_darwin_netif_os_monitor_t *monitor) {
    if (monitor->wake_write_fd >= 0) {
        const uint8_t wake = 1u;
        ssize_t written = write(monitor->wake_write_fd, &wake, sizeof(wake));
        (void)written;
    }
}

void h2_darwin_netif_os_monitor_close(
    h2_darwin_netif_os_monitor_t *monitor) {
    if (monitor->route_fd >= 0) close(monitor->route_fd);
    if (monitor->wake_read_fd >= 0) close(monitor->wake_read_fd);
    if (monitor->wake_write_fd >= 0) close(monitor->wake_write_fd);
    monitor->route_fd = -1;
    monitor->wake_read_fd = -1;
    monitor->wake_write_fd = -1;
}
