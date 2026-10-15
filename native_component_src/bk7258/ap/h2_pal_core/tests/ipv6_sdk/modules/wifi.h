#include "components/event.h"
#define WIFI_LINKSTATE_STA_CONNECTED 4
#define WIFI_LINKSTATE_STA_GOT_IP 5
#define WIFI_LINKSTATE_STA_DISCONNECTED 6
typedef struct {
  int state;
  char ssid[33];
  uint8_t bssid[6];
  unsigned channel;
  int rssi;
} wifi_link_status_t;
int bk_wifi_sta_get_link_status(wifi_link_status_t *);
