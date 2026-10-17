#ifndef H2_GIZCLAW_E2E_BLE_PEER_TRANSFER_H
#define H2_GIZCLAW_E2E_BLE_PEER_TRANSFER_H

#include "h2/pal/hal/h2_pal_ble.h"

typedef struct h2_gizclaw_e2e_ble_peer_expected {
  uint64_t revision;
  size_t url_bytes;
  const char *url_sha256;
} h2_gizclaw_e2e_ble_peer_expected_t;

/* Discover and read the real protocol over one already-connected PAL link.
 * No credential bytes escape this synchronous call or enter diagnostics. */
int h2_gizclaw_e2e_ble_peer_transfer(
    const h2_pal_ble_host_api_t *ble, uint16_t connection,
    const h2_gizclaw_e2e_ble_peer_expected_t *expected, size_t *out_chunks);

#endif
