#ifndef H2_ESP_WIFI_IP_H
#define H2_ESP_WIFI_IP_H

#include "esp_netif.h"
#include "h2/pal/hal/h2_pal_wifi.h"

/* Called after ESP-NETIF's association handler has brought the STA up. */
int h2_esp_wifi_ip_start(esp_netif_t *netif);
void h2_esp_wifi_ip_stop(esp_netif_t *netif);
void h2_esp_wifi_ip_snapshot(esp_netif_t *netif,
                             h2_pal_wifi_sta_status_t *status);

#endif
