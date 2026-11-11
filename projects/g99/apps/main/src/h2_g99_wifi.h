#ifndef H2_G99_WIFI_H
#define H2_G99_WIFI_H

/* Wi-Fi settings worker. Scans and connections block in PAL, so they run on
 * one Main-owned task; the UI reads copied snapshots and never waits. */

#include "hal/h2_pal_wifi.h"
#include "h2_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_G99_WIFI_MAX_NETWORKS 12u
#define H2_G99_WIFI_CONNECT_TIMEOUT_MS 20000u

typedef enum h2_g99_wifi_phase {
  H2_G99_WIFI_UNAVAILABLE = 0,
  H2_G99_WIFI_IDLE,
  H2_G99_WIFI_SCANNING,
  H2_G99_WIFI_CONNECTING,
} h2_g99_wifi_phase_t;

typedef struct h2_g99_wifi_network {
  char ssid[H2_PAL_WIFI_SSID_MAX + 1];
  int rssi;
  h2_pal_wifi_security_t security;
} h2_g99_wifi_network_t;

typedef struct h2_g99_wifi_snapshot {
  uint32_t version;
  h2_g99_wifi_phase_t phase;
  bool connected;
  char connected_ssid[H2_PAL_WIFI_SSID_MAX + 1];
  /** Result of the last finished scan or connect; OK before any. */
  h2_pal_result_t last_result;
  /** True when the last finished operation was a connect attempt. */
  bool last_was_connect;
  /** Counts finished connect attempts, so a fast attempt is never missed. */
  uint32_t connects_done;
  h2_g99_wifi_network_t networks[H2_G99_WIFI_MAX_NETWORKS];
  uint8_t network_count;
} h2_g99_wifi_snapshot_t;

typedef struct h2_g99_wifi h2_g99_wifi_t;

/** Starts the worker. A Runtime without Wi-Fi STA yields a worker whose
 * snapshot reports UNAVAILABLE. The worker first tries saved networks. */
h2_pal_result_t h2_g99_wifi_create(h2_runtime_t* runtime,
                                   h2_g99_wifi_t** out_wifi);

/** Joins the worker after its current PAL call returns. NULL succeeds. */
h2_pal_result_t h2_g99_wifi_destroy(h2_g99_wifi_t* wifi);

h2_pal_result_t h2_g99_wifi_request_scan(h2_g99_wifi_t* wifi);

/** Copies credentials; the password may be empty for open networks. */
h2_pal_result_t h2_g99_wifi_request_connect(h2_g99_wifi_t* wifi,
                                            const char* ssid,
                                            const char* password);

void h2_g99_wifi_get(h2_g99_wifi_t* wifi, h2_g99_wifi_snapshot_t* out);

/** Adds one scan entry: drops hidden SSIDs, keeps the strongest duplicate,
 * keeps the list sorted by RSSI and bounded. Exposed for tests. */
void h2_g99_wifi_merge(h2_g99_wifi_snapshot_t* snapshot,
                       const h2_pal_wifi_scan_entry_t* entry);

/** Signal bars 0..4 for an RSSI in dBm. */
uint8_t h2_g99_wifi_bars(int rssi);

#ifdef __cplusplus
}
#endif

#endif
