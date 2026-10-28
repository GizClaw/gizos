#include "h2_bk_coredump.h"
#include "h2_bk_coredump_test_sdk.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { BASE = 0x700000, CAPACITY = 8192 };
static uint8_t flash[CAPACITY];
static uint8_t memory[4096];
static unsigned writes, fail_write;
static bool initialized = true;
test_scb_t test_scb;
extern void h2_bk_coredump_capture(uint32_t reason,
                                   const SAVED_CONTEXT* context);
extern void h2_bk_coredump_test_reset(void);

bool bk_flash_is_driver_inited(void) { return initialized; }
uint32_t bk_flash_get_current_total_size(void) { return 0x800000; }
int bk_flash_read_bytes(uint32_t address, uint8_t* data, uint32_t size) {
  assert(address >= BASE && size <= CAPACITY &&
         address - BASE <= CAPACITY - size);
  memcpy(data, flash + (address - BASE), size);
  return BK_OK;
}
int bk_flash_dump_write(uint32_t address, const uint8_t* data, uint32_t size) {
  assert(address >= BASE && size <= CAPACITY &&
         address - BASE <= CAPACITY - size);
  ++writes;
  if (writes == fail_write) {
    memcpy(flash + (address - BASE), data, size / 2u);
    return -1;
  }
  for (uint32_t i = 0; i < size; ++i) flash[address - BASE + i] &= data[i];
  return BK_OK;
}
void test_copy_stack(void* data, uint32_t address, uint32_t size) {
  assert(address == 0x28010000 && size <= sizeof(memory));
  memcpy(data, memory, size);
}
static void blank(void) {
  h2_bk_coredump_test_reset();
  memset(flash, 0xff, sizeof(flash));
  for (unsigned i = 0; i < sizeof(memory); ++i) memory[i] = (uint8_t)(i * 7u);
  writes = fail_write = 0;
  test_scb = (test_scb_t){.CFSR = 0x10000, .HFSR = 0x40000000};
}
int main(void) {
  SAVED_CONTEXT context = {.r0 = 0xc0de0001u,
                           .r1 = 0xc0de0002u,
                           .sp = 0x28010000,
                           .pc = 0x2345678,
                           .lr = 0x2341235,
                           .xpsr = 0x1000000};
  h2_bk_coredump_info_t info;
  blank();
  initialized = false;
  assert(h2_bk_coredump_configure(BASE, CAPACITY) == H2_PAL_ERR_INVALID_STATE);
  initialized = true;
  assert(h2_bk_coredump_configure(BASE + 1, CAPACITY) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_bk_coredump_configure(BASE, 4096) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_bk_coredump_configure(BASE, CAPACITY) == H2_PAL_OK);
  assert(h2_bk_coredump_armed());
  h2_bk_coredump_capture(3u, &context);
  assert(!h2_bk_coredump_armed() && writes == 4u);
  /* Existing H2Loader export reads the little-endian byte length first. */
  assert(flash[0] == 0x40u && flash[1] == 0x11u && flash[2] == 0u &&
         flash[3] == 0u);
  assert(h2_bk_coredump_read_info(&info) == H2_PAL_OK);
  assert(info.pc == 0x2345678 && info.sp == 0x28010000);
#if CONFIG_H2_BK_PLATFORM_CP_COREDUMP
  assert(info.processor == 2u && info.core == 0u);
#else
  assert(info.processor == 1u && info.core == 1u);
#endif
  assert(info.reason == 3u && info.stack_bytes == 4096u);
  assert(memcmp(flash + 288u, memory, sizeof(memory)) == 0);
  h2_bk_coredump_capture(7u, &context);
  assert(writes == 4u);
  h2_bk_coredump_test_reset();
  assert(h2_bk_coredump_configure(BASE, CAPACITY) == H2_PAL_OK);
  assert(!h2_bk_coredump_armed());
  assert(h2_bk_coredump_read_info(&info) == H2_PAL_OK);
  flash[288u] ^= 1u;
  assert(h2_bk_coredump_read_info(&info) == H2_PAL_OK && info.stack_bytes == 0);
  flash[32u] ^= 1u;
  assert(h2_bk_coredump_read_info(&info) == H2_PAL_ERR_NOT_FOUND);
  blank();
  fail_write = 2;
  assert(h2_bk_coredump_configure(BASE, CAPACITY) == H2_PAL_OK);
  h2_bk_coredump_capture(3u, &context);
  assert(h2_bk_coredump_read_info(&info) == H2_PAL_OK && info.stack_bytes == 0);
  blank();
  fail_write = 1;
  assert(h2_bk_coredump_configure(BASE, CAPACITY) == H2_PAL_OK);
  h2_bk_coredump_capture(3u, &context);
  assert(h2_bk_coredump_read_info(&info) == H2_PAL_ERR_NOT_FOUND);
  blank();
  context.sp = 0x60010000;
  assert(h2_bk_coredump_configure(BASE, CAPACITY) == H2_PAL_OK);
  h2_bk_coredump_capture(3u, &context);
  assert(h2_bk_coredump_read_info(&info) == H2_PAL_OK && info.stack_bytes == 0);
  puts(
      "coredump: reboot, first fault, CRC, partial writes and unsafe stack PASS");
  return 0;
}
