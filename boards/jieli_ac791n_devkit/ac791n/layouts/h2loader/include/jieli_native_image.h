#ifndef H2_JIELI_NATIVE_IMAGE_H
#define H2_JIELI_NATIVE_IMAGE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
  uint32_t code_offset;
  uint32_t code_length;
  uint16_t code_crc;
} h2_jieli_native_image_t;

static inline uint16_t h2_jieli_native_u16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t h2_jieli_native_u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Pinned WL82 SDK jl_fs_crc16: CRC-CCITT, seed zero, over header bytes 2..31. */
static inline uint16_t h2_jieli_native_header_crc(const uint8_t *p) {
  uint16_t crc = 0u;
  for (size_t i = 2u; i < 32u; ++i) {
    crc ^= (uint16_t)((uint16_t)p[i] << 8);
    for (unsigned bit = 0u; bit < 8u; ++bit) {
      crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000u) ? 0x1021u : 0u));
    }
  }
  return crc;
}

/* Parse only the bounded UFW directory, not executable bytes. Mirrors the
 * pinned SDK update_data/app_core probe; payload CRC is verified separately
 * by the SDK writer. Never infer codeLength from the outer package size. */
static inline int h2_jieli_native_image_parse(
    const uint8_t *directory, size_t available, uint32_t image_size,
    h2_jieli_native_image_t *out) {
  if (directory == NULL || out == NULL || available < 32u) return -1;
  if (h2_jieli_native_u16(directory) != h2_jieli_native_header_crc(directory) ||
      memcmp(directory + 16u, "update_data", 12u) != 0 ||
      h2_jieli_native_u32(directory + 8u) != image_size ||
      h2_jieli_native_u32(directory + 4u) != 32u) return -1;
  const uint16_t count = h2_jieli_native_u16(directory + 14u);
  if (count == 0u || count > 7u || available < 32u * (count + 1u)) return -1;
  const uint8_t *code = directory + 32u * count;
  if (h2_jieli_native_u16(code) != h2_jieli_native_header_crc(code) ||
      memcmp(code + 16u, "app_core", 9u) != 0) return -1;
  const uint32_t offset = h2_jieli_native_u32(code + 4u);
  const uint32_t length = h2_jieli_native_u32(code + 8u);
  if (offset < 32u * (count + 1u) || offset > image_size ||
      length == 0u || length > image_size - offset) return -1;
  out->code_offset = offset;
  out->code_length = length;
  out->code_crc = h2_jieli_native_u16(code + 2u);
  return 0;
}

#endif
