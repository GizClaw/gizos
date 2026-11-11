#include "h2_g99_wifi.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #condition); \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

static h2_pal_wifi_scan_entry_t entry(const char* ssid, int rssi) {
  h2_pal_wifi_scan_entry_t e;
  memset(&e, 0, sizeof(e));
  e.ssid_len = strlen(ssid);
  memcpy(e.ssid, ssid, e.ssid_len);
  e.rssi = rssi;
  e.security = H2_PAL_WIFI_SECURITY_WPA2;
  return e;
}

static void test_merge_sorts_and_deduplicates(void) {
  h2_g99_wifi_snapshot_t s;
  memset(&s, 0, sizeof(s));
  h2_pal_wifi_scan_entry_t e = entry("Office", -60);
  h2_g99_wifi_merge(&s, &e);
  e = entry("Home", -40);
  h2_g99_wifi_merge(&s, &e);
  e = entry("", -20);
  h2_g99_wifi_merge(&s, &e);
  e = entry("Cafe", -80);
  h2_g99_wifi_merge(&s, &e);
  CHECK(s.network_count == 3u);
  CHECK(strcmp(s.networks[0].ssid, "Home") == 0);
  CHECK(strcmp(s.networks[1].ssid, "Office") == 0);
  CHECK(strcmp(s.networks[2].ssid, "Cafe") == 0);
  /* A weaker duplicate is ignored; a stronger one moves forward. */
  e = entry("Office", -70);
  h2_g99_wifi_merge(&s, &e);
  CHECK(s.networks[1].rssi == -60);
  e = entry("Cafe", -30);
  h2_g99_wifi_merge(&s, &e);
  CHECK(s.network_count == 3u);
  CHECK(strcmp(s.networks[0].ssid, "Cafe") == 0);
  CHECK(s.networks[0].rssi == -30);
  CHECK(strcmp(s.networks[1].ssid, "Home") == 0);
  h2_g99_wifi_merge(&s, NULL);
  h2_g99_wifi_merge(NULL, &e);
  CHECK(s.network_count == 3u);
}

static void test_merge_bounded(void) {
  h2_g99_wifi_snapshot_t s;
  memset(&s, 0, sizeof(s));
  char name[8];
  for (int i = 0; i < (int)H2_G99_WIFI_MAX_NETWORKS; ++i) {
    (void)snprintf(name, sizeof(name), "n%d", i);
    h2_pal_wifi_scan_entry_t e = entry(name, -50 - i);
    h2_g99_wifi_merge(&s, &e);
  }
  CHECK(s.network_count == H2_G99_WIFI_MAX_NETWORKS);
  h2_pal_wifi_scan_entry_t weak = entry("weak", -95);
  h2_g99_wifi_merge(&s, &weak);
  CHECK(s.network_count == H2_G99_WIFI_MAX_NETWORKS);
  CHECK(strcmp(s.networks[H2_G99_WIFI_MAX_NETWORKS - 1u].ssid, "n11") == 0);
  h2_pal_wifi_scan_entry_t strong = entry("strong", -10);
  h2_g99_wifi_merge(&s, &strong);
  CHECK(s.network_count == H2_G99_WIFI_MAX_NETWORKS);
  CHECK(strcmp(s.networks[0].ssid, "strong") == 0);
  CHECK(strcmp(s.networks[H2_G99_WIFI_MAX_NETWORKS - 1u].ssid, "n10") == 0);
  for (uint8_t i = 1u; i < s.network_count; ++i)
    CHECK(s.networks[i - 1u].rssi >= s.networks[i].rssi);
}

static void test_bars(void) {
  CHECK(h2_g99_wifi_bars(-40) == 4u);
  CHECK(h2_g99_wifi_bars(-55) == 4u);
  CHECK(h2_g99_wifi_bars(-60) == 3u);
  CHECK(h2_g99_wifi_bars(-70) == 2u);
  CHECK(h2_g99_wifi_bars(-80) == 1u);
  CHECK(h2_g99_wifi_bars(-90) == 0u);
}

int main(void) {
  test_merge_sorts_and_deduplicates();
  test_merge_bounded();
  test_bars();
  if (failures != 0) {
    fprintf(stderr, "%d wifi check(s) failed\n", failures);
    return 1;
  }
  return 0;
}
