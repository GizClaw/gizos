#ifndef H2_PAL_WEBRTC_NETWORK_H
#define H2_PAL_WEBRTC_NETWORK_H
#include "h2_runtime.h"

/* Reuse an active IP connection or borrow the saved STA profile. */
int h2_pal_webrtc_device_prepare_network(h2_runtime_t *runtime);

#endif
