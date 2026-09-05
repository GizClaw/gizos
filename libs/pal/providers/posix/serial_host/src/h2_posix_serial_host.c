#include "h2_posix_serial_host_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <IOKit/serial/ioss.h>
#endif

struct h2_pal_serial_host_session {
    int fd;
    int closed;
    pthread_mutex_t mutex;
    struct termios original_termios;
    int has_original_termios;
    h2_pal_uart_io_stream_api_t stream;
};

static h2_pal_result_t serial_configure(
    void *user,
    const h2_pal_uart_io_stream_config_t *config);
static h2_pal_result_t serial_read(
    void *user,
    void *buffer,
    size_t len,
    size_t *out_read,
    uint32_t timeout_ms);
static h2_pal_result_t serial_write(
    void *user,
    const void *buffer,
    size_t len,
    size_t *out_written,
    uint32_t timeout_ms);
static h2_pal_result_t serial_flush(void *user);

static const h2_pal_uart_io_stream_vtable_t serial_stream_vtable = {
    .configure = serial_configure,
    .read = serial_read,
    .write = serial_write,
    .flush = serial_flush,
};

static int64_t monotonic_ms(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return 0;
    }
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

static h2_pal_result_t map_io_error(int error_number) {
    switch (error_number) {
        case EAGAIN:
#if EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:
#endif
            return H2_PAL_ERR_WOULD_BLOCK;
        case ETIMEDOUT:
            return H2_PAL_ERR_TIMEOUT;
        case EBADF:
        case EIO:
        case ENODEV:
        case ENXIO:
        case EPIPE:
            return H2_PAL_ERR_CLOSED;
        default:
            return H2_PAL_ERR_IO;
    }
}

static h2_pal_result_t map_open_error(int error_number) {
    switch (error_number) {
        case ENOENT:
        case ENODEV:
        case ENXIO:
            return H2_PAL_ERR_NOT_FOUND;
        case EACCES:
        case EPERM:
        case EBUSY:
        case EAGAIN:
            return H2_PAL_ERR_UNAVAILABLE;
        default:
            return H2_PAL_ERR_IO;
    }
}

#if defined(__APPLE__)
static int set_custom_speed_native(
    int fd,
    unsigned long request,
    void *speed) {
    return ioctl(fd, request, speed);
}

static const h2_posix_serial_host_darwin_ops_t darwin_ops = {
    .set_attributes = tcsetattr,
    .set_custom_speed = set_custom_speed_native,
};

h2_pal_result_t h2_posix_serial_host_apply_darwin_custom_speed(
    int fd,
    uint32_t baud_rate,
    const struct termios *original_attributes,
    const h2_posix_serial_host_darwin_ops_t *ops) {
    speed_t arbitrary_speed;
    int error_number;
    if (original_attributes == NULL || ops == NULL ||
        ops->set_attributes == NULL || ops->set_custom_speed == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    arbitrary_speed = (speed_t)baud_rate;
    if (ops->set_custom_speed(fd, IOSSIOSPEED, &arbitrary_speed) == 0) {
        return H2_PAL_OK;
    }
    error_number = errno;
    (void)ops->set_attributes(fd, TCSANOW, original_attributes);
    if (error_number == EINVAL || error_number == ENOTTY) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return map_io_error(error_number);
}
#endif

static speed_t baud_constant(uint32_t baud_rate) {
    switch (baud_rate) {
        case 50u:
            return B50;
        case 75u:
            return B75;
        case 110u:
            return B110;
        case 300u:
            return B300;
        case 600u:
            return B600;
        case 1200u:
            return B1200;
        case 2400u:
            return B2400;
        case 4800u:
            return B4800;
        case 9600u:
            return B9600;
        case 19200u:
            return B19200;
        case 38400u:
            return B38400;
        case 57600u:
            return B57600;
        case 115200u:
            return B115200;
#ifdef B230400
        case 230400u:
            return B230400;
#endif
#ifdef B460800
        case 460800u:
            return B460800;
#endif
#ifdef B921600
        case 921600u:
            return B921600;
#endif
        default:
            return (speed_t)0;
    }
}

static h2_pal_result_t configure_locked(
    h2_pal_serial_host_session_t *session,
    const h2_pal_uart_io_stream_config_t *config) {
    struct termios attributes;
#if defined(__APPLE__)
    struct termios original_attributes;
#endif
    speed_t speed;
#if defined(__APPLE__)
    int use_arbitrary_speed = 0;
#endif

    if (session->closed) {
        return H2_PAL_ERR_CLOSED;
    }
    if (config->parity != H2_PAL_UART_PARITY_NONE &&
        config->parity != H2_PAL_UART_PARITY_EVEN &&
        config->parity != H2_PAL_UART_PARITY_ODD) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (config->stop_bits != 1u && config->stop_bits != 2u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if ((config->flow_control & ~H2_PAL_UART_FLOW_CONTROL_RTS_CTS) != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    speed = baud_constant(config->baud_rate);
    if (speed == (speed_t)0) {
#if defined(__APPLE__)
        if (config->baud_rate == 0u) {
            return H2_PAL_ERR_UNSUPPORTED;
        }
        speed = B9600;
        use_arbitrary_speed = 1;
#else
        return H2_PAL_ERR_UNSUPPORTED;
#endif
    }
    if (tcgetattr(session->fd, &attributes) != 0) {
        return map_io_error(errno);
    }
#if defined(__APPLE__)
    original_attributes = attributes;
#endif

    cfmakeraw(&attributes);
    attributes.c_cflag |= CLOCAL | CREAD;
    attributes.c_cflag &= ~(CSIZE | PARENB | PARODD | CSTOPB);
    switch (config->data_bits) {
        case 5u:
            attributes.c_cflag |= CS5;
            break;
        case 6u:
            attributes.c_cflag |= CS6;
            break;
        case 7u:
            attributes.c_cflag |= CS7;
            break;
        case 8u:
            attributes.c_cflag |= CS8;
            break;
        default:
            return H2_PAL_ERR_INVALID_ARG;
    }
    if (config->stop_bits == 2u) {
        attributes.c_cflag |= CSTOPB;
    }
    if (config->parity != H2_PAL_UART_PARITY_NONE) {
        attributes.c_cflag |= PARENB;
        if (config->parity == H2_PAL_UART_PARITY_ODD) {
            attributes.c_cflag |= PARODD;
        }
    }
#if defined(__APPLE__)
    attributes.c_cflag &= ~(CCTS_OFLOW | CRTS_IFLOW);
    if ((config->flow_control & H2_PAL_UART_FLOW_CONTROL_CTS) != 0u) {
        attributes.c_cflag |= CCTS_OFLOW;
    }
    if ((config->flow_control & H2_PAL_UART_FLOW_CONTROL_RTS) != 0u) {
        attributes.c_cflag |= CRTS_IFLOW;
    }
#elif defined(CRTSCTS)
    attributes.c_cflag &= ~CRTSCTS;
    if (config->flow_control != H2_PAL_UART_FLOW_CONTROL_NONE) {
        if (config->flow_control != H2_PAL_UART_FLOW_CONTROL_RTS_CTS) {
            return H2_PAL_ERR_UNSUPPORTED;
        }
        attributes.c_cflag |= CRTSCTS;
    }
#else
    if (config->flow_control != H2_PAL_UART_FLOW_CONTROL_NONE) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
#endif
    attributes.c_cc[VMIN] = 0;
    attributes.c_cc[VTIME] = 0;
    if (cfsetispeed(&attributes, speed) != 0 ||
        cfsetospeed(&attributes, speed) != 0) {
        return map_io_error(errno);
    }
    if (tcsetattr(session->fd, TCSANOW, &attributes) != 0) {
        return map_io_error(errno);
    }
#if defined(__APPLE__)
    if (use_arbitrary_speed) {
        return h2_posix_serial_host_apply_darwin_custom_speed(
            session->fd,
            config->baud_rate,
            &original_attributes,
            &darwin_ops);
    }
#endif
    return H2_PAL_OK;
}

static h2_pal_result_t serial_configure(
    void *user,
    const h2_pal_uart_io_stream_config_t *config) {
    h2_pal_serial_host_session_t *session =
        (h2_pal_serial_host_session_t *)user;
    h2_pal_result_t result;
    if (session == NULL || config == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&session->mutex);
    result = configure_locked(session, config);
    pthread_mutex_unlock(&session->mutex);
    return result;
}

static h2_pal_result_t wait_for_fd(
    int fd,
    short events,
    uint32_t timeout_ms) {
    struct pollfd descriptor = {
        .fd = fd,
        .events = events,
    };
    const int poll_timeout =
        timeout_ms > (uint32_t)INT_MAX ? INT_MAX : (int)timeout_ms;
    int result;
    do {
        result = poll(&descriptor, 1, poll_timeout);
    } while (result < 0 && errno == EINTR);
    if (result == 0) {
        return H2_PAL_ERR_TIMEOUT;
    }
    if (result < 0) {
        return map_io_error(errno);
    }
    if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return H2_PAL_ERR_CLOSED;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t serial_read(
    void *user,
    void *buffer,
    size_t len,
    size_t *out_read,
    uint32_t timeout_ms) {
    h2_pal_serial_host_session_t *session =
        (h2_pal_serial_host_session_t *)user;
    h2_pal_result_t result;
    ssize_t count;
    if (session == NULL || buffer == NULL || out_read == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_read = 0u;
    if (len == 0u) {
        return H2_PAL_OK;
    }
    pthread_mutex_lock(&session->mutex);
    if (session->closed) {
        pthread_mutex_unlock(&session->mutex);
        return H2_PAL_ERR_CLOSED;
    }
    result = wait_for_fd(session->fd, POLLIN, timeout_ms);
    if (result != H2_PAL_OK) {
        pthread_mutex_unlock(&session->mutex);
        return result;
    }
    do {
        count = read(session->fd, buffer, len);
    } while (count < 0 && errno == EINTR);
    if (count < 0) {
        result = map_io_error(errno);
    } else if (count == 0) {
        result = H2_PAL_ERR_CLOSED;
    } else {
        *out_read = (size_t)count;
        result = H2_PAL_OK;
    }
    pthread_mutex_unlock(&session->mutex);
    return result;
}

static h2_pal_result_t serial_write(
    void *user,
    const void *buffer,
    size_t len,
    size_t *out_written,
    uint32_t timeout_ms) {
    h2_pal_serial_host_session_t *session =
        (h2_pal_serial_host_session_t *)user;
    const uint8_t *bytes = (const uint8_t *)buffer;
    int64_t deadline;
    h2_pal_result_t result = H2_PAL_OK;
    if (session == NULL || buffer == NULL || out_written == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_written = 0u;
    if (len == 0u) {
        return H2_PAL_OK;
    }
    pthread_mutex_lock(&session->mutex);
    if (session->closed) {
        pthread_mutex_unlock(&session->mutex);
        return H2_PAL_ERR_CLOSED;
    }
    deadline = monotonic_ms() + timeout_ms;
    while (*out_written < len) {
        int64_t remaining = deadline - monotonic_ms();
        ssize_t count;
        if (remaining < 0) {
            result = H2_PAL_ERR_TIMEOUT;
            break;
        }
        result = wait_for_fd(
            session->fd,
            POLLOUT,
            (uint32_t)remaining);
        if (result != H2_PAL_OK) {
            break;
        }
        do {
            count = write(
                session->fd,
                bytes + *out_written,
                len - *out_written);
        } while (count < 0 && errno == EINTR);
        if (count < 0) {
            result = map_io_error(errno);
            if (result == H2_PAL_ERR_WOULD_BLOCK &&
                monotonic_ms() <= deadline) {
                continue;
            }
            break;
        }
        if (count == 0) {
            result = H2_PAL_ERR_CLOSED;
            break;
        }
        *out_written += (size_t)count;
    }
    pthread_mutex_unlock(&session->mutex);
    return result;
}

static h2_pal_result_t serial_flush(void *user) {
    h2_pal_serial_host_session_t *session =
        (h2_pal_serial_host_session_t *)user;
    h2_pal_result_t result = H2_PAL_OK;
    const int64_t deadline =
        monotonic_ms() + H2_PAL_SERIAL_HOST_FLUSH_TIMEOUT_MS;
    if (session == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&session->mutex);
    if (session->closed) {
        result = H2_PAL_ERR_CLOSED;
    } else {
        for (;;) {
            int pending = 0;
            const struct timespec delay = {
                .tv_sec = 0,
                .tv_nsec = 1000000,
            };
            if (ioctl(session->fd, TIOCOUTQ, &pending) != 0) {
                result = errno == ENOTTY
                    ? H2_PAL_ERR_UNSUPPORTED
                    : map_io_error(errno);
                break;
            }
            if (pending <= 0) {
                break;
            }
            if (monotonic_ms() >= deadline) {
                result = H2_PAL_ERR_TIMEOUT;
                break;
            }
            (void)nanosleep(&delay, NULL);
        }
    }
    pthread_mutex_unlock(&session->mutex);
    return result;
}

static h2_pal_result_t serial_scan(
    void *user,
    h2_pal_serial_host_snapshot_t **out_snapshot) {
    (void)user;
    return h2_posix_serial_host_scan_native(out_snapshot);
}

static h2_pal_result_t serial_snapshot_count(
    void *user,
    const h2_pal_serial_host_snapshot_t *snapshot,
    size_t *out_count) {
    (void)user;
    if (snapshot == NULL || out_count == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_count = snapshot->count;
    return H2_PAL_OK;
}

static h2_pal_result_t serial_snapshot_get(
    void *user,
    const h2_pal_serial_host_snapshot_t *snapshot,
    size_t index,
    h2_pal_serial_host_port_info_t *out_info) {
    (void)user;
    if (snapshot == NULL || out_info == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (index >= snapshot->count) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    *out_info = snapshot->ports[index];
    return H2_PAL_OK;
}

static h2_pal_result_t serial_snapshot_destroy(
    void *user,
    h2_pal_serial_host_snapshot_t **inout_snapshot) {
    h2_pal_serial_host_snapshot_t *snapshot;
    (void)user;
    if (inout_snapshot == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    snapshot = *inout_snapshot;
    if (snapshot == NULL) {
        return H2_PAL_OK;
    }
    free(snapshot->ports);
    free(snapshot);
    *inout_snapshot = NULL;
    return H2_PAL_OK;
}

static h2_pal_result_t serial_open(
    void *user,
    const char *port_id,
    const h2_pal_uart_io_stream_config_t *config,
    h2_pal_serial_host_session_t **out_session) {
    h2_pal_serial_host_session_t *session;
    h2_pal_result_t result;
    int fd;
    (void)user;
    if (port_id == NULL || config == NULL || out_session == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_session = NULL;
    fd = open(port_id, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return map_open_error(errno);
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        result = map_open_error(errno);
        close(fd);
        return result;
    }
    session = (h2_pal_serial_host_session_t *)calloc(1u, sizeof(*session));
    if (session == NULL) {
        close(fd);
        return H2_PAL_ERR_NO_MEMORY;
    }
    session->fd = fd;
    session->stream.user = session;
    session->stream.vtable = &serial_stream_vtable;
    if (pthread_mutex_init(&session->mutex, NULL) != 0) {
        close(fd);
        free(session);
        return H2_PAL_ERR_IO;
    }
    if (tcgetattr(fd, &session->original_termios) == 0) {
        session->has_original_termios = 1;
    }
    result = configure_locked(session, config);
    if (result != H2_PAL_OK) {
        pthread_mutex_destroy(&session->mutex);
        close(fd);
        free(session);
        return result;
    }
    *out_session = session;
    return H2_PAL_OK;
}

static h2_pal_result_t serial_session_stream(
    void *user,
    h2_pal_serial_host_session_t *session,
    const h2_pal_uart_io_stream_api_t **out_stream) {
    (void)user;
    if (session == NULL || out_stream == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&session->mutex);
    if (session->closed) {
        pthread_mutex_unlock(&session->mutex);
        return H2_PAL_ERR_CLOSED;
    }
    *out_stream = &session->stream;
    pthread_mutex_unlock(&session->mutex);
    return H2_PAL_OK;
}

static h2_pal_result_t serial_set_control_lines(
    void *user,
    h2_pal_serial_host_session_t *session,
    uint32_t line_mask,
    uint32_t asserted_lines) {
    int set_bits = 0;
    int clear_bits = 0;
    h2_pal_result_t result = H2_PAL_OK;
    (void)user;
    if (session == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&session->mutex);
    if (session->closed) {
        result = H2_PAL_ERR_CLOSED;
    } else {
        if ((line_mask & H2_PAL_SERIAL_HOST_CONTROL_DTR) != 0u) {
            if ((asserted_lines & H2_PAL_SERIAL_HOST_CONTROL_DTR) != 0u) {
                set_bits |= TIOCM_DTR;
            } else {
                clear_bits |= TIOCM_DTR;
            }
        }
        if ((line_mask & H2_PAL_SERIAL_HOST_CONTROL_RTS) != 0u) {
            if ((asserted_lines & H2_PAL_SERIAL_HOST_CONTROL_RTS) != 0u) {
                set_bits |= TIOCM_RTS;
            } else {
                clear_bits |= TIOCM_RTS;
            }
        }
        /* Use the atomic bit operations used by established serial clients.
         * Some USB-UART drivers accept TIOCMGET/TIOCMSET without applying the
         * requested output transition, while TIOCMBIS/TIOCMBIC correctly
         * update the physical DTR/RTS lines. */
        if (clear_bits != 0 &&
            ioctl(session->fd, TIOCMBIC, &clear_bits) != 0) {
            result =
                errno == ENOTTY ? H2_PAL_ERR_UNSUPPORTED : map_io_error(errno);
        } else if (set_bits != 0 &&
                   ioctl(session->fd, TIOCMBIS, &set_bits) != 0) {
            result =
                errno == ENOTTY ? H2_PAL_ERR_UNSUPPORTED : map_io_error(errno);
        }
    }
    pthread_mutex_unlock(&session->mutex);
    return result;
}

static h2_pal_result_t serial_get_control_lines(
    void *user,
    h2_pal_serial_host_session_t *session,
    uint32_t *out_asserted_lines) {
    int bits;
    h2_pal_result_t result = H2_PAL_OK;
    (void)user;
    if (session == NULL || out_asserted_lines == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_asserted_lines = 0u;
    pthread_mutex_lock(&session->mutex);
    if (session->closed) {
        result = H2_PAL_ERR_CLOSED;
    } else if (ioctl(session->fd, TIOCMGET, &bits) != 0) {
        result = errno == ENOTTY ? H2_PAL_ERR_UNSUPPORTED : map_io_error(errno);
    } else {
        if ((bits & TIOCM_DTR) != 0) {
            *out_asserted_lines |= H2_PAL_SERIAL_HOST_CONTROL_DTR;
        }
        if ((bits & TIOCM_RTS) != 0) {
            *out_asserted_lines |= H2_PAL_SERIAL_HOST_CONTROL_RTS;
        }
    }
    pthread_mutex_unlock(&session->mutex);
    return result;
}

static h2_pal_result_t serial_close(
    void *user,
    h2_pal_serial_host_session_t **inout_session) {
    h2_pal_serial_host_session_t *session;
    h2_pal_result_t result = H2_PAL_OK;
    (void)user;
    if (inout_session == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    session = *inout_session;
    if (session == NULL) {
        return H2_PAL_OK;
    }
    pthread_mutex_lock(&session->mutex);
    session->closed = 1;
    if (session->has_original_termios &&
        tcsetattr(session->fd, TCSANOW, &session->original_termios) != 0 &&
        errno != ENXIO && errno != ENODEV && errno != EIO) {
        result = H2_PAL_ERR_IO;
    }
    if (close(session->fd) != 0 && result == H2_PAL_OK) {
        result = map_io_error(errno);
    }
    session->fd = -1;
    pthread_mutex_unlock(&session->mutex);
    pthread_mutex_destroy(&session->mutex);
    free(session);
    *inout_session = NULL;
    return result;
}

static const h2_pal_serial_host_vtable_t serial_host_vtable = {
    .scan = serial_scan,
    .snapshot_count = serial_snapshot_count,
    .snapshot_get = serial_snapshot_get,
    .snapshot_destroy = serial_snapshot_destroy,
    .open = serial_open,
    .session_stream = serial_session_stream,
    .set_control_lines = serial_set_control_lines,
    .get_control_lines = serial_get_control_lines,
    .close = serial_close,
};

static const h2_pal_serial_host_api_t serial_host_api = {
    .user = NULL,
    .vtable = &serial_host_vtable,
};

const h2_pal_serial_host_api_t *h2_posix_serial_host_api(void) {
    return &serial_host_api;
}
