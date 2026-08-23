#ifndef H2_DESKTOP_APP_SUPPORT_C_H
#define H2_DESKTOP_APP_SUPPORT_C_H

#include "h2/pal/application/h2_pal_mqtt.h"
#include "h2/pal/application/h2_pal_webrtc.h"
#include "h2/pal/net/h2_pal_net.h"
#include "h2/pal/os/h2_pal_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_desktop_network_services h2_desktop_network_services_t;

h2_pal_result_t h2_desktop_network_services_create(
    int with_mqtt, int with_webrtc,
    h2_desktop_network_services_t **out_services);
void h2_desktop_network_services_destroy(
    h2_desktop_network_services_t *services);
const h2_pal_net_api_t *h2_desktop_host_net_api(void);
const h2_pal_crypto_api_t *h2_desktop_network_services_crypto(
    const h2_desktop_network_services_t *services);
const h2_pal_mqtt_api_t *h2_desktop_network_services_mqtt(
    const h2_desktop_network_services_t *services);
const h2_pal_webrtc_api_t *h2_desktop_network_services_webrtc(
    const h2_desktop_network_services_t *services);

#ifdef __cplusplus
}
#endif

#endif
