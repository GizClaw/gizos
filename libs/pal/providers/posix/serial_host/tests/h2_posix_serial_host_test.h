#ifndef H2_POSIX_SERIAL_HOST_TEST_H
#define H2_POSIX_SERIAL_HOST_TEST_H

#include "h2/pal/os/h2_pal_serial_host.h"

#ifdef __cplusplus
extern "C" {
#endif

int h2_posix_serial_host_run_tests(
    const h2_pal_serial_host_api_t *serial_host);

#ifdef __cplusplus
}
#endif

#endif
