#include "ipv6_ap.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "lwip/icmp6.h"
#include "lwip/ip6_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/raw.h"
#include "lwip/tcpip.h"
#include "lwip/timeouts.h"
#include <stdatomic.h>
#include <string.h>

/* An isolated on-link /64. A zero router lifetime advertises no WAN route. */
static struct netif *ap_netif;
static struct raw_pcb *advertiser;
static atomic_int advertiser_result = H2_PAL_ERR_WOULD_BLOCK;
static atomic_int advertiser_enabled;
static int ap_owned;
static uint8_t prefix[16];
static void advertise(void *user) {
  (void)user;
  if (!advertiser_enabled || !advertiser || !ap_netif)
    return;
  uint8_t packet[56] = {134, 0, 0, 0, 64};
  packet[16] = 1; /* source link-layer address */
  packet[17] = 1;
  memcpy(packet + 18, ap_netif->hwaddr, 6);
  packet[24] = 3; /* prefix information */
  packet[25] = 4;
  packet[26] = 64;
  packet[27] = 0xc0; /* on-link + autonomous address configuration */
  packet[30] = 0x07;
  packet[31] = 0x08; /* valid 1800 seconds */
  packet[34] = 0x02;
  packet[35] = 0x58; /* preferred 600 seconds */
  memcpy(packet + 40, prefix, 16);
  struct pbuf *buffer = pbuf_alloc(PBUF_IP, sizeof(packet), PBUF_RAM);
  if (buffer) {
    ip_addr_t destination, source;
    IP_SET_TYPE_VAL(destination, IPADDR_TYPE_V6);
    IP_SET_TYPE_VAL(source, IPADDR_TYPE_V6);
    ip6addr_aton("ff02::1", ip_2_ip6(&destination));
    ip6_addr_copy(*ip_2_ip6(&source), *netif_ip6_addr(ap_netif, 0));
    ip6_addr_assign_zone(ip_2_ip6(&destination), IP6_MULTICAST, ap_netif);
    pbuf_take(buffer, packet, sizeof(packet));
    raw_sendto_if_src(advertiser, buffer, &destination, ap_netif, &source);
    pbuf_free(buffer);
  }
  /* Repeated advertisements also cover a station joining after AP startup. */
  if (advertiser_enabled)
    sys_timeout(6000, advertise, NULL);
}
static void start_advertiser(void *user) {
  (void)user;
  if (!advertiser_enabled || !ap_netif)
    return;
  ip6_addr_t subnet;
  ip6addr_aton("fd53:697a:6f73:626::", &subnet);
  memcpy(prefix, subnet.addr, sizeof(prefix));
  advertiser = raw_new_ip_type(IPADDR_TYPE_V6, IP6_NEXTH_ICMP6);
  if (!advertiser) {
    advertiser_result = H2_PAL_ERR_NO_MEMORY;
    return;
  }
  raw_bind_netif(advertiser, ap_netif);
  advertiser->ttl = 255;
  /* lwIP selects a separate hop limit for multicast packets. */
  raw_set_multicast_ttl(advertiser, 255);
  advertiser->chksum_reqd = 1;
  advertiser->chksum_offset = 2;
  advertise(NULL);
  advertiser_result = H2_PAL_OK;
}
static esp_err_t stop_advertiser(void *user) {
  (void)user;
  sys_untimeout(advertise, NULL);
  if (advertiser) {
    raw_remove(advertiser);
    advertiser = NULL;
  }
  ap_netif = NULL;
  return ESP_OK;
}
int h2_ipv6_ap_stop(h2_runtime_t *runtime) {
  if (!runtime)
    return H2_PAL_ERR_INVALID_ARG;
  advertiser_enabled = 0;
  if (!ap_owned)
    return H2_PAL_OK;
  /* This synchronous TCP/IP callback also fences a queued startup callback.
   * A late callback checks enabled and cannot revive the advertiser. */
  if (esp_netif_tcpip_exec(stop_advertiser, NULL) != ESP_OK)
    return H2_PAL_ERR_IO;
  int rc = h2_pal_wifi_ap_stop(runtime->wifi_ap, 10000);
  if (!rc)
    ap_owned = 0;
  return rc;
}
int h2_ipv6_ap_start(h2_runtime_t *runtime) {
  if (!runtime || !runtime->time)
    return H2_PAL_ERR_INVALID_ARG;
  if (ap_owned)
    return H2_PAL_ERR_INVALID_STATE;
  advertiser_result = H2_PAL_ERR_WOULD_BLOCK;
  advertiser_enabled = 0;
  h2_pal_wifi_ap_config_t config = {.ssid = "h2ipv6-amoled",
                                    .ssid_len = 13,
                                    .password = "palipv6e2e",
                                    .password_len = 10,
                                    .channel = 6,
                                    .max_clients = 1,
                                    .security = H2_PAL_WIFI_SECURITY_WPA2};
  int rc = h2_pal_wifi_sta_disconnect(runtime->wifi_sta);
  if (!rc)
    rc = h2_pal_wifi_ap_start(runtime->wifi_ap, &config, 10000);
  if (rc)
    return rc;
  ap_owned = 1;
  esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (!ap || esp_netif_create_ip6_linklocal(ap) != ESP_OK) {
    rc = H2_PAL_ERR_UNAVAILABLE;
    goto cleanup;
  }
  esp_ip6_addr_t address;
  if (!ip6addr_aton("fd53:697a:6f73:626::1", (ip6_addr_t *)&address) ||
      esp_netif_add_ip6_address(ap, address, true) != ESP_OK) {
    rc = H2_PAL_ERR_IO;
    goto cleanup;
  }
  ap_netif = esp_netif_get_netif_impl(ap);
  if (!ap_netif) {
    rc = H2_PAL_ERR_UNAVAILABLE;
    goto cleanup;
  }
  /* DAD must finish before the link-local source can advertise the prefix. */
  for (unsigned i = 0; i < 50; ++i) {
    esp_ip6_addr_t link_local;
    if (esp_netif_get_ip6_linklocal(ap, &link_local) == ESP_OK) {
      advertiser_enabled = 1;
      if (tcpip_callback(start_advertiser, NULL) != ERR_OK) {
        rc = H2_PAL_ERR_IO;
        goto cleanup;
      }
      for (unsigned j = 0;
           j < 100 && advertiser_result == H2_PAL_ERR_WOULD_BLOCK; ++j)
        h2_pal_time_sleep_ms(runtime->time, 10);
      rc = advertiser_result;
      if (rc == H2_PAL_ERR_WOULD_BLOCK)
        rc = H2_PAL_ERR_TIMEOUT;
      if (rc)
        goto cleanup;
      return H2_PAL_OK;
    }
    h2_pal_time_sleep_ms(runtime->time, 100);
  }
  rc = H2_PAL_ERR_TIMEOUT;
cleanup:
  {
    int cleaned = h2_ipv6_ap_stop(runtime);
    return cleaned ? cleaned : rc;
  }
}
