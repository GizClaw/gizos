#ifndef H2_GIZCLAW_E2E_BLE_PEER_RECEIPT_H
#define H2_GIZCLAW_E2E_BLE_PEER_RECEIPT_H

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

enum { H2_GIZCLAW_E2E_BLE_PEER_TIMEOUT_MS = 120000u };

typedef struct h2_gizclaw_e2e_ble_peer_receipt {
  const char *version, *execution, *window, *url_sha256;
  const char *started_utc, *finished_utc;
  uint64_t revision, url_bytes, elapsed_ms;
  unsigned completed_connections;
  size_t chunks[2];
  int rc;
  bool qualified;
} h2_gizclaw_e2e_ble_peer_receipt_t;

/* All text is validated by the CLI producer before opening a BLE Host. This
 * serializer is shared by the real producer and its compiled host test. */
static inline int h2_gizclaw_e2e_ble_peer_write_receipt(
    FILE *output, const h2_gizclaw_e2e_ble_peer_receipt_t *value) {
  return fprintf(output,
      "{\"schema\":2,\"backend\":\"darwin-pal\",\"version\":\"%s\","
      "\"execution\":\"%s\",\"window\":\"%s\",\"revision\":%" PRIu64 ","
      "\"url_bytes\":%" PRIu64 ",\"url_sha256\":\"%s\","
      "\"completed_connections\":%u,\"chunks\":[%zu,%zu],\"rediscovered\":%s,"
      "\"started_at_utc\":\"%s\",\"finished_at_utc\":\"%s\","
      "\"timeout_ms\":%u,\"elapsed_ms\":%" PRIu64 ",\"rc\":%d,\"qualified\":%s}\n",
      value->version, value->execution, value->window, value->revision,
      value->url_bytes, value->url_sha256, value->completed_connections,
      value->chunks[0], value->chunks[1],
      value->completed_connections == 2u ? "true" : "false",
      value->started_utc, value->finished_utc, (unsigned)H2_GIZCLAW_E2E_BLE_PEER_TIMEOUT_MS,
      value->elapsed_ms, value->rc, value->qualified ? "true" : "false");
}

#endif
