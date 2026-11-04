#ifndef H2_JIELI_WL82_BOOT_H
#define H2_JIELI_WL82_BOOT_H

#include <stdint.h>

/* Board/linker input, available before setup_arch and CPU1 startup. All
 * addresses are physical NOR/SFC offsets, except the retained CPU address ABI
 * below. The board provides one immutable object for every image of a layout.
 */
typedef struct h2_jieli_wl82_boot_geometry {
  uint32_t bank_1_sfc_base;
  uint32_t bank_2_sfc_base;
  uint32_t coredump_flash_address;
} h2_jieli_wl82_boot_geometry_t;
const h2_jieli_wl82_boot_geometry_t* h2_jieli_wl82_boot_geometry(void);

/* Fixed WL82 retained ABI. The board linker must reserve this complete 8 KiB
 * window unchanged in every image participating in a warm handoff. */
#define H2_JIELI_WARM_REQUEST_ADDR UINT32_C(0x01c7fc00)
#define H2_JIELI_WARM_MAGIC UINT32_C(0x42573248)
#define H2_JIELI_WARM_INFLIGHT UINT32_C(0x464c4e49)

typedef struct h2_jieli_warm_request {
  uint32_t magic;
  uint32_t sfc_base;
  uint32_t check;
  uint32_t count;
  uint32_t last_base;
  uint32_t inflight;
} h2_jieli_warm_request_t;

static inline uint32_t h2_jieli_warm_request_check(uint32_t base) {
  return ~(H2_JIELI_WARM_MAGIC ^ base ^ UINT32_C(0x5a5aa5a5));
}

static inline int h2_jieli_warm_request_active_at(
    const volatile h2_jieli_warm_request_t* request, uint32_t mapped_base,
    uint32_t bank_2_base) {
  return mapped_base == bank_2_base && request->magic == 0u &&
         request->inflight == H2_JIELI_WARM_INFLIGHT &&
         request->sfc_base == mapped_base &&
         request->last_base == mapped_base &&
         request->check == h2_jieli_warm_request_check(mapped_base);
}

static inline uint32_t h2_jieli_warm_request_mapped_base(
    const volatile h2_jieli_warm_request_t* request, uint32_t mapped_base,
    int native_result, uint32_t native_base, uint32_t bank_1_base,
    uint32_t bank_2_base) {
  if (mapped_base != bank_1_base && mapped_base != bank_2_base) return 0u;
  if (native_result == 0) return native_base == mapped_base ? mapped_base : 0u;
  return h2_jieli_warm_request_active_at(request, mapped_base, bank_2_base)
             ? mapped_base
             : 0u;
}

/* Consume before attempting a handoff. Interrupted/torn/replayed publication
 * fails closed into the ROM-selected image. */
static inline uint32_t h2_jieli_warm_request_consume(
    volatile h2_jieli_warm_request_t* request, uint32_t bank_1_base,
    uint32_t bank_2_base) {
  const uint32_t magic = request->magic;
  const uint32_t base = request->sfc_base;
  const uint32_t check = request->check;
  request->magic = 0u;
  if (magic != H2_JIELI_WARM_MAGIC ||
      check != h2_jieli_warm_request_check(base) ||
      (base != bank_1_base && base != bank_2_base))
    return 0u;
  return base;
}

void h2_jieli_wl82_warm_boot_request(uint32_t base);
void h2_jieli_wl82_warm_boot_early(void);

#endif
