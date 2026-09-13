#include "jieli_warm_boot.h"

#include "asm/includes.h"

#include <string.h>

/* Board-layout warm hand-off from the ROM-selected Loader to another flash bank.
 *
 * uboot picks the bank with the highest valid BootInfo, programs
 * JL_SFC->BASE_ADR with that bank's sfc_base_addr and calls the image entry
 * (_start, 0x2000120) with r0 pointing at a hand-off block that
 * boot_info_init() copies into boot_info/sdram_info/ex_app_info. The Loader
 * reproduces exactly that last step for a bank BootInfo did not select. Code
 * addresses and SFC decryption are CPU-address relative, so the same image
 * runs from either bank once the window moves.
 *
 * The request and the hand-off block live at fixed addresses in the unused
 * tail of the H2 retained RAM window (0x1c7dd4c..0x1c7fd4c), below
 * boot_info. They never move with an image's .h2_retained layout, survive
 * the software reset, and are not touched by chip_entry before
 * boot_info_init() has consumed them. */

#define WARM_SNAPSHOT_ADDR UINT32_C(0x01c7ee00)
#define WARM_HANDOFF_ADDR UINT32_C(0x01c7fc40)
#define WARM_FLASH_HEAD_ADDR UINT32_C(0x01c7fcc0)
#define BOOT_INFO_ADDR UINT32_C(0x01c7fd4c)
#define SDRAM_INFO_ADDR UINT32_C(0x01c7fd74)
#define EX_APP_INFO_ADDR UINT32_C(0x01c7fd7c)

#define SNAPSHOT_MAGIC UINT32_C(0x50414e53) /* "SNAP" */
/* coredump.c's retained layout, identical in every H2Loader-layout image. */
#define RAM_MARKER_ADDR UINT32_C(0x01c7dd4c)
#define RETAINED_LOG_ADDR UINT32_C(0x01c7dd5c)
#define RETAINED_LOG_CAPACITY 2048u
#define IMAGE_ENTRY UINT32_C(0x02000120)
#define FLASH_WINDOW UINT32_C(0x02000000)
#define FLASH_WINDOW_SIZE UINT32_C(0x00800000)


/* What the image entered by the last warm hand-off left behind: its boot
 * probe marker and retained console ring. */
typedef struct warm_snapshot {
  uint32_t magic;
  uint32_t marker[4];
  uint32_t log_magic;
  uint32_t log_head;
  uint32_t log_total;
  uint8_t log[RETAINED_LOG_CAPACITY];
} warm_snapshot_t;

#define SNAPSHOT ((volatile warm_snapshot_t *)WARM_SNAPSHOT_ADDR)

#define REQUEST ((volatile h2_jieli_warm_request_t *)H2_JIELI_WARM_REQUEST_ADDR)

extern void icache_flush(void *ptr, int len);
extern void flushinv_dcache(void *ptr, int len);
extern void flush_all_cache(void);
extern void __local_irq_disable(void);
extern char _HEAP_END[];

uint32_t h2_jieli_warm_boot_count(void) { return REQUEST->count; }

uint32_t h2_jieli_warm_boot_last_base(void) { return REQUEST->last_base; }

uint32_t h2_jieli_warm_boot_running_base(int native_result, uint32_t native_base) {
  return h2_jieli_warm_request_running_base(
      REQUEST, JL_SFC->BASE_ADR, native_result, native_base);
}

void h2_jieli_warm_boot_report(void (*write)(const char *line)) {
  extern int snprintf(char *buffer, size_t size, const char *format, ...);
  char line[160];
  if (write == NULL || SNAPSHOT->magic != SNAPSHOT_MAGIC) return;
  SNAPSHOT->magic = 0u;
  (void)snprintf(line, sizeof(line),
      "H2_JIELI_WARM_SNAPSHOT marker=%08x/%08x stage=%u result=%d "
      "log_magic=%08x head=%u total=%u\r\n",
      (unsigned)SNAPSHOT->marker[0], (unsigned)SNAPSHOT->marker[1],
      (unsigned)SNAPSHOT->marker[2], (int)SNAPSHOT->marker[3],
      (unsigned)SNAPSHOT->log_magic, (unsigned)SNAPSHOT->log_head,
      (unsigned)SNAPSHOT->log_total);
  write(line);
  if (SNAPSHOT->log_head >= RETAINED_LOG_CAPACITY) return;
  uint32_t available = SNAPSHOT->log_total < RETAINED_LOG_CAPACITY
                           ? SNAPSHOT->log_total
                           : RETAINED_LOG_CAPACITY;
  uint32_t start = (SNAPSHOT->log_head + RETAINED_LOG_CAPACITY - available) %
                   RETAINED_LOG_CAPACITY;
  size_t used = 0u;
  for (uint32_t i = 0u; i <= available; ++i) {
    char c = i < available
                 ? (char)SNAPSHOT->log[(start + i) % RETAINED_LOG_CAPACITY]
                 : '\n';
    if (c == '\r') continue;
    if (c == '\n' || used >= 120u) {
      if (used != 0u) {
        line[used] = '\0';
        char out[160];
        (void)snprintf(out, sizeof(out), "H2_JIELI_WARM_LOG %s\r\n", line);
        write(out);
        used = 0u;
      }
      if (c == '\n') continue;
    }
    line[used++] = (c >= 0x20 && c < 0x7f) ? c : '.';
  }
}

#define CACHE_CON ((volatile uint32_t *)UINT32_C(0x01eee008))
#define ICACHE_TAG ((volatile uint32_t *)UINT32_C(0x01f08000))
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
__attribute__((noinline, noreturn)) static void warm_jump(
    uint32_t sfc_base, const void *handoff) {
  remap_flash(sfc_base);
  ((void (*)(const void *))IMAGE_ENTRY)(handoff);
  for (;;) {
  }
}

/* Called first thing in main(), before setup_arch(): only chip_entry has run,
 * so the hardware is still in the state uboot left, apart from memory the
 * next image's chip_entry initialises again. CPU1 is not started yet. */
void h2_jieli_wl82_warm_boot_early(void) {
  if (h2_jieli_warm_request_is_active_image(REQUEST, JL_SFC->BASE_ADR)) {
    return;
  }
  if (REQUEST->inflight == H2_JIELI_WARM_INFLIGHT) {
    REQUEST->inflight = 0u;
    for (uint32_t i = 0u; i < 4u; ++i) {
      SNAPSHOT->marker[i] = ((const volatile uint32_t *)RAM_MARKER_ADDR)[i];
    }
    const volatile uint32_t *log = (const volatile uint32_t *)RETAINED_LOG_ADDR;
    SNAPSHOT->log_magic = log[0];
    SNAPSHOT->log_head = log[1];
    SNAPSHOT->log_total = log[2];
    for (uint32_t i = 0u; i < RETAINED_LOG_CAPACITY; ++i) {
      SNAPSHOT->log[i] = ((const volatile uint8_t *)(RETAINED_LOG_ADDR + 12u))[i];
    }
    SNAPSHOT->magic = SNAPSHOT_MAGIC;
  }
  const uint32_t sfc_base = h2_jieli_warm_request_take(REQUEST);
  if (sfc_base == 0u) {
    return;
  }
  REQUEST->count = REQUEST->count + 1u;
  REQUEST->last_base = sfc_base;
  REQUEST->inflight = H2_JIELI_WARM_INFLIGHT;
  /* A marker only the target image's own boot probe can overwrite. */
  ((volatile uint32_t *)RAM_MARKER_ADDR)[2] = UINT32_C(0xabcd0001);

  const volatile uint8_t *boot_info = (const volatile uint8_t *)BOOT_INFO_ADDR;
  volatile uint8_t *head = (volatile uint8_t *)WARM_FLASH_HEAD_ADDR;
  volatile uint8_t *handoff = (volatile uint8_t *)WARM_HANDOFF_ADDR;
  for (uint32_t i = 0u; i < 32u; ++i) head[i] = 0u;
  for (uint32_t i = 0u; i < 96u; ++i) handoff[i] = 0u;
  /* boot_info_init(): flash_size = fs_info->FlashSize (+8), vm.align =
   * fs_info->align (+13). */
  *(volatile uint32_t *)(head + 8) = *(const volatile uint32_t *)(boot_info + 24);
  head[13] = boot_info[0];
  *(volatile uint32_t *)(handoff + 0) = WARM_FLASH_HEAD_ADDR;
  *(volatile uint32_t *)(handoff + 4) = sfc_base;
  /* system/boot.h: BOOT_DEVICE_INFO.sfc.app_addr is a CPU address, NOT
   * BootInfo.codeLength from the separate native update metadata API. */
  *(volatile uint32_t *)(handoff + 8) = *(const volatile uint32_t *)(boot_info + 20);
  *(volatile uint16_t *)(handoff + 12) = *(const volatile uint16_t *)(boot_info + 28);
  *(volatile uint16_t *)(handoff + 14) = *(const volatile uint16_t *)(boot_info + 30);
  for (uint32_t i = 0u; i < 8u; ++i) handoff[16u + i] = boot_info[32u + i];
  for (uint32_t i = 0u; i < 8u; ++i) {
    handoff[80u + i] = ((const volatile uint8_t *)SDRAM_INFO_ADDR)[i];
  }
  for (uint32_t i = 0u; i < 4u; ++i) {
    handoff[88u + i] = ((const volatile uint8_t *)EX_APP_INFO_ADDR)[i];
  }

  __local_irq_disable();
  /* Write back everything this image's chip_entry dirtied so no stale line is
   * evicted over the next image's freshly initialised memory. */
  flushinv_dcache((void *)0x01c00000, 0x00080000);
  flushinv_dcache((void *)0x04000000, (int)((uint32_t)_HEAP_END - 0x04000000u + 32u));
  flushinv_dcache((void *)FLASH_WINDOW, (int)FLASH_WINDOW_SIZE);
  warm_jump(sfc_base, (const void *)WARM_HANDOFF_ADDR);
}
