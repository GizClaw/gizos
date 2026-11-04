#ifndef H2_JIELI_PENDING_BOOT_H
#define H2_JIELI_PENDING_BOOT_H

#include "h2/pal/hal/h2_pal_power.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Original pi32v2 Preference wire schema, encoded without C struct padding.
 * This envelope owns only H2's SHA binding. Header bytes remain opaque here. */
#define H2_JIELI_PENDING_BOOT_MAGIC UINT32_C(0x31425048)
#define H2_JIELI_PENDING_BOOT_WIRE_SIZE 112u
#define H2_JIELI_PENDING_BOOT_KEY "jieli_pending_boot"
#define H2_JIELI_PENDING_NATIVE_FORMAT 1u
#define H2_JIELI_PENDING_NATIVE_SIZE 40u

static inline int h2_jieli_pending_checksum_valid(const char* checksum) {
  if (checksum == NULL) return 0;
  for (size_t i = 0u; i < 64u; ++i) {
    const char c = checksum[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
  }
  return checksum[64] == '\0';
}

static inline int h2_jieli_pending_boot_encode(
    const h2_pal_power_native_evidence_t* evidence, const char* checksum,
    uint8_t out[H2_JIELI_PENDING_BOOT_WIRE_SIZE]) {
  if (evidence == NULL || out == NULL ||
      evidence->format != H2_JIELI_PENDING_NATIVE_FORMAT ||
      evidence->size != H2_JIELI_PENDING_NATIVE_SIZE ||
      evidence->bytes[6] != 0u || evidence->bytes[7] != 0u ||
      (evidence->bytes[0] | evidence->bytes[1] | evidence->bytes[2] |
       evidence->bytes[3]) == 0u ||
      !h2_jieli_pending_checksum_valid(checksum))
    return 0;
  memset(out, 0, H2_JIELI_PENDING_BOOT_WIRE_SIZE);
  for (unsigned i = 0u; i < 4u; ++i)
    out[i] = (uint8_t)(H2_JIELI_PENDING_BOOT_MAGIC >> (8u * i));
  memcpy(out + 4u, evidence->bytes, 8u);
  memcpy(out + 12u, checksum, 65u);
  memcpy(out + 77u, evidence->bytes + 8u, 32u);
  return 1;
}

static inline int h2_jieli_pending_boot_decode(
    const void* blob, size_t size, const char* expected_checksum,
    h2_pal_power_native_evidence_t* out_evidence) {
  if (out_evidence == NULL) return 0;
  memset(out_evidence, 0, sizeof(*out_evidence));
  if (blob == NULL || size != H2_JIELI_PENDING_BOOT_WIRE_SIZE ||
      !h2_jieli_pending_checksum_valid(expected_checksum))
    return 0;
  const uint8_t* bytes = blob;
  for (unsigned i = 0u; i < 4u; ++i)
    if (bytes[i] != (uint8_t)(H2_JIELI_PENDING_BOOT_MAGIC >> (8u * i)))
      return 0;
  if (bytes[10] != 0u || bytes[11] != 0u || bytes[109] != 0u ||
      bytes[110] != 0u || bytes[111] != 0u ||
      (bytes[4] | bytes[5] | bytes[6] | bytes[7]) == 0u ||
      memcmp(bytes + 12u, expected_checksum, 65u) != 0)
    return 0;
  out_evidence->format = H2_JIELI_PENDING_NATIVE_FORMAT;
  out_evidence->size = H2_JIELI_PENDING_NATIVE_SIZE;
  memcpy(out_evidence->bytes, bytes + 4u, 8u);
  memcpy(out_evidence->bytes + 8u, bytes + 77u, 32u);
  return 1;
}

#endif
