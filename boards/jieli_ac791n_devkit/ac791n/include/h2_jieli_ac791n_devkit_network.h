#ifndef H2_JIELI_AC791N_DEVKIT_NETWORK_H
#define H2_JIELI_AC791N_DEVKIT_NETWORK_H

#include "h2/pal/net/h2_pal_netif.h"

/* Internal ownership boundary shared by the Wi-Fi and netif providers.
 * begin reserves native radio use; end releases it even on snapshot failure.
 * capture requires that reservation or the SDK's serialized HSM context. */
int h2_jieli_wifi_netif_begin(h2_pal_netif_status_t *status, uint32_t *generation);
int h2_jieli_wifi_netif_end(uint32_t generation);
/* Wi-Fi owns these transitions: started after wifi_on, stopping before
 * wifi_off (drains native users), stopped after wifi_off (settles DNS).
 * Callers must close all sockets and resolvers before disconnect/ap_stop. */
void h2_jieli_net_stack_started(void);
void h2_jieli_net_stack_stopping(void);
void h2_jieli_net_stack_stopped(void);
int h2_jieli_netif_capture_ip(h2_pal_netif_status_t *status);

#endif
