#ifndef H2_POSIX_SERIAL_HOST_INTERNAL_H
#define H2_POSIX_SERIAL_HOST_INTERNAL_H

#include "h2/pal/os/h2_pal_serial_host.h"

struct h2_pal_serial_host_snapshot {
    h2_pal_serial_host_port_info_t *ports;
    size_t count;
};

h2_pal_result_t h2_posix_serial_host_scan_native(
    h2_pal_serial_host_snapshot_t **out_snapshot);

const h2_pal_serial_host_api_t *h2_posix_serial_host_api(void);

#endif
