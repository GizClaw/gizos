#include "h2_bk_h2loader_power_test_sdk.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
part_flag update_part_flag;
static uint8_t data[9];
static int fail, missing;
static uint8_t running;
static bk_logic_partition_t control = {123};
const bk_logic_partition_t *bk_flash_partition_get_info(int x) {
  (void)x;
  return missing ? NULL : &control;
}
int bk_flash_read_bytes(uint32_t addr, uint8_t *out, size_t n) {
  assert(addr == 123 && n == 9);
  memcpy(out, data, n);
  return fail ? -1 : 0;
}
uint8_t bk_ota_get_current_partition(void) { return running; }
void bk_wdt_force_reboot(void) {}
int h2_bk_h2loader_commit_staged_app_boot(void) {
  return H2_PAL_ERR_INVALID_STATE;
}
int h2_bk_h2loader_select_confirmed_boot_partition(uint32_t id) {
  data[0] = data[4] = id - 1;
  data[8] = id + 2;
  return 0;
}
static h2_pal_result_t collect_app(void *user,
                                 const h2_pal_power_boot_partition_t *partition) {
  if (partition->id == 2u) *(uint32_t *)user = partition->flags;
  return H2_PAL_OK;
}

static void test_rollback_bootability(const h2_pal_power_api_t *api) {
  uint32_t flags = 0u;
  running = EXEX_A_PART;
  data[0] = EXEX_A_PART;
  data[4] = EXEC_B_PART;
  data[8] = CONFIRM_EXEC_A;
  assert(h2_pal_power_list_boot_partitions(api, collect_app, &flags) == 0);
  assert(flags & H2_PAL_POWER_BOOT_PARTITION_FLAG_APP);
  assert(!(flags & H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE));
  /* Before the reset the running candidate remains addressable. */
  running = EXEC_B_PART;
  assert(h2_pal_power_list_boot_partitions(api, collect_app, &flags) == 0);
  assert(flags & H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE);
  running = EXEX_A_PART;
  /* A freshly staged candidate has not failed a trial. */
  data[8] = 1u;
  assert(h2_pal_power_list_boot_partitions(api, collect_app, &flags) == 0);
  assert(flags & H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE);
  /* Explicit Loader selection clears the attempted-candidate selection. */
  assert(h2_pal_power_set_next_boot_partition(api, 1u) == 0);
  assert(h2_pal_power_list_boot_partitions(api, collect_app, &flags) == 0);
  assert(flags & H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE);
  data[0] = data[4] = EXEC_B_PART;
  data[8] = CONFIRM_EXEC_B;
  assert(h2_pal_power_list_boot_partitions(api, collect_app, &flags) == 0);
  assert(flags & H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE);
  fail = 1;
  assert(h2_pal_power_list_boot_partitions(api, collect_app, &flags) == H2_PAL_ERR_IO);
  fail = 0;
}

int main(void) {
  const h2_pal_power_api_t *api = h2_bk_h2loader_power_api();
  test_rollback_bootability(api);
  h2_pal_power_boot_partition_t out;
  assert(h2_pal_power_get_next_boot_partition(api, NULL) ==
         H2_PAL_ERR_INVALID_ARG);
  memset(data, 255, 9);
  running = 0;
  assert(h2_pal_power_get_next_boot_partition(api, &out) == 0 && out.id == 1);
  assert(out.flags & H2_PAL_POWER_BOOT_PARTITION_FLAG_NEXT);
  assert(!(out.flags & H2_PAL_POWER_BOOT_PARTITION_FLAG_RUNNING));
  running = 1;
  assert(h2_pal_power_get_next_boot_partition(api, &out) == 0 && out.id == 2);
  for (unsigned id = 1; id <= 2; id++) {
    assert(h2_pal_power_set_next_boot_partition(api, id) == 0);
    assert(h2_pal_power_get_next_boot_partition(api, &out) == 0 &&
           out.id == id);
  }
  data[0] = 0;
  data[4] = 1;
  data[8] = 1;
  assert(h2_pal_power_get_next_boot_partition(api, &out) == 0 && out.id == 2);
  data[0] = 1;
  data[4] = 0;
  assert(h2_pal_power_get_next_boot_partition(api, &out) == 0 && out.id == 1);
  data[4] = 99;
  assert(h2_pal_power_get_next_boot_partition(api, &out) ==
         H2_PAL_ERR_INVALID_STATE);
  fail = 1;
  assert(h2_pal_power_get_next_boot_partition(api, &out) == H2_PAL_ERR_IO);
  fail = 0;
  missing = 1;
  assert(h2_pal_power_get_next_boot_partition(api, &out) == H2_PAL_ERR_IO);
  puts("power provider tests PASS");
  return 0;
}
