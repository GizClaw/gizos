#include "h2_bk_flash_access.h"
#include "flash_access.h"

#ifdef H2_BK_FLASH_ACCESS_HOST_TEST
#include "h2_bk_flash_access_test_sdk.h"
#else
#include "driver/flash.h"
#include "core/h2_pal_errors.h"
#include "modules/ota.h"
#include "os/os.h"
#include "armstar.h"
#endif
#include <string.h>

int h2_bk_flash_access_ready(void) {
#if CONFIG_FLASH_CP_AP_DIRECT_ACCESS || CONFIG_CACHE_ENABLE
  return H2_PAL_ERR_UNSUPPORTED;
#else
  return bk_ota_get_current_partition() == 0u ? H2_PAL_OK
                                              : H2_PAL_ERR_UNSUPPORTED;
#endif
}

int h2_bk_flash_read_plain(uint32_t physical, uint32_t capacity,
                           uint32_t offset, void* output, size_t size) {
  uint8_t* dst = output;
  uint8_t packets[16u * 34u];
  const uint32_t logical_capacity = capacity / 34u * 32u;
  if ((!output && size) || physical % 34u || physical >= 0x1000000u ||
      capacity > 0x1000000u - physical || offset > logical_capacity ||
      size > logical_capacity - offset)
    return H2_PAL_ERR_INVALID_ARG;
  if (h2_bk_flash_access_ready() != H2_PAL_OK) return H2_PAL_ERR_UNSUPPORTED;
  while (size) {
    const uint32_t first_block = offset / 32u;
    size_t blocks = (offset % 32u + size + 31u) / 32u;
    if (blocks > 16u) blocks = 16u;
    if (bk_flash_read_bytes(physical + first_block * 34u, packets,
                            (uint32_t)blocks * 34u) != BK_OK)
      return H2_PAL_ERR_IO;
    for (size_t i = 0; i < blocks; ++i) {
      const uint8_t* packet = packets + i * 34u;
      const uint32_t within = offset % 32u;
      size_t take = 32u - within;
      if (take > size) take = size;
      if (h2_bk_flash_erased(packet)) {
        memset(dst, 0xff, take);
      } else {
        if (!h2_bk_flash_crc_valid(packet)) return H2_PAL_ERR_IO;
        const uintptr_t address =
            0x02000000u + physical / 34u * 32u + (first_block + i) * 32u;
        const uint32_t flags = rtos_enter_critical();
        /* Invalidate on the executing core; exclude migration through copy.
         * Batch raw reads to avoid a separate CP RPC for every CRC unit. */
        SCB_InvalidateDCache_by_Addr((void*)address, 32);
        __DSB();
#ifdef H2_BK_FLASH_ACCESS_HOST_TEST
        h2_bk_test_mapped_copy(address, within, dst, take);
#else
        memcpy(dst, (const void*)(address + within), take);
#endif
        rtos_exit_critical(flags);
      }
      dst += take;
      offset += (uint32_t)take;
      size -= take;
    }
  }
  return H2_PAL_OK;
}
