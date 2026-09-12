#ifndef H2_JIELI_PENDING_BOOT_H
#define H2_JIELI_PENDING_BOOT_H

#include "jieli_native_image.h"
#include "jieli_upgrade_io.h"
#include "h2_jieli_ac791n_devkit_partitions.h"

/* Versioned, single-blob Pref record. This is not a native BootInfo ABI. */
typedef struct {
  uint32_t magic;
  uint32_t code_length;
  uint16_t code_crc;
  uint16_t reserved;
  char image_checksum[65];
  uint8_t header[H2_JIELI_UPGRADE_HEADER_SIZE];
} h2_jieli_pending_boot_t;

#define H2_JIELI_PENDING_BOOT_MAGIC UINT32_C(0x31425048)
/* Preserve the original pi32v2 record bytes, without serializing C padding.
 * Integers are little-endian; bytes 109..111 are reserved and must be zero. */
#define H2_JIELI_PENDING_BOOT_WIRE_SIZE 112u

static inline void h2_jieli_pending_boot_encode(
    const h2_jieli_pending_boot_t *record,
    uint8_t out[H2_JIELI_PENDING_BOOT_WIRE_SIZE]) {
  memset(out, 0, H2_JIELI_PENDING_BOOT_WIRE_SIZE);
  for (unsigned i = 0; i < 4u; ++i) {
    out[i] = (uint8_t)(record->magic >> (8u * i));
    out[4u + i] = (uint8_t)(record->code_length >> (8u * i));
  }
  out[8] = (uint8_t)record->code_crc;
  out[9] = (uint8_t)(record->code_crc >> 8u);
  out[10] = (uint8_t)record->reserved;
  out[11] = (uint8_t)(record->reserved >> 8u);
  memcpy(out + 12u, record->image_checksum, 65u);
  memcpy(out + 77u, record->header, H2_JIELI_UPGRADE_HEADER_SIZE);
}

static inline int h2_jieli_pending_boot_decode(
    h2_jieli_pending_boot_t *record, const void *blob, size_t size) {
  if (record == NULL || blob == NULL ||
      size != H2_JIELI_PENDING_BOOT_WIRE_SIZE) return 0;
  const uint8_t *bytes = blob;
  if (bytes[109] != 0u || bytes[110] != 0u || bytes[111] != 0u) return 0;
  memset(record, 0, sizeof(*record));
  record->magic = h2_jieli_native_u32(bytes);
  record->code_length = h2_jieli_native_u32(bytes + 4u);
  record->code_crc = h2_jieli_native_u16(bytes + 8u);
  record->reserved = h2_jieli_native_u16(bytes + 10u);
  memcpy(record->image_checksum, bytes + 12u, 65u);
  memcpy(record->header, bytes + 77u, H2_JIELI_UPGRADE_HEADER_SIZE);
  return 1;
}

static inline int h2_jieli_pending_boot_matches(
    const h2_jieli_pending_boot_t *record, size_t size, const char *checksum) {
  if (record == NULL || checksum == NULL || size != sizeof(*record) ||
      record->magic != H2_JIELI_PENDING_BOOT_MAGIC || record->reserved != 0u ||
      record->code_length == 0u || record->code_length > H2_JIELI_IMAGE_MAX_SIZE ||
      record->image_checksum[64] != '\0' || strlen(checksum) != 64u) return 0;
  for (unsigned i = 0; i < 64u; ++i) {
    char c = record->image_checksum[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
  }
  return memcmp(record->image_checksum, checksum, 65u) == 0;
}

/* Plaintext produced by the SDK's key-FFFF 32-byte header decoder. */
static inline int h2_jieli_pending_boot_header_valid(
    const h2_jieli_pending_boot_t *record, const uint8_t plain[32]) {
  return plain != NULL && record != NULL &&
      h2_jieli_native_u16(plain) == h2_jieli_native_header_crc(plain) &&
      h2_jieli_native_u16(plain + 4u) == record->code_crc &&
      h2_jieli_native_u32(plain + 8u) == record->code_length;
}

#endif
