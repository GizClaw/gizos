#ifndef H2_LINUX_SERIAL_HOST_H
#define H2_LINUX_SERIAL_HOST_H

#include "h2/pal/os/h2_pal_serial_host.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Return the Linux Host Serial provider. */
const h2_pal_serial_host_api_t *h2_linux_serial_host_api(void);

#ifdef __cplusplus
}
#endif

#endif
