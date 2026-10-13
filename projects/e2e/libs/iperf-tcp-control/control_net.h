#ifndef H2_IPERF_E2E_CONTROL_NET_H
#define H2_IPERF_E2E_CONTROL_NET_H
#include "h2/pal/net/h2_pal_net.h"
/* Native POSIX/lwIP iperf fixture only: one borrowed base API per image.
 * The first live opened/accepted TCP socket is the sequential control
 * session. Data sockets keep their original options. Closing control resets
 * the identity for the next session; the Runtime and PAL ABI are unchanged. */
const h2_pal_net_api_t *h2_iperf_e2e_control_net(const h2_pal_net_api_t *base);
#endif
