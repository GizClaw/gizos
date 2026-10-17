#ifndef H2_GIZCLAW_E2E_DEVKIT_CAPTURE_H
#define H2_GIZCLAW_E2E_DEVKIT_CAPTURE_H

#include "ledger.h"
#include "result_console.h"
#include "h2/pal/os/h2_pal_crypto.h"

static inline bool h2_gizclaw_capture_literal(const char **at,
    const char *end, const char *literal) {
  const size_t size = strlen(literal);
  if ((size_t)(end - *at) < size || memcmp(*at, literal, size)) return false;
  *at += size;
  return true;
}
static inline bool h2_gizclaw_capture_hex(const char **at,
    const char *end, size_t size) {
  if ((size_t)(end - *at) < size) return false;
  for (size_t i = 0u; i < size; ++i)
    if (!((*at)[i] >= '0' && (*at)[i] <= '9') &&
        !((*at)[i] >= 'a' && (*at)[i] <= 'f')) return false;
  *at += size;
  return true;
}
static inline bool h2_gizclaw_capture_number(const char **at,
    const char *end, uint64_t maximum) {
  if (*at == end || **at < '1' || **at > '9') return false;
  uint64_t number = 0u;
  while (*at < end && **at >= '0' && **at <= '9') {
    const uint64_t digit = (uint64_t)(**at - '0');
    if (number > (maximum - digit) / 10u) return false;
    number = number * 10u + digit;
    ++*at;
  }
  return number <= maximum;
}

/* Borrowed crypto; initialize this boot's zeroed storage once, before Wi-Fi.
 * The later BOOT and both independent host receipts reuse these actual bytes. */
static inline int h2_gizclaw_e2e_devkit_execution(
    const h2_pal_crypto_api_t *crypto, char execution[33]) {
  if (execution == NULL || execution[0] != '\0') return H2_PAL_ERR_INVALID_STATE;
  uint8_t nonce[16];
  int rc = h2_pal_crypto_random(crypto, nonce, sizeof(nonce));
  if (rc != H2_PAL_OK) return rc;
  uint8_t any = 0u;
  for (size_t i = 0u; i < sizeof(nonce); ++i) any |= nonce[i];
  if (any == 0u) return H2_PAL_ERR_INVALID_STATE;
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0u; i < sizeof(nonce); ++i) {
    execution[i * 2u] = hex[nonce[i] >> 4u];
    execution[i * 2u + 1u] = hex[nonce[i] & 15u];
  }
  execution[32] = '\0';
  return H2_PAL_OK;
}

/* The managed reboot's command transport must be available before its unique
 * first marker. This is startup handoff, outside every App case/network budget. */
static inline int h2_gizclaw_e2e_devkit_prepare_execution(
    const h2_pal_time_api_t *time, const h2_pal_crypto_api_t *crypto,
    char execution[33]) {
  int rc = h2_pal_time_sleep_ms(time, 5000u);
  return rc == H2_PAL_OK ? h2_gizclaw_e2e_devkit_execution(crypto, execution) : rc;
}

/* Caller serializes with the evidence mutex. Only this public, secret-free
 * readiness shape is streamed; all records retain their original ledger bytes.
 * An invalid ready or failed write/drain poisons the ledger's admission gate. */
static inline int h2_gizclaw_e2e_devkit_capture(
    h2_gizclaw_e2e_ledger_t *ledger, h2_esp_e2e_console_t *console,
    const char *record, size_t size) {
  if (ledger == NULL) return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_e2e_ledger_append(ledger, record, size);
  if (ledger->error != H2_PAL_OK) return ledger->error;
  static const char prefix[] = "H2_GIZCLAW_E2E stage=ble-binding-ready ";
  if (size < sizeof(prefix) - 1u || memcmp(record, prefix, sizeof(prefix) - 1u))
    return H2_PAL_OK;
  const char *at = record + sizeof(prefix) - 1u, *end = record + size - 1u;
  char line[256];
  if (size >= sizeof(line) ||
      !h2_gizclaw_capture_literal(&at, end, "window=h2e2e-") ||
      !h2_gizclaw_capture_hex(&at, end, 16u) ||
      !h2_gizclaw_capture_literal(&at, end, " revision=") ||
      !h2_gizclaw_capture_number(&at, end, UINT64_MAX) ||
      !h2_gizclaw_capture_literal(&at, end, " url_bytes=") ||
      !h2_gizclaw_capture_number(&at, end, 1024u) ||
      !h2_gizclaw_capture_literal(&at, end, " url_sha256=") ||
      !h2_gizclaw_capture_hex(&at, end, 64u) || at != end)
    return ledger->error = H2_PAL_ERR_FORMAT;
  memcpy(line, record, size);
  line[size] = '\0';
  int rc = h2_esp_e2e_console_record(console, line);
  if (rc != H2_PAL_OK) ledger->error = rc;
  return rc;
}
#endif
