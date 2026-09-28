#ifndef H2_PAL_HTTP_NETWORK_H
#define H2_PAL_HTTP_NETWORK_H
#include "h2_runtime.h"
/* Keep an existing usable connection; otherwise borrow the saved STA profile. */
int h2_pal_http_device_prepare_network(h2_runtime_t *runtime);
#endif
