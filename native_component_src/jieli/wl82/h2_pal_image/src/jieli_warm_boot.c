#include "jieli_warm_boot.h"

#include "asm/includes.h"

#include <string.h>

/* Board-layout warm hand-off from the ROM-selected Loader to another flash
 * bank.
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

#define SNAPSHOT_MAGIC UINT32_C(0x50414e53) /* "SNAP" */
/* coredump.c's retained layout, identical in every H2Loader-layout image. */
#define RAM_MARKER_ADDR UINT32_C(0x01c7ed94)
#define RETAINED_LOG_ADDR UINT32_C(0x01c7e588)
#define RETAINED_LOG_CAPACITY 2048u

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

#define SNAPSHOT ((volatile warm_snapshot_t*)WARM_SNAPSHOT_ADDR)

#define REQUEST ((volatile h2_jieli_warm_request_t*)H2_JIELI_WARM_REQUEST_ADDR)

uint32_t h2_jieli_warm_boot_count(void) { return REQUEST->count; }

uint32_t h2_jieli_warm_boot_last_base(void) { return REQUEST->last_base; }

uint32_t h2_jieli_warm_boot_running_base(int native_result,
                                         uint32_t native_base) {
  return h2_jieli_warm_request_mapped_base(
      REQUEST, JL_SFC->BASE_ADR, native_result, native_base,
      h2_jieli_wl82_boot_geometry()->bank_1_sfc_base,
      h2_jieli_wl82_boot_geometry()->bank_2_sfc_base);
}

uint32_t h2_jieli_warm_boot_take_rom_base(void) {
  const h2_jieli_wl82_boot_geometry_t* geometry = h2_jieli_wl82_boot_geometry();
  return h2_jieli_warm_request_take_origin(REQUEST, JL_SFC->BASE_ADR,
                                           geometry->bank_1_sfc_base,
                                           geometry->bank_2_sfc_base);
}

void h2_jieli_warm_boot_report(void (*write)(const char* line)) {
  extern int snprintf(char* buffer, size_t size, const char* format, ...);
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
  if ((SNAPSHOT->log_magic & ~UINT32_C(1)) != UINT32_C(0x474f4c48) ||
      SNAPSHOT->log_head >= RETAINED_LOG_CAPACITY)
    return;
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

void h2_jieli_warm_boot_snapshot_previous(void) {
  REQUEST->inflight = 0u;
  for (uint32_t i = 0u; i < 4u; ++i) {
    SNAPSHOT->marker[i] = ((const volatile uint32_t*)RAM_MARKER_ADDR)[i];
  }
  const volatile uint32_t* log = (const volatile uint32_t*)RETAINED_LOG_ADDR;
  SNAPSHOT->log_magic = log[0];
  SNAPSHOT->log_head = log[1];
  SNAPSHOT->log_total = log[2];
  for (uint32_t i = 0u; i < RETAINED_LOG_CAPACITY; ++i) {
    SNAPSHOT->log[i] = ((const volatile uint8_t*)(RETAINED_LOG_ADDR + 12u))[i];
  }
  SNAPSHOT->magic = SNAPSHOT_MAGIC;
}

void h2_jieli_warm_boot_mark_handoff(uint32_t sfc_base) {
  REQUEST->count = REQUEST->count + 1u;
  REQUEST->last_base = sfc_base;
  REQUEST->inflight = H2_JIELI_WARM_INFLIGHT;
  /* This still runs under the ROM-selected mapping, before warm_jump. */
  h2_jieli_warm_request_record_origin(REQUEST, JL_SFC->BASE_ADR);
  /* A marker only the target image's own boot probe can overwrite. */
  ((volatile uint32_t*)RAM_MARKER_ADDR)[2] = UINT32_C(0xabcd0001);
}

void h2_jieli_wl82_warm_boot_request(uint32_t base) {
  REQUEST->origin_magic = 0u;
  REQUEST->magic = 0u;
  REQUEST->sfc_base = base;
  REQUEST->check = h2_jieli_warm_request_check(base);
  __asm__ volatile("csync" ::: "memory");
  REQUEST->magic = H2_JIELI_WARM_MAGIC;
  __asm__ volatile("csync" ::: "memory");
}
