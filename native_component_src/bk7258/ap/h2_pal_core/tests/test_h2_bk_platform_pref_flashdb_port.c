#include "driver/flash.h"
#include "driver/flash_partition.h"
#include "flashdb.h"
#include "os/os.h"
#include "os/mem.h"

#include <assert.h>
#include <limits.h>
#include <string.h>

#define STORAGE_SIZE 8192u
static uint8_t storage[STORAGE_SIZE];
static unsigned reads;
static uint32_t last_offset, last_size;
static int fail_read, fail_write_after_commit, fail_erase_after_commit;
static int fail_lock, fail_alloc;
static unsigned allocations;
static _Alignas(4) uint8_t cache_storage[1024];
static unsigned lock_depth;
static flash_protect_type_t protect = FLASH_PROTECT_ALL;
static const bk_logic_partition_t partition = {
    CONFIG_FLASHDB_KVDB_START_ADDR, CONFIG_FLASHDB_KVDB_SIZE};
static int mutex_token;

void *psram_malloc(size_t size) {
  assert(lock_depth == 1u && size <= sizeof(cache_storage));
  ++allocations;
  return fail_alloc ? NULL : cache_storage;
}

const bk_logic_partition_t *bk_flash_partition_get_info(int id) {
  assert(id == BK_PARTITION_FLASHDB);
  return &partition;
}
int rtos_init_mutex(beken_mutex_t *out) { *out = &mutex_token; return kNoErr; }
int rtos_lock_mutex(beken_mutex_t *mutex) {
  assert(*mutex == &mutex_token && lock_depth == 0u);
  if (fail_lock) return -1;
  ++lock_depth;
  return kNoErr;
}
int rtos_unlock_mutex(beken_mutex_t *mutex) {
  assert(*mutex == &mutex_token && lock_depth == 1u);
  --lock_depth;
  return kNoErr;
}
bk_err_t bk_flash_read_bytes(uint32_t offset, uint8_t *buffer, uint32_t size) {
  assert(lock_depth == 1u);
  ++reads; last_offset = offset; last_size = size;
  if (fail_read || offset > STORAGE_SIZE || size > STORAGE_SIZE - offset)
    return -1;
  memcpy(buffer, storage + offset, size);
  return BK_OK;
}
bk_err_t bk_flash_write_bytes(uint32_t offset, const uint8_t *data, uint32_t size) {
  assert(lock_depth == 1u && offset <= STORAGE_SIZE && size <= STORAGE_SIZE - offset);
  memcpy(storage + offset, data, size);
  return fail_write_after_commit ? -1 : BK_OK;
}
bk_err_t bk_flash_erase_sector(uint32_t offset) {
  assert(lock_depth == 1u && protect == FLASH_PROTECT_NONE);
  assert(offset <= STORAGE_SIZE - 4096u);
  memset(storage + offset, 0xff, 4096u);
  return fail_erase_after_commit ? -1 : BK_OK;
}
flash_protect_type_t bk_flash_get_protect_type(void) { return protect; }
void bk_flash_set_protect_type(flash_protect_type_t type) { protect = type; }

static void reset(void) {
  for (unsigned i = 0; i < STORAGE_SIZE; ++i) storage[i] = (uint8_t)i;
  fail_read = fail_write_after_commit = fail_erase_after_commit = fail_lock = 0;
  reads = 0u;
  assert(g_flashdb0.ops.init() == 0 && lock_depth == 0u);
}
static void test_allocation_failure_keeps_real_uncached_reads_available(void) {
  fail_alloc = 1;
  assert(g_flashdb0.ops.init() == 0);
  uint8_t data[32];
  for (unsigned i = 0; i < STORAGE_SIZE; ++i) storage[i] = (uint8_t)i;
  assert(g_flashdb0.ops.read(32, data, sizeof(data)) == 32);
  assert(g_flashdb0.ops.read(64, data, sizeof(data)) == 32);
  assert(memcmp(data, storage + 64, sizeof(data)) == 0 && reads == 2u);
  fail_alloc = 0;
}
static void test_adjacent_crc_reads_share_one_physical_read(void) {
  reset();
  uint8_t data[32];
  assert(g_flashdb0.ops.read(32, data, sizeof(data)) == 32);
  assert(memcmp(data, storage + 32, sizeof(data)) == 0);
  assert(g_flashdb0.ops.read(64, data, sizeof(data)) == 32);
  assert(memcmp(data, storage + 64, sizeof(data)) == 0);
  assert(reads == 1u);
}
static void test_readahead_stays_inside_partition_and_large_reads_pass_through(void) {
  reset();
  uint8_t data[1024];
  assert(g_flashdb0.ops.read(4090, data, 6u) == 6);
  assert(last_offset >= CONFIG_FLASHDB_KVDB_START_ADDR);
  assert(last_offset + last_size <= CONFIG_FLASHDB_KVDB_START_ADDR + CONFIG_FLASHDB_KVDB_SIZE);
  assert(memcmp(data, storage + 4090, 6u) == 0);
  assert(g_flashdb0.ops.read(256, data, sizeof(data)) == (int)sizeof(data));
  assert(last_offset == 256u && last_size == sizeof(data));
  assert(memcmp(data, storage + 256, sizeof(data)) == 0);
}
static void test_cross_line_reads_pass_through_without_readahead(void) {
  reset(); uint8_t data[32];
  assert(g_flashdb0.ops.read(500, data, sizeof(data)) == (int)sizeof(data));
  assert(last_offset == 500u && last_size == sizeof(data));
  assert(memcmp(data, storage + 500, sizeof(data)) == 0);
}
static void test_write_invalidates_even_when_driver_reports_failure_after_commit(void) {
  for (unsigned fail = 0u; fail < 2u; ++fail) {
    reset();
    uint8_t data[4], changed[4] = {0xaa, 0xbb, 0xcc, 0xdd};
    assert(g_flashdb0.ops.read(16, data, 4u) == 4);
    fail_write_after_commit = (int)fail;
    assert(g_flashdb0.ops.write(16, changed, 4u) == (fail ? -1 : 4));
    assert(g_flashdb0.ops.read(16, data, 4u) == 4);
    assert(memcmp(data, changed, 4u) == 0 && reads == 2u);
  }
}
static void test_erase_invalidates_even_after_uncertain_commit_and_restores_protection(void) {
  for (unsigned fail = 0u; fail < 2u; ++fail) {
    reset(); uint8_t data[4];
    assert(g_flashdb0.ops.read(16, data, 4u) == 4);
    fail_erase_after_commit = (int)fail;
    assert(g_flashdb0.ops.erase(0, 4096u) == (fail ? -1 : 4096));
    assert(protect == FLASH_PROTECT_ALL);
    assert(g_flashdb0.ops.read(16, data, 4u) == 4);
    for (unsigned i = 0u; i < sizeof(data); ++i) assert(data[i] == 0xff);
    assert(reads == 2u);
  }
}
static void test_read_failure_does_not_publish_a_cache_line(void) {
  reset(); uint8_t data[4];
  fail_read = 1;
  assert(g_flashdb0.ops.read(16, data, 4u) == -1);
  fail_read = 0;
  assert(g_flashdb0.ops.read(16, data, 4u) == 4);
  assert(memcmp(data, storage + 16, 4u) == 0 && reads == 2u);
}
static void test_lock_failure_does_not_read_or_mutate_storage(void) {
  reset(); uint8_t data[4] = {1, 2, 3, 4};
  fail_lock = 1;
  assert(g_flashdb0.ops.read(16, data, 4u) == -1);
  assert(g_flashdb0.ops.write(16, data, 4u) == -1);
  assert(g_flashdb0.ops.erase(0, 4096u) == -1);
  assert(reads == 0u && storage[16] == 16u && protect == FLASH_PROTECT_ALL);
  fail_lock = 0;
  assert(g_flashdb0.ops.read(16, data, 4u) == 4);
}
int main(void) {
  test_allocation_failure_keeps_real_uncached_reads_available();
  test_adjacent_crc_reads_share_one_physical_read();
  test_readahead_stays_inside_partition_and_large_reads_pass_through();
  test_cross_line_reads_pass_through_without_readahead();
  test_write_invalidates_even_when_driver_reports_failure_after_commit();
  test_erase_invalidates_even_after_uncertain_commit_and_restores_protection();
  test_read_failure_does_not_publish_a_cache_line();
  test_lock_failure_does_not_read_or_mutate_storage();
  assert(lock_depth == 0u && allocations == 2u);
  return 0;
}
