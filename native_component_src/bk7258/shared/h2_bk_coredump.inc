#include "sdkconfig.h"
#include "h2_bk_coredump_shared.h"

#include <stddef.h>
#include <string.h>

#if CONFIG_H2_BK_PLATFORM_COREDUMP || CONFIG_H2_BK_PLATFORM_CP_COREDUMP
#ifdef H2_BK_COREDUMP_HOST_TEST
#include "h2_bk_coredump_test_sdk.h"
#else
#include "armstar.h"
#include "stack_base.h"
#include "bk_fault_observer.h"
#include <driver/flash.h>
#include <os/os.h>
#include <soc/reg_base.h>
/* Present in the pinned SDK driver specifically for IRQ-disabled dumps. */
extern bk_err_t bk_flash_dump_write(uint32_t address, const uint8_t* data,
                                    uint32_t size);
#if CONFIG_H2_BK_PLATFORM_CP_COREDUMP
#define H2_CD_CORE() 0u
#else
#define H2_CD_CORE() ((uint32_t)rtos_get_core_id() - 1u)
#endif
#define H2_CD_COPY(dst, address, length) \
  memcpy((dst), (const void*)(uintptr_t)(address), (length))
#endif

enum {
  HEADER_BYTES = 256,
  STACK_HEADER_BYTES = 32,
  STACK_BYTES = 4096,
  FOOTER_BYTES = 32,
  CAPTURE_BYTES =
      HEADER_BYTES + STACK_HEADER_BYTES + STACK_BYTES + FOOTER_BYTES,
};
static uint32_t dump_address;
static uint32_t dump_size;
static volatile uint32_t claimed;
static bool configured;
static bool armed;
/* Fatal capture needs neither heap allocation nor the interrupted task stack.
 */
static uint8_t register_record[HEADER_BYTES];
static uint8_t stack_record[STACK_BYTES];
static uint8_t stack_header[STACK_HEADER_BYTES];
static uint8_t stack_footer[FOOTER_BYTES];

static void put32(uint8_t* out, uint32_t value) {
  for (unsigned i = 0; i < 4u; ++i) out[i] = (uint8_t)(value >> (8u * i));
}

static uint32_t get32(const uint8_t* in) {
  uint32_t value = 0;
  for (unsigned i = 0; i < 4u; ++i) value |= (uint32_t)in[i] << (8u * i);
  return value;
}

static uint32_t crc_update(uint32_t crc, const uint8_t* data, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8u; ++bit)
      crc = (crc >> 1u) ^ (UINT32_C(0xedb88320) & (0u - (crc & 1u)));
  }
  return crc;
}

static uint32_t crc32(const uint8_t* data, size_t size) {
  return ~crc_update(UINT32_MAX, data, size);
}

static uint32_t stack_length(uint32_t sp, uint32_t cfsr) {
  /* A stacking/unstacking fault does not provide a safe readable frame. */
  if ((sp & 3u) != 0u || (cfsr & UINT32_C(0x3838)) != 0u) return 0;
  uint32_t end = 0;
  if (sp >= UINT32_C(0x28000000) && sp < UINT32_C(0x280a0000))
    end = UINT32_C(0x280a0000);
  else if (sp >= UINT32_C(0x20000000) && sp < UINT32_C(0x20004000))
    end = UINT32_C(0x20004000);
  if (end == 0u) return 0;
  uint32_t size = end - sp;
  if (size > STACK_BYTES) size = STACK_BYTES;
  return size & ~UINT32_C(31);
}

int h2_bk_coredump_configure(uint32_t address, uint32_t size) {
  if (!bk_flash_is_driver_inited()) return H2_PAL_ERR_INVALID_STATE;
  const uint32_t flash_size = bk_flash_get_current_total_size();
  if ((address & 4095u) != 0u || size < CAPTURE_BYTES ||
      address >= flash_size || size > flash_size - address)
    return H2_PAL_ERR_INVALID_ARG;
  if (configured)
    return address == dump_address && size == dump_size
               ? H2_PAL_OK
               : H2_PAL_ERR_INVALID_STATE;
  uint8_t bytes[32];
  bool blank = true;
  for (uint32_t offset = 0; offset < CAPTURE_BYTES; offset += sizeof(bytes)) {
    if (bk_flash_read_bytes(address + offset, bytes, sizeof(bytes)) != BK_OK)
      return H2_PAL_ERR_IO;
    for (unsigned i = 0; i < sizeof(bytes); ++i)
      if (bytes[i] != 0xffu) blank = false;
  }
  dump_address = address;
  dump_size = size;
  claimed = 0u;
  armed = blank;
  configured = true;
  return H2_PAL_OK;
}

bool h2_bk_coredump_armed(void) {
  return configured && armed &&
         __atomic_load_n(&claimed, __ATOMIC_ACQUIRE) == 0u;
}

/* Called from the SDK exception path with local IRQs disabled, after the
 * native exception guard/prologue. It is not an application-written marker. */
void h2_bk_coredump_capture(uint32_t reason, const SAVED_CONTEXT* context) {
  uint32_t expected = 0u;
  if (!configured || !armed || context == NULL ||
      !__atomic_compare_exchange_n(&claimed, &expected, 1u, false,
                                   __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
    return;
  const uint32_t cfsr = SCB->CFSR;
  const uint32_t bytes = stack_length((uint32_t)context->sp, cfsr);
  memset(register_record, 0, sizeof(register_record));
  /* H2Loader's existing transport reads the little-endian length first. */
  put32(register_record, CAPTURE_BYTES);
  memcpy(register_record + 4u, "H2BKDMP1", 8u);
#if CONFIG_H2_BK_PLATFORM_CP_COREDUMP
  put32(register_record + 12u, 2u);
#else
  put32(register_record + 12u, 1u);
#endif
  put32(register_record + 16u, H2_CD_CORE());
  put32(register_record + 20u, reason);
  put32(register_record + 24u, bytes);
  put32(register_record + 28u, HEADER_BYTES);
  for (unsigned i = 0; i < 24u; ++i)
    put32(register_record + 32u + 4u * i, (uint32_t)context->cm33_regs[i]);
  put32(register_record + 128u, cfsr);
  put32(register_record + 132u, SCB->HFSR);
  put32(register_record + 136u, SCB->BFAR);
  put32(register_record + 140u, SCB->MMFAR);
#if CONFIG_H2_BK_PLATFORM_CP_COREDUMP
  put32(register_record + 144u, 2u);
#endif
  put32(register_record + 248u, crc32(register_record, 248u));
  put32(register_record + 252u, UINT32_C(0x43444f4e));
  if (bk_flash_dump_write(dump_address, register_record,
                          sizeof(register_record)) != BK_OK ||
      bytes == 0u)
    return;
  H2_CD_COPY(stack_record, (uint32_t)context->sp, bytes);
  memset(stack_header, 0, sizeof(stack_header));
  memcpy(stack_header, "H2STACK1", 8u);
  put32(stack_header + 8u, (uint32_t)context->sp);
  put32(stack_header + 12u, bytes);
  put32(stack_header + 16u, crc32(stack_record, bytes));
  put32(stack_header + 24u, crc32(stack_header, 24u));
  put32(stack_header + 28u, UINT32_C(0x53544f4e));
  memset(stack_footer, 0, sizeof(stack_footer));
  memcpy(stack_footer, "H2END001", 8u);
  const uint32_t body = dump_address + HEADER_BYTES + STACK_HEADER_BYTES;
  if (bk_flash_dump_write(body, stack_record, bytes) != BK_OK ||
      bk_flash_dump_write(body + bytes, stack_footer, sizeof(stack_footer)) !=
          BK_OK)
    return;
  (void)bk_flash_dump_write(dump_address + HEADER_BYTES, stack_header,
                            sizeof(stack_header));
}

#ifndef H2_BK_COREDUMP_HOST_TEST
void bk_fault_context_persist(uint32_t reason, const SAVED_CONTEXT* context) {
  h2_bk_coredump_capture(reason, context);
}
#endif

int h2_bk_coredump_read_info(h2_bk_coredump_info_t* out_info) {
  if (out_info == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out_info, 0, sizeof(*out_info));
  if (!configured) return H2_PAL_ERR_INVALID_STATE;
  uint8_t header[HEADER_BYTES];
  if (bk_flash_read_bytes(dump_address, header, sizeof(header)) != BK_OK)
    return H2_PAL_ERR_IO;
  if (get32(header) != CAPTURE_BYTES ||
      memcmp(header + 4u, "H2BKDMP1", 8u) != 0 || (get32(header + 12u) != 1u && get32(header + 12u) != 2u) ||
      get32(header + 28u) != HEADER_BYTES ||
      get32(header + 248u) != crc32(header, 248u) ||
      get32(header + 252u) != UINT32_C(0x43444f4e))
    return H2_PAL_ERR_NOT_FOUND;
  if (get32(header + 12u) == 2u && get32(header + 144u) != 2u)
    return H2_PAL_ERR_NOT_FOUND;
  out_info->processor = get32(header + 12u) == 2u ? 2u : 1u;
  out_info->core = get32(header + 16u);
  out_info->reason = get32(header + 20u);
  out_info->sp = get32(header + 32u + 13u * 4u);
  out_info->lr = get32(header + 32u + 14u * 4u);
  out_info->pc = get32(header + 32u + 15u * 4u);
  out_info->cfsr = get32(header + 128u);
  out_info->hfsr = get32(header + 132u);
  uint8_t stack[STACK_HEADER_BYTES];
  if (bk_flash_read_bytes(dump_address + HEADER_BYTES, stack, sizeof(stack)) !=
      BK_OK)
    return H2_PAL_ERR_IO;
  if (memcmp(stack, "H2STACK1", 8u) == 0 &&
      get32(stack + 24u) == crc32(stack, 24u) &&
      get32(stack + 28u) == UINT32_C(0x53544f4e)) {
    const uint32_t size = get32(stack + 12u);
    uint32_t crc = UINT32_MAX;
    bool readable = size != 0u && size <= STACK_BYTES && (size & 31u) == 0u;
    uint8_t chunk[128];
    for (uint32_t offset = 0; readable && offset < size;) {
      const uint32_t count =
          size - offset < sizeof(chunk) ? size - offset : sizeof(chunk);
      readable = bk_flash_read_bytes(
                     dump_address + HEADER_BYTES + sizeof(stack) + offset,
                     chunk, count) == BK_OK;
      if (readable) crc = crc_update(crc, chunk, count);
      offset += count;
    }
    if (readable && ~crc == get32(stack + 16u)) {
      uint8_t footer[FOOTER_BYTES];
      if (bk_flash_read_bytes(
              dump_address + HEADER_BYTES + sizeof(stack) + size, footer,
              sizeof(footer)) == BK_OK &&
          memcmp(footer, "H2END001", 8u) == 0) {
        out_info->stack_address = get32(stack + 8u);
        out_info->stack_bytes = size;
      }
    }
  }
  return H2_PAL_OK;
}
#ifdef H2_BK_COREDUMP_HOST_TEST
void h2_bk_coredump_test_reset(void) {
  configured = false;
  armed = false;
  claimed = 0u;
}
#endif
#else
int h2_bk_coredump_configure(uint32_t address, uint32_t size) {
  (void)address;
  (void)size;
  return H2_PAL_ERR_UNSUPPORTED;
}
bool h2_bk_coredump_armed(void) { return false; }
int h2_bk_coredump_read_info(h2_bk_coredump_info_t* out_info) {
  if (out_info == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out_info, 0, sizeof(*out_info));
  return H2_PAL_ERR_UNSUPPORTED;
}
#endif
