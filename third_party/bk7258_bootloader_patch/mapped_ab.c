/* Freestanding adaptation of the pinned vendor checker entry. The original
 * bootloader still owns slot attempts, confirmation flags and the final jump.
 * This payload has no key, UART output, heap or writable static storage. */
#include "contract.h"

#define RBL_SIZE 96u
#define RBL_PAGE_SIZE 4096u
#define FNV_SEED 0x811c9dc5u

static uint32_t read_word(const volatile uint8_t* p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}

static int valid_vector(const volatile uint8_t* view, uint32_t offset,
                        uint32_t base, uint32_t end) {
  uint32_t sp = read_word(view + offset), pc = read_word(view + offset + 4u);
  return sp >= 0x28000000u && sp <= 0x280a0000u && !(sp & 7u) &&
         (pc & 1u) && (pc & ~1u) >= base + offset &&
         (pc & ~1u) < base + end;
}

#ifndef BK_MAPPED_AB_HOST_TEST
#define FLASH_BEGIN (*(volatile uint32_t*)0x44030058u)
#define FLASH_END (*(volatile uint32_t*)0x4403005cu)
#define FLASH_DELTA (*(volatile uint32_t*)0x44030060u)
#define FLASH_ENABLE (*(volatile uint32_t*)0x44030064u)
#define FLASH_STATE (*(volatile uint32_t*)0x44030024u)
#define SCB_CCR (*(volatile uint32_t*)0xe000ed14u)

static unsigned crc_errors(void) { return (FLASH_STATE >> 8) & 0xffu; }
static void barrier(void) { __asm__ volatile("dsb sy\n\tisb sy" ::: "memory"); }
#else
extern unsigned bk_mapped_ab_test_crc_errors(void);
extern volatile uint32_t bk_mapped_ab_test_registers[5];
extern const uint8_t* bk_mapped_ab_test_lookup(const char* name);
extern const volatile uint8_t* bk_mapped_ab_test_xip(uint32_t address);
extern void bk_mapped_ab_test_sync_view(uint32_t base, uint32_t size);
#define FLASH_BEGIN bk_mapped_ab_test_registers[0]
#define FLASH_END bk_mapped_ab_test_registers[1]
#define FLASH_DELTA bk_mapped_ab_test_registers[2]
#define FLASH_ENABLE bk_mapped_ab_test_registers[3]
#define SCB_CCR bk_mapped_ab_test_registers[4]
#define crc_errors bk_mapped_ab_test_crc_errors
static void barrier(void) {}
#endif

/* The pinned SDK core_star.h fixes both cache line sizes at 32 bytes.
 * Invalidate only read-only Flash data: never discard dirty RAM/stack lines.
 * Run after every mapping change, before validating or executing that view. */
static void sync_view(uint32_t base, uint32_t size) {
  barrier();
#ifdef BK_MAPPED_AB_HOST_TEST
  bk_mapped_ab_test_sync_view(base, size);
#else
  if (SCB_CCR & (1u << 16)) {
    for (uint32_t offset = 0; offset < size; offset += 32u)
      *(volatile uint32_t*)0xe000ef5cu = base + offset; /* DCIMVAC */
  }
  if (SCB_CCR & (1u << 17))
    *(volatile uint32_t*)0xe000ef50u = 0u; /* ICIALLU */
#endif
  barrier();
}

static const volatile uint8_t* xip_pointer(uint32_t address) {
#ifdef BK_MAPPED_AB_HOST_TEST
  return bk_mapped_ab_test_xip(address);
#else
  return (const volatile uint8_t*)address;
#endif
}

/* Returns failure on a malformed header/vector, payload digest mismatch or
 * controller CRC-error transition. A counter already at 0xff cannot show a new
 * error, and checking each 32-byte XIP unit keeps a transition from being lost
 * to the eight-bit limit. */
int bk_mapped_ab_validate(const volatile uint8_t* view, uint32_t size,
                          uint32_t cp_size, uint32_t base) {
  if (!view || size <= RBL_PAGE_SIZE || (size & 4095u) ||
      !cp_size || (cp_size & 4095u) || cp_size > size - RBL_PAGE_SIZE - 8u ||
      base > UINT32_MAX - size)
    return -1;
  const unsigned errors = crc_errors();
  if (errors == 0xffu) return -1;
  const volatile uint8_t* head = view + size - RBL_PAGE_SIZE;
  const uint32_t payload_size = read_word(head + 84u);
  if (read_word(head) != 0x004c4252u || head[4] || head[5] ||
      !payload_size || (payload_size & 31u) ||
      payload_size != read_word(head + 88u) || payload_size < cp_size + 8u ||
      payload_size > size - RBL_PAGE_SIZE)
    return -1;
  uint32_t crc = 0xffffffffu;
  for (uint32_t i = 0; i < RBL_SIZE - 4u; ++i)
    crc = bk_mapped_ab_crc_step(crc, head[i]);
  if (~crc != read_word(head + 92u) || crc_errors() != errors ||
      !valid_vector(view, 0, base, cp_size) ||
      !valid_vector(view, cp_size, base, payload_size) || crc_errors() != errors)
    return -1;
  uint32_t hash = FNV_SEED;
  crc = 0xffffffffu;
  for (uint32_t offset = 0; offset < payload_size; offset += 32u) {
    for (uint32_t i = 0; i < 32u; ++i) {
      uint8_t byte = view[offset + i];
      hash = (hash ^ byte) * 0x1000193u;
      crc = bk_mapped_ab_crc_step(crc, byte);
    }
    if (crc_errors() != errors) return -1;
  }
  return hash == read_word(head + 80u) && ~crc == read_word(head + 76u) &&
                 crc_errors() == errors
             ? 0
             : -1;
}

/* Name lookup and descriptor layout are pinned in manifest.json. Header and
 * payload are both selected from this lookup, fixing the vendor fallback's
 * mismatched-payload argument without changing native state transitions. */
#ifndef BK_MAPPED_AB_HOST_TEST
__attribute__((section(".text.entry")))
#endif
int bk_mapped_ab_check(const void* ignored, const char* name) {
  (void)ignored;
  FLASH_ENABLE &= ~1u;
  barrier();
  const bk_mapped_ab_contract_t* contract =
      (const bk_mapped_ab_contract_t*)xip_pointer(
          BK_MAPPED_AB_XIP_BASE + BK_MAPPED_AB_DESCRIPTOR_OFFSET);
  if (!bk_mapped_ab_contract_valid(contract)) return -1;
  if (bk_mapped_ab_crc32(
          (const void*)xip_pointer(BK_MAPPED_AB_XIP_BASE +
                                  contract->payload_offset),
          contract->payload_size) != contract->payload_crc32)
    return -1;
  typedef const uint8_t* (*lookup_t)(const char*);
#ifdef BK_MAPPED_AB_HOST_TEST
  const lookup_t lookup = bk_mapped_ab_test_lookup;
#else
  const lookup_t lookup = (lookup_t)0x02002d85u;
#endif
  const uint8_t *a = lookup("appa"), *b = lookup("s_app"),
                *target = name ? lookup(name) : NULL;
  if (!a || !b || (target != a && target != b) ||
      read_word(a + 52u) != contract->loader_offset ||
      read_word(a + 56u) != contract->loader_size ||
      read_word(b + 52u) != contract->trial_offset ||
      read_word(b + 56u) != contract->trial_size)
    return -1;
  const uint32_t base = BK_MAPPED_AB_XIP_BASE +
                        bk_mapped_ab_logical(contract->loader_offset);
  const uint32_t size = bk_mapped_ab_logical(contract->loader_size);
  /* Mapping was disabled at entry; drop a previous B view before fallback. */
  sync_view(base, size);
  if (target == b) {
    FLASH_BEGIN = base;
    FLASH_END = base + bk_mapped_ab_logical(contract->loader_size);
    FLASH_DELTA = BK_MAPPED_AB_XIP_BASE +
                  bk_mapped_ab_logical(contract->trial_offset -
                                       contract->loader_offset);
    barrier();
    FLASH_ENABLE |= 1u;
    barrier();
    sync_view(base, size);
    if (FLASH_BEGIN != base ||
        FLASH_END != base + size ||
        FLASH_DELTA != BK_MAPPED_AB_XIP_BASE +
                           bk_mapped_ab_logical(contract->trial_offset -
                                                contract->loader_offset) ||
        !(FLASH_ENABLE & 1u)) {
      FLASH_ENABLE &= ~1u;
      sync_view(base, size);
      return -1;
    }
  }
  int rc = bk_mapped_ab_validate(
      xip_pointer(base), bk_mapped_ab_logical(contract->loader_size),
      bk_mapped_ab_logical(contract->cp_size), base);
  if (rc) {
    FLASH_ENABLE &= ~1u;
    sync_view(base, size);
  }
  return rc;
}
