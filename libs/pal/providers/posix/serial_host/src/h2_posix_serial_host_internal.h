#ifndef H2_POSIX_SERIAL_HOST_INTERNAL_H
#define H2_POSIX_SERIAL_HOST_INTERNAL_H

#include "h2/pal/os/h2_pal_serial_host.h"

#if defined(__APPLE__)
#include <termios.h>

typedef struct h2_posix_serial_host_darwin_ops {
    int (*set_attributes)(int fd, int action, const struct termios *attributes);
    int (*set_custom_speed)(int fd, unsigned long request, void *speed);
} h2_posix_serial_host_darwin_ops_t;

h2_pal_result_t h2_posix_serial_host_apply_darwin_custom_speed(
    int fd,
    uint32_t baud_rate,
    const struct termios *original_attributes,
    const h2_posix_serial_host_darwin_ops_t *ops);
#endif

struct h2_pal_serial_host_snapshot {
    h2_pal_serial_host_port_info_t *ports;
    size_t count;
};

h2_pal_result_t h2_posix_serial_host_scan_native(
    h2_pal_serial_host_snapshot_t **out_snapshot);

const h2_pal_serial_host_api_t *h2_posix_serial_host_api(void);

#endif
