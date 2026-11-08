#include "flash_access.h"

#ifdef H2_BK_CP_FLASH_ACCESS_HOST_TEST
#include "flash_access_test_sdk.h"
#else
#include "driver/flash.h"
#include "os/os.h"
#include "armstar.h"
#endif
#include <string.h>

int h2_bk_flash_cp_vector(uint32_t physical, uint32_t vector[2]) {
  uint8_t packet[34];
  if (!vector || physical % 34u || physical > 0x1000000u - 34u) return -1;
  if (bk_flash_read_bytes(physical, packet, sizeof(packet)) != BK_OK ||
      h2_bk_flash_erased(packet) || !h2_bk_flash_crc_valid(packet))
    return -1;
  const uintptr_t address = 0x02000000u + physical / 34u * 32u;
  const uint32_t flags = rtos_disable_int();
#ifdef H2_BK_CP_FLASH_ACCESS_HOST_TEST
  h2_bk_test_cp_vector_copy(address, vector);
#else
  SCB_InvalidateDCache_by_Addr((void*)address, 32);
  __DSB();
  memcpy(vector, (const void*)address, 8);
#endif
  rtos_enable_int(flags);
  return 0;
}
