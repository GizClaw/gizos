#include "app_config.h"
#include "asm/sfc_norflash_api.h"
#include "device/ioctl_cmds.h"
#include "jieli_upgrade_io.h"
#include "h2_jieli_warm_request.h"
#include <string.h>

#ifdef CONFIG_SDFILE_EXT_ENABLE
#error "This h2loader layout upgrade adapter supports internal NOR only"
#endif

/* Complete NOR-only replacement for the pinned update.a dev_upgrade_api.c.o.
 * Supplying every symbol prevents that archive member from being extracted;
 * unlike --wrap this does not depend on redirecting calls after LTO.
 * Keep the updater algorithm and physical NOR implementation in the SDK.
 * Default behavior is SDK passthrough; explicit arming defers the P2 header.
 */
extern u32 boot_info_get_sfc_base_addr(void);
/* Optional, diagnostic-only observer; production packages do not define it. */
__attribute__((weak)) void h2_jieli_upgrade_erase_observer(u32 addr) {
  (void)addr;
}

#define HEADER_ADDR (H2_JIELI_BANK_2_SFC_BASE - H2_JIELI_UPGRADE_HEADER_SIZE)
enum { GATE_OFF, GATE_ARMED, GATE_WRITING, GATE_CAPTURED, GATE_FAILED };
static int header_gate;
static u8 captured_header[H2_JIELI_UPGRADE_HEADER_SIZE];

static int erased(const u8 *data) {
  for (unsigned i = 0; i < H2_JIELI_UPGRADE_HEADER_SIZE; ++i)
    if (data[i] != 0xffu) return 0;
  return 1;
}

static int overlaps_header(u32 addr, u32 len) {
  return len != 0u && addr < H2_JIELI_BANK_2_SFC_BASE &&
         (uint64_t)addr + len > HEADER_ADDR;
}

int h2_jieli_upgrade_header_arm(void) {
  u8 physical[H2_JIELI_UPGRADE_HEADER_SIZE];
  if (boot_info_get_sfc_base_addr() != H2_JIELI_BANK_1_SFC_BASE ||
      norflash_origin_read(physical, HEADER_ADDR, sizeof(physical)) !=
          (int)sizeof(physical) || !erased(physical)) return -1;
  int expected = GATE_OFF;
  return __atomic_compare_exchange_n(&header_gate, &expected, GATE_ARMED, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) ? 0 : -1;
}

int h2_jieli_upgrade_header_copy(u8 out[H2_JIELI_UPGRADE_HEADER_SIZE]) {
  if (out == NULL ||
      __atomic_load_n(&header_gate, __ATOMIC_ACQUIRE) != GATE_CAPTURED) return -1;
  memcpy(out, captured_header, sizeof(captured_header));
  return __atomic_load_n(&header_gate, __ATOMIC_ACQUIRE) == GATE_CAPTURED ? 0 : -1;
}

int h2_jieli_upgrade_header_publish(const u8 header[H2_JIELI_UPGRADE_HEADER_SIZE]) {
  u8 physical[H2_JIELI_UPGRADE_HEADER_SIZE];
  if (header == NULL || erased(header) ||
      boot_info_get_sfc_base_addr() != H2_JIELI_BANK_2_SFC_BASE ||
      norflash_origin_read(physical, HEADER_ADDR, sizeof(physical)) !=
          (int)sizeof(physical)) return -1;
  if (memcmp(physical, header, sizeof(physical)) == 0) return 0;
  if (!erased(physical)) return -1;
  /* SDK callers ignore these return values; physical readback, not an
   * undocumented protection-helper convention, determines commit success. */
  (void)norflash_protect_suspend();
  int written = norflash_write(NULL, (void *)header, sizeof(physical), HEADER_ADDR);
  (void)norflash_protect_resume();
  if (written != (int)sizeof(physical) ||
      norflash_origin_read(physical, HEADER_ADDR, sizeof(physical)) !=
          (int)sizeof(physical)) return -1;
  return memcmp(physical, header, sizeof(physical)) == 0 ? 0 : -1;
}

void switch_upgrade_dev(u8 dev_type) {
  /* The pinned SDK ignores this selector when SDFILE_EXT is disabled. */
  (void)dev_type;
}

u32 get_app_boot_base_addr(void) {
  return boot_info_get_sfc_base_addr();
}

u32 dev_upgrade_read(u8 *buf, u32 addr, u32 len) {
  return norflash_read(NULL, buf, len, addr) == (int)len ? len : 0u;
}

u32 dev_upgrade_origin_read(u8 *buf, u32 addr, u32 len) {
  if (__atomic_load_n(&header_gate, __ATOMIC_ACQUIRE) != GATE_OFF &&
      overlaps_header(addr, len)) {
    return addr == HEADER_ADDR && len == sizeof(captured_header) &&
           h2_jieli_upgrade_header_copy(buf) == 0 ? len : 0u;
  }
  return norflash_origin_read(buf, addr, len) == (int)len ? len : 0u;
}

u32 dev_upgrade_write(u8 *buf, u32 addr, u32 len) {
  if (__atomic_load_n(&header_gate, __ATOMIC_ACQUIRE) != GATE_OFF &&
      overlaps_header(addr, len)) {
    if (buf == NULL || addr != HEADER_ADDR || len != sizeof(captured_header)) {
      __atomic_store_n(&header_gate, GATE_FAILED, __ATOMIC_RELEASE);
      return 0u;
    }
    int expected = GATE_ARMED;
    if (__atomic_compare_exchange_n(&header_gate, &expected, GATE_WRITING, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      memcpy(captured_header, buf, sizeof(captured_header));
      expected = GATE_WRITING;
      return __atomic_compare_exchange_n(&header_gate, &expected, GATE_CAPTURED, 0,
                                        __ATOMIC_RELEASE, __ATOMIC_RELAXED) ? len : 0u;
    }
    if (expected == GATE_CAPTURED &&
        memcmp(captured_header, buf, sizeof(captured_header)) == 0) return len;
    __atomic_store_n(&header_gate, GATE_FAILED, __ATOMIC_RELEASE);
    return 0u;
  }
  return norflash_write(NULL, buf, len, addr) == (int)len ? len : 0u;
}

u8 dev_upgrade_erase(u32 command, u32 addr) {
  u32 ioctl;
  switch (command) {
    case 1u: ioctl = IOCTL_ERASE_BLOCK; break;
    case 2u: ioctl = IOCTL_ERASE_SECTOR; break;
    case 3u: ioctl = IOCTL_ERASE_PAGE; break;
    default: return 0u;
  }
  /* Match the pinned updater adapter's accepted-command result convention. */
  (void)norflash_ioctl(NULL, ioctl, addr);
  h2_jieli_upgrade_erase_observer(addr);
  return 1u;
}

void dev_upgrade_protect_suspend(void) {
  (void)norflash_protect_suspend();
}

void dev_upgrade_protect_resume(void) {
  (void)norflash_protect_resume();
}
