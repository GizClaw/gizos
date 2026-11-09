#include "jieli_warm_boot.h"
#include "asm/includes.h"

/* Linked only by the layout's recovery/Loader startup graph. Apps share the
 * retained publisher and observation helpers, never this early consumer. */
#define WARM_HANDOFF_ADDR UINT32_C(0x01c7fc40)
#define WARM_FLASH_HEAD_ADDR UINT32_C(0x01c7fcc0)
#define BOOT_INFO_ADDR UINT32_C(0x01c7fd4c)
#define SDRAM_INFO_ADDR UINT32_C(0x01c7fd74)
#define EX_APP_INFO_ADDR UINT32_C(0x01c7fd7c)
#define IMAGE_ENTRY UINT32_C(0x02000120)
#define FLASH_WINDOW UINT32_C(0x02000000)
#define FLASH_WINDOW_SIZE UINT32_C(0x00800000)
#define REQUEST ((volatile h2_jieli_warm_request_t*)H2_JIELI_WARM_REQUEST_ADDR)
extern void flushinv_dcache(void* ptr, int len);
extern void flush_all_cache(void);
extern void __local_irq_disable(void);
extern char _HEAP_END[];

#define CACHE_CON ((volatile uint32_t*)UINT32_C(0x01eee008))
#define ICACHE_TAG ((volatile uint32_t*)UINT32_C(0x01f08000))
#define ICACHE_TAG_COUNT 1024u

/* Move the flash window the way the SDK's flush_all_cache() reconfigures the
 * SFC: caches off, SFC idle and disabled, every icache tag invalidated, then
 * the SFC and caches restored. Dirty data must drain under the old mapping. */
AT(.volatile_ram_code)
__attribute__((noinline)) static void remap_flash(uint32_t sfc_base) {
  /* Drain dirty data-cache lines under the OLD mapping. The pinned SDK
   * implementation lives in RAM and also invalidates the instruction tags.
   * Clearing instruction tags alone is not its complete flush sequence. */
  flush_all_cache();
  const uint32_t sfc_con = JL_SFC->CON;
  __asm__ volatile("csync");
  while ((*CACHE_CON & UINT32_C(0x4000)) == 0u) {
  }
  const uint32_t cache_con = *CACHE_CON;
  *CACHE_CON = cache_con & ~UINT32_C(0x300);
  __asm__ volatile("csync");
  while ((int32_t)JL_SFC->CON < 0) {
  }
  JL_SFC->CON = sfc_con & ~UINT32_C(1);
  for (uint32_t i = 0u; i < ICACHE_TAG_COUNT; ++i) {
    ICACHE_TAG[i] &= ~UINT32_C(0x8000);
  }
  JL_SFC->BASE_ADR = sfc_base;
  __asm__ volatile("csync");
  __asm__ volatile("ssync");
  JL_SFC->CON = sfc_con;
  __asm__ volatile("csync");
  *CACHE_CON = cache_con;
  __asm__ volatile("csync");
  __asm__ volatile("ssync");
}

AT(.volatile_ram_code)
__attribute__((noinline, noreturn)) static void warm_jump(uint32_t sfc_base,
                                                          const void* handoff) {
  remap_flash(sfc_base);
  ((void (*)(const void*))IMAGE_ENTRY)(handoff);
  for (;;) {
  }
}

/* Called first thing in main(), before setup_arch(): only chip_entry has run,
 * so the hardware is still in the state uboot left, apart from memory the
 * next image's chip_entry initialises again. CPU1 is not started yet. */
void h2_jieli_wl82_warm_boot_early(void) {
  if (h2_jieli_warm_request_active_at(
          REQUEST, JL_SFC->BASE_ADR,
          h2_jieli_wl82_boot_geometry()->bank_2_sfc_base)) {
    return;
  }
  if (REQUEST->inflight == H2_JIELI_WARM_INFLIGHT)
    h2_jieli_warm_boot_snapshot_previous();
  const uint32_t sfc_base = h2_jieli_warm_request_consume(
      REQUEST, h2_jieli_wl82_boot_geometry()->bank_1_sfc_base,
      h2_jieli_wl82_boot_geometry()->bank_2_sfc_base);
  if (sfc_base == 0u) {
    return;
  }
  h2_jieli_warm_boot_mark_handoff(sfc_base);

  const volatile uint8_t* boot_info = (const volatile uint8_t*)BOOT_INFO_ADDR;
  volatile uint8_t* head = (volatile uint8_t*)WARM_FLASH_HEAD_ADDR;
  volatile uint8_t* handoff = (volatile uint8_t*)WARM_HANDOFF_ADDR;
  for (uint32_t i = 0u; i < 32u; ++i) head[i] = 0u;
  for (uint32_t i = 0u; i < 96u; ++i) handoff[i] = 0u;
  /* boot_info_init(): flash_size = fs_info->FlashSize (+8), vm.align =
   * fs_info->align (+13). */
  *(volatile uint32_t*)(head + 8) = *(const volatile uint32_t*)(boot_info + 24);
  head[13] = boot_info[0];
  *(volatile uint32_t*)(handoff + 0) = WARM_FLASH_HEAD_ADDR;
  *(volatile uint32_t*)(handoff + 4) = sfc_base;
  /* system/boot.h: BOOT_DEVICE_INFO.sfc.app_addr is a CPU address, NOT
   * BootInfo.codeLength from the separate native update metadata API. */
  *(volatile uint32_t*)(handoff + 8) =
      *(const volatile uint32_t*)(boot_info + 20);
  *(volatile uint16_t*)(handoff + 12) =
      *(const volatile uint16_t*)(boot_info + 28);
  *(volatile uint16_t*)(handoff + 14) =
      *(const volatile uint16_t*)(boot_info + 30);
  for (uint32_t i = 0u; i < 8u; ++i) handoff[16u + i] = boot_info[32u + i];
  for (uint32_t i = 0u; i < 8u; ++i) {
    handoff[80u + i] = ((const volatile uint8_t*)SDRAM_INFO_ADDR)[i];
  }
  for (uint32_t i = 0u; i < 4u; ++i) {
    handoff[88u + i] = ((const volatile uint8_t*)EX_APP_INFO_ADDR)[i];
  }

  __local_irq_disable();
  /* Write back everything this image's chip_entry dirtied so no stale line is
   * evicted over the next image's freshly initialised memory. */
  flushinv_dcache((void*)0x01c00000, 0x00080000);
  flushinv_dcache((void*)0x04000000,
                  (int)((uint32_t)_HEAP_END - 0x04000000u + 32u));
  flushinv_dcache((void*)FLASH_WINDOW, (int)FLASH_WINDOW_SIZE);
  warm_jump(sfc_base, (const void*)WARM_HANDOFF_ADDR);
}
