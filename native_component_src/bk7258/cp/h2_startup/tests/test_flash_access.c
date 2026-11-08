#include "flash_access_test_sdk.h"
#include "flash_access.h"
#include <assert.h>
#include <string.h>

static unsigned flags, loads;
static uint8_t packet[34];

uint32_t rtos_disable_int(void) {
  assert(!flags);
  flags = 1;
  return 7;
}
void rtos_enable_int(uint32_t f) {
  assert(f == 7 && flags);
  flags = 0;
}
int bk_flash_read_bytes(uint32_t a, uint8_t* out, uint32_t n) {
  assert(a == 0x264000 && n == 34);
  memcpy(out, packet, n);
  return 0;
}
void h2_bk_test_cp_vector_copy(uintptr_t p, uint32_t* out) {
  assert(p == 0x2240000 && flags);
  loads++;
  memcpy(out, packet, 8);
}
int main(void) {
  assert(h2_bk_flash_crc32(0, (const uint8_t*)"123456789", 9) == 0xcbf43926);
  uint32_t vector[2];
  memset(packet, 255, sizeof(packet));
  assert(h2_bk_flash_cp_vector(0x264000, vector) < 0 && !loads);
  memset(packet, 0, 32);
  uint16_t crc = 0xffff;
  for (unsigned i = 0; i < 32; i++) {
    crc ^= (uint16_t)packet[i] << 8;
    for (unsigned b = 0; b < 8; b++)
      crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x8005 : 0));
  }
  packet[32] = (uint8_t)(crc >> 8);
  packet[33] = (uint8_t)crc;
  assert(h2_bk_flash_cp_vector(0x264000, vector) == 0 && loads == 1);
  packet[0] ^= 1;
  assert(h2_bk_flash_cp_vector(0x264000, vector) < 0 && loads == 1);
  return 0;
}
