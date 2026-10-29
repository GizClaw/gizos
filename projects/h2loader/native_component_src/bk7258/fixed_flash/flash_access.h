#ifndef H2_BK_FLASH_ACCESS_SHARED_H
#define H2_BK_FLASH_ACCESS_SHARED_H

#include <stddef.h>
#include <stdint.h>

#define H2_BK_FLASH_STATUS_SERVER 4u
#define H2_BK_FLASH_STATUS_CLIENT 19u
#define H2_BK_FLASH_STATUS_COMMAND 1u
#define H2_BK_FLASH_STATUS_SIZE 16u
#define H2_BK_FLASH_AES_OFF 0u
#define H2_BK_FLASH_AES_ON 1u
#define H2_BK_FLASH_AES_UNKNOWN 2u

/* Private byte-encoded IPC, never a key or a caller-selected memory address. */
static inline void h2_bk_flash_status_request(uint8_t out[16]) {
  const uint8_t request[16] = {'H', '2', 'F', 'S', 1, 0, 0, 0,
                               1,   0,   0,   0,   2, 0, 0, 0};
  for (size_t i = 0; i < 16; ++i) out[i] = request[i];
}

static inline int h2_bk_flash_status_valid(const uint8_t data[16], int reply) {
  uint8_t request[16];
  h2_bk_flash_status_request(request);
  for (size_t i = 0; i < 16; ++i) {
    if (i == 12 && reply) continue;
    if (data[i] != request[i]) return 0;
  }
  return data[12] <= H2_BK_FLASH_AES_UNKNOWN;
}

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

/* Implemented on CP; used before the AP starts as well as by its query task. */
int h2_bk_flash_status_start(void);
int h2_bk_flash_cp_vector(uint32_t physical, uint32_t vector[2]);

#endif
