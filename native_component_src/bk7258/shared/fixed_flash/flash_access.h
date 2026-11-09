#ifndef H2_BK_FLASH_ACCESS_SHARED_H
#define H2_BK_FLASH_ACCESS_SHARED_H

#include <stddef.h>
#include <stdint.h>

static inline int h2_bk_flash_erased(const uint8_t packet[34]) {
  for (size_t i = 0; i < 34; ++i)
    if (packet[i] != 0xffu) return 0;
  return 1;
}

static inline int h2_bk_flash_crc_valid(const uint8_t packet[34]) {
  uint16_t crc = 0xffffu;
  for (size_t i = 0; i < 32; ++i) {
    crc ^= (uint16_t)packet[i] << 8;
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000u) ? 0x8005u : 0u));
  }
  return packet[32] == (uint8_t)(crc >> 8) && packet[33] == (uint8_t)crc;
}

static inline uint32_t h2_bk_flash_crc32(uint32_t crc, const uint8_t* data,
                                         size_t size) {
  crc = ~crc;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
  }
  return ~crc;
}

/* Implemented on CP; used before the AP starts. */
int h2_bk_flash_cp_vector(uint32_t physical, uint32_t vector[2]);

#endif
