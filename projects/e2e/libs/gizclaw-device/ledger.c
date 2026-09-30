#include "ledger.h"
#include <string.h>

int h2_gizclaw_e2e_ledger_init(h2_gizclaw_e2e_ledger_t *ledger, char *buffer,
                              size_t capacity) {
  if (ledger == NULL || buffer == NULL || capacity == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  *ledger = (h2_gizclaw_e2e_ledger_t){.data = buffer, .capacity = capacity};
  return H2_PAL_OK;
}

void h2_gizclaw_e2e_ledger_append(h2_gizclaw_e2e_ledger_t *ledger,
                                 const char *record, size_t size) {
  if (ledger == NULL || ledger->error != H2_PAL_OK)
    return;
  static const char prefix[] = "H2_GIZCLAW_E2E ";
  if (ledger->frozen || ledger->data == NULL || record == NULL ||
      size < sizeof(prefix) || record[size - 1u] != '\n' ||
      memcmp(record, prefix, sizeof(prefix) - 1u) != 0 ||
      memchr(record, '\0', size) != NULL || memchr(record, '\n', size - 1u) != NULL) {
    ledger->error = H2_PAL_ERR_INVALID_STATE;
    return;
  }
  if (size > ledger->capacity - ledger->size) {
    ledger->error = H2_PAL_ERR_NO_SPACE;
    return;
  }
  memcpy(ledger->data + ledger->size, record, size);
  ledger->size += size;
  ledger->records++;
}

int h2_gizclaw_e2e_ledger_freeze(h2_gizclaw_e2e_ledger_t *ledger) {
  if (ledger == NULL || ledger->data == NULL || ledger->frozen)
    return H2_PAL_ERR_INVALID_STATE;
  if (ledger->error != H2_PAL_OK)
    return ledger->error;
  if (ledger->size == 0u)
    return H2_PAL_ERR_INVALID_STATE;
  /* IEEE CRC32 detects lost/truncated UART records; it is not an authenticity
   * claim. The host receipt additionally hashes the exact complete log. */
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0u; i < ledger->size; ++i) {
    crc ^= (uint8_t)ledger->data[i];
    for (unsigned bit = 0u; bit < 8u; ++bit)
      crc = (crc >> 1u) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  ledger->crc32 = ~crc;
  ledger->frozen = true;
  return H2_PAL_OK;
}
