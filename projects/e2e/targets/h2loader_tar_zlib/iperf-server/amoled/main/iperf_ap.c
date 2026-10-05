#include "iperf_ap.h"

#include "esp_netif_net_stack.h"
#include "lwip/ip6_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"
#include "lwip/timeouts.h"
#include <stdio.h>
#include <string.h>

/* On-link ULA only: router lifetime zero provides no Internet/default route. */
static void advertise(void *user) {
  h2_iperf_amoled_ap_t *state = user;
  struct netif *netif = esp_netif_get_netif_impl(state->ap);
  uint8_t packet[56] = {134, 0, 0, 0, 64};
  packet[16] = 1;
  packet[17] = 1;
  memcpy(packet + 18, netif->hwaddr, 6);
  packet[24] = 3;
  packet[25] = 4;
  packet[26] = 64;
  packet[27] = 0xc0;
  packet[30] = 0x07;
  packet[31] = 0x08; /* valid 1800 s */
  packet[34] = 0x02;
  packet[35] = 0x58; /* preferred 600 s */
  ip6_addr_t prefix;
  ip6addr_aton("fd53:697a:6f73:626::", &prefix);
  memcpy(packet + 40, prefix.addr, 16);
  struct pbuf *buffer = pbuf_alloc(PBUF_IP, sizeof(packet), PBUF_RAM);
  if (buffer != NULL) {
    ip_addr_t destination, source;
    IP_SET_TYPE_VAL(destination, IPADDR_TYPE_V6);
    IP_SET_TYPE_VAL(source, IPADDR_TYPE_V6);
    ip6addr_aton("ff02::1", ip_2_ip6(&destination));
    ip6_addr_copy(*ip_2_ip6(&source), *netif_ip6_addr(netif, 0));
    ip6_addr_assign_zone(ip_2_ip6(&destination), IP6_MULTICAST, netif);
    if (pbuf_take(buffer, packet, sizeof(packet)) == ERR_OK)
      (void)raw_sendto_if_src(state->advertiser, buffer, &destination, netif,
                              &source);
    pbuf_free(buffer);
  }
  sys_timeout(3000, advertise, state);
}
static void start_advertiser(void *user) {
  h2_iperf_amoled_ap_t *state = user;
  struct netif *netif = esp_netif_get_netif_impl(state->ap);
  state->advertiser = raw_new_ip_type(IPADDR_TYPE_V6, IP6_NEXTH_ICMP6);
  if (state->advertiser == NULL) {
    state->callback_result = H2_PAL_ERR_NO_MEMORY;
    return;
  }
  raw_bind_netif(state->advertiser, netif);
  state->advertiser->ttl = 255;
  raw_set_multicast_ttl(state->advertiser, 255);
  state->advertiser->chksum_reqd = 1;
  state->advertiser->chksum_offset = 2;
  advertise(state);
  state->callback_result = H2_PAL_OK;
}
static void stop_advertiser(void *user) {
  h2_iperf_amoled_ap_t *state = user;
  sys_untimeout(advertise, state);
  if (state->advertiser != NULL) {
    raw_remove(state->advertiser);
    state->advertiser = NULL;
  }
}
int h2_iperf_amoled_ap_stop(void *user) {
  h2_iperf_amoled_ap_t *state = user;
  if (state == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  /* Wait for timer/PCB removal before the Wi-Fi provider destroys its netif. */
  if (state->advertiser != NULL &&
      tcpip_callback_wait(stop_advertiser, state) != ERR_OK)
    return H2_PAL_ERR_IO;
  int rc = h2_pal_wifi_ap_stop(state->runtime->wifi_ap, 5000u);
  if (rc == H2_PAL_OK) {
    state->ap = NULL;
    state->active = false;
  }
  return rc;
}
int h2_iperf_amoled_ap_start(void *user, h2_iperf_server_app_mode_t mode,
                             h2_iperf_server_app_network_t *out_network) {
  h2_iperf_amoled_ap_t *state = user;
  if (state == NULL || out_network == NULL || state->active ||
      (mode != H2_IPERF_SERVER_APP_MODE_IPV4 &&
       mode != H2_IPERF_SERVER_APP_MODE_IPV6 &&
       mode != H2_IPERF_SERVER_APP_MODE_DUAL))
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_network, 0, sizeof(*out_network));
  const h2_pal_wifi_ap_config_t config = {
      .ssid = H2_IPERF_SERVER_APP_SSID,
      .ssid_len = sizeof(H2_IPERF_SERVER_APP_SSID) - 1,
      .password = H2_IPERF_SERVER_APP_PASSWORD,
      .password_len = sizeof(H2_IPERF_SERVER_APP_PASSWORD) - 1,
      .channel = 6,
      .max_clients = 4,
      .security = H2_PAL_WIFI_SECURITY_WPA2};
  int rc = h2_pal_wifi_sta_disconnect(state->runtime->wifi_sta);
  if (rc != H2_PAL_OK)
    return rc;
  rc = h2_pal_wifi_ap_start(state->runtime->wifi_ap, &config, 5000u);
  if (rc != H2_PAL_OK)
    return rc;
  state->active = true;
  state->ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (state->ap == NULL) {
    rc = H2_PAL_ERR_UNAVAILABLE;
    goto failed;
  }
  if (mode == H2_IPERF_SERVER_APP_MODE_IPV6) {
    esp_err_t error = esp_netif_dhcps_stop(state->ap);
    if (error != ESP_OK && error != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
      rc = H2_PAL_ERR_IO;
      goto failed;
    }
    const esp_netif_ip_info_t no_ipv4 = {0};
    if (esp_netif_set_ip_info(state->ap, &no_ipv4) != ESP_OK) {
      rc = H2_PAL_ERR_IO;
      goto failed;
    }
  } else {
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(state->ap, &info) != ESP_OK ||
        info.ip.addr == 0) {
      rc = H2_PAL_ERR_IO;
      goto failed;
    }
    out_network->ipv4.family = H2_PAL_NET_FAMILY_IPV4;
    memcpy(out_network->ipv4.ip, &info.ip.addr, 4);
    (void)snprintf(out_network->ipv4_text, sizeof(out_network->ipv4_text),
                   IPSTR, IP2STR(&info.ip));
  }
  if (mode != H2_IPERF_SERVER_APP_MODE_IPV4) {
    if (esp_netif_create_ip6_linklocal(state->ap) != ESP_OK) {
      rc = H2_PAL_ERR_IO;
      goto failed;
    }
    esp_ip6_addr_t address = {0};
    ip6_addr_t parsed;
    if (!ip6addr_aton(H2_IPERF_SERVER_APP_IPV6_TEXT, &parsed)) {
      rc = H2_PAL_ERR_FORMAT;
      goto failed;
    }
    memcpy(address.addr, parsed.addr, sizeof(address.addr));
    if (esp_netif_add_ip6_address(state->ap, address, true) != ESP_OK) {
      rc = H2_PAL_ERR_IO;
      goto failed;
    }
    bool ready = false;
    for (unsigned i = 0; i < 50; ++i) {
      esp_ip6_addr_t local;
      if (esp_netif_get_ip6_linklocal(state->ap, &local) == ESP_OK) {
        ready = true;
        break;
      }
      (void)h2_pal_time_sleep_ms(state->runtime->time, 100u);
    }
    if (!ready) {
      rc = H2_PAL_ERR_TIMEOUT;
      goto failed;
    }
    if (tcpip_callback_wait(start_advertiser, state) != ERR_OK) {
      rc = H2_PAL_ERR_IO;
      goto failed;
    }
    rc = state->callback_result;
    if (rc != H2_PAL_OK)
      goto failed;
    out_network->ipv6.family = H2_PAL_NET_FAMILY_IPV6;
    memcpy(out_network->ipv6.ip, parsed.addr, 16);
    memcpy(out_network->ipv6_text, H2_IPERF_SERVER_APP_IPV6_TEXT,
           sizeof(H2_IPERF_SERVER_APP_IPV6_TEXT));
  }
  return H2_PAL_OK;
failed:
  /* Cleanup must complete before the manager can retry another mode. */
  while (h2_iperf_amoled_ap_stop(state) != H2_PAL_OK)
    (void)h2_pal_time_sleep_ms(state->runtime->time, 100u);
  memset(out_network, 0, sizeof(*out_network));
  return rc;
}
