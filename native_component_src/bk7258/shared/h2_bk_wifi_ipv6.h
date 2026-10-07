#ifndef H2_BK_WIFI_IPV6_H
#define H2_BK_WIFI_IPV6_H

#include <stdint.h>

/* Separate opcode and fixed-width layout leave old P1 Wi-Fi RPCs intact. */
#define H2_BK_WIFI_IPV6_VERSION 1u
#define H2_BK_WIFI_IPV6_MAX 3u

typedef struct h2_bk_wifi_ipv6_address {
  uint8_t address[16];
  uint32_t preferred;
} h2_bk_wifi_ipv6_address_t;

typedef struct h2_bk_wifi_ipv6_snapshot {
  uint32_t version;
  uint32_t size;
  uint32_t generation;
  uint32_t connected;
  uint32_t count;
  uint32_t ssid_len;
  uint32_t channel;
  int32_t rssi;
  uint8_t ssid[32];
  uint8_t bssid[6];
  uint8_t reserved[2];
  h2_bk_wifi_ipv6_address_t addresses[H2_BK_WIFI_IPV6_MAX];
} h2_bk_wifi_ipv6_snapshot_t;

/* CP copies a coherent current-association snapshot. Caller provides size. */
int h2_bk_wifi_ipv6_snapshot(h2_bk_wifi_ipv6_snapshot_t *out);
/* Preserve a working IPv6 association when DHCPv4 times out. */
int h2_bk_wifi_ipv6_ready(void);

#endif
