#pragma once
#include <stdint.h>
typedef int flash_protect_type_t;
#define FLASH_PROTECT_NONE 0
static inline int bk_flash_read_bytes(uint32_t offset, void *data,
                                      uint32_t size) {
  (void)offset;
  (void)data;
  (void)size;
  return 0;
}
static inline flash_protect_type_t bk_flash_get_protect_type(void) { return 0; }
static inline int bk_flash_set_protect_type(flash_protect_type_t type) {
  (void)type;
  return 0;
}
static inline int bk_flash_erase_sector(uint32_t offset) {
  (void)offset;
  return 0;
}
