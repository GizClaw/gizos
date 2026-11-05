#include "h2_bk_platform_storage.h"
#include "h2_bk_disk_test_sdk.h"
#include <assert.h>
#include <string.h>
#define BASE 0x7a0000u
static uint8_t storage[8192];
static flash_protect_type_t protect = FLASH_PROTECT_ALL;
static int fail_erase, fail_open, fail_write, fail_restore;
static unsigned erases, sleeps, writes, protection_reads;
flash_protect_type_t bk_flash_get_protect_type(void) {
  ++protection_reads;
  return protect;
}
int bk_flash_set_protect_type(flash_protect_type_t value) {
  if ((value == FLASH_PROTECT_NONE && fail_open) ||
      (value == FLASH_PROTECT_ALL && fail_restore))
    return -1;
  protect = value;
  return 0;
}
int bk_flash_read_bytes(uint32_t address, void* out, uint32_t size) {
  assert(address >= BASE && size <= sizeof(storage) - (address - BASE));
  memcpy(out, storage + (address - BASE), size);
  return 0;
}
int bk_flash_write_bytes(uint32_t address, const void* data, uint32_t size) {
  assert(protect == FLASH_PROTECT_NONE && address >= BASE &&
         size <= sizeof(storage) - (address - BASE));
  ++writes;
  if (fail_write) return -1;
  const uint8_t* p = data;
  for (uint32_t i = 0; i < size; ++i) storage[address - BASE + i] &= p[i];
  return 0;
}
int bk_flash_erase_sector(uint32_t address) {
  assert(protect == FLASH_PROTECT_NONE && address >= BASE &&
         address - BASE <= 4096);
  ++erases;
  if (fail_erase) return -1;
  memset(storage + (address - BASE), 255, 4096);
  return 0;
}
void rtos_delay_milliseconds(uint32_t ms) {
  assert(ms == 10);
  ++sleeps;
}
static void test_protection_restore(const h2_pal_disk_api_t* api) {
  const uint8_t data = 0x5a;
  for (unsigned erase = 0u; erase < 2u; ++erase) {
    for (unsigned native_failure = 0u; native_failure < 2u; ++native_failure) {
      fail_erase = fail_write = (int)native_failure;
      fail_restore = 1;
      const unsigned reads_before = protection_reads;
      int rc = erase ? h2_pal_disk_erase(api, 7, 0, 4096)
                     : h2_pal_disk_write(api, 7, 0, &data, sizeof(data));
      assert(rc == H2_PAL_ERR_IO && protect == FLASH_PROTECT_NONE);
      assert(protection_reads == reads_before + 1u);
      const unsigned writes_before = writes, erases_before = erases;
      assert(h2_pal_disk_flush(api, 7) == H2_PAL_ERR_IO);
      assert(h2_pal_disk_flush(api, 8) == H2_PAL_ERR_NOT_FOUND);
      assert(h2_pal_disk_write(api, 7, 0, &data, sizeof(data)) ==
             H2_PAL_ERR_IO);
      assert(h2_pal_disk_erase(api, 7, 0, 4096) == H2_PAL_ERR_IO);
      assert(writes == writes_before && erases == erases_before &&
             protection_reads == reads_before + 1u);
      fail_restore = fail_erase = fail_write = 0;
      if (erase) {
        assert(h2_pal_disk_flush(api, 7) == H2_PAL_OK);
        assert(protect == FLASH_PROTECT_ALL &&
               protection_reads == reads_before + 1u);
      }
      /* A later mutation must restore before capturing protection again. */
      assert(h2_pal_disk_write(api, 7, 0, &data, sizeof(data)) == H2_PAL_OK);
      assert(protect == FLASH_PROTECT_ALL &&
             protection_reads == reads_before + 2u);
      assert(h2_pal_disk_flush(api, 7) == H2_PAL_OK);
    }
  }
  fail_open = fail_restore = 1;
  const unsigned writes_before = writes;
  assert(h2_pal_disk_write(api, 7, 0, &data, sizeof(data)) == H2_PAL_ERR_IO);
  assert(writes == writes_before && protect == FLASH_PROTECT_ALL);
  assert(h2_pal_disk_flush(api, 7) == H2_PAL_ERR_IO);
  fail_open = fail_restore = 0;
  assert(h2_pal_disk_flush(api, 7) == H2_PAL_OK);
  assert(protect == FLASH_PROTECT_ALL);
  protect = FLASH_PROTECT_NONE;
  assert(h2_pal_disk_write(api, 7, 0, &data, sizeof(data)) == H2_PAL_OK);
  assert(h2_pal_disk_flush(api, 7) == H2_PAL_OK);
  assert(protect == FLASH_PROTECT_NONE);
}

int main(void) {
  const h2_pal_disk_api_t* api = h2_bk_platform_disk_api();
  h2_pal_disk_partition_t partition;
  assert(h2_pal_disk_get_partition(api, 7, &partition) == H2_PAL_ERR_NOT_FOUND);
  const h2_bk_disk_region_t region = {
      .id = 7, .offset = BASE, .size = sizeof(storage), .name = "dump"};
  assert(h2_bk_platform_disk_configure(&region) == 0);
  assert(h2_bk_platform_disk_configure(&region) == 0);
  assert(h2_pal_disk_get_partition(api, 7, &partition) == 0 &&
         partition.size == sizeof(storage) &&
         partition.erase_block_size == 4096 && !strcmp(partition.name, "dump"));
  assert(h2_pal_disk_erase(api, 7, 0, sizeof(storage)) == 0 && erases == 2 &&
         sleeps == 2);
  assert(protect == FLASH_PROTECT_ALL);
  const uint8_t evidence[] = {32, 0, 0, 0, 'H', '2'};
  uint8_t readback[sizeof(evidence)];
  assert(h2_pal_disk_write(api, 7, 4094, evidence, sizeof(evidence)) == 0);
  assert(h2_pal_disk_read(api, 7, 4094, readback, sizeof(readback)) == 0 &&
         !memcmp(evidence, readback, sizeof(evidence)) &&
         protect == FLASH_PROTECT_ALL);
  assert(h2_pal_disk_erase(api, 7, 1, 4096) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_disk_erase(api, 8, 0, 4096) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_disk_write(api, 7, UINT64_MAX, evidence, sizeof(evidence)) ==
         H2_PAL_ERR_INVALID_ARG);
  fail_open = 1;
  assert(h2_pal_disk_write(api, 7, 0, evidence, sizeof(evidence)) ==
         H2_PAL_ERR_IO);
  assert(protect == FLASH_PROTECT_ALL);
  fail_open = 0;
  fail_erase = 1;
  assert(h2_pal_disk_erase(api, 7, 0, 4096) == H2_PAL_ERR_IO &&
         protect == FLASH_PROTECT_ALL);
  fail_erase = 0;
  test_protection_restore(api);
  return 0;
}
