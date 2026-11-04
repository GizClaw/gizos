#include "h2_bk_h2loader_power_test_sdk.h"
#include "h2_bk_platform_image.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t data[12];
static int fail, missing, feed_error;
static unsigned releases, reboots, feeds, sdk_confirms, flag_erases;
static unsigned aes_state, native_execution, staged_partition, sdk_commits;
static int sdk_commit_error;
part_flag update_part_flag;
static uint8_t running;
static bk_logic_partition_t control = {123};
const bk_logic_partition_t* bk_flash_partition_get_info(int x) {
  (void)x;
  return missing ? NULL : &control;
}
int bk_flash_read_bytes(uint32_t addr, uint8_t* out, size_t n) {
  assert(addr == 123 && n <= sizeof(data));
  memcpy(out, data, n);
  return fail ? -1 : 0;
}
uint8_t bk_ota_get_current_partition(void) { return running; }
void h2_bk_platform_sd_fs_release(void) { ++releases; }
void bk_wdt_force_reboot(void) {
  assert(releases == reboots + 1);
  ++reboots;
}
int bk_wdt_feed(void) {
  ++feeds;
  return feed_error;
}
void bk_ota_double_check_for_execution(void) {
  ++sdk_confirms;
  /* Model only the SDK confirming a running temporary B image. */
  if (running == EXEC_B_PART) {
    data[0] = data[4] = EXEC_B_PART;
    data[8] = CONFIRM_EXEC_B;
  }
}
void bk_ota_confirm_update_partition(unsigned flag) {
  assert(flag == (running ? CONFIRM_EXEC_B : CONFIRM_EXEC_A));
  bk_ota_double_check_for_execution();
}
int bk_flash_write_bytes(uint32_t addr, const uint8_t* bytes, size_t n) {
  assert(addr == 123 && n == 12);
  memcpy(data, bytes, n);
  return fail ? -1 : 0;
}
int bk_flash_erase_sector(uint32_t addr) {
  assert(addr == 123);
  ++flag_erases;
  memset(data, 255, sizeof(data));
  return fail ? -1 : 0;
}
int bk_flash_set_protect_type(flash_protect_type_t t) {
  (void)t;
  return 0;
}
flash_protect_type_t bk_flash_get_protect_type(void) {
  return FLASH_PROTECT_ALL;
}
int h2_bk_image_write_active(void) { return 0; }
int h2_bk_image_prepared(uint32_t id) { return staged_partition == id; }
void h2_bk_image_selected(uint32_t id) {
  if (staged_partition == id) staged_partition = 0;
}
int bk_ota_update_partition_flag(int result) {
  assert(result == 0 && update_part_flag == UPDATE_B_PART);
  ++sdk_commits;
  if (sdk_commit_error) return -1;
  memset(data, 255, sizeof(data));
  data[0] = data[4] = EXEC_B_PART;
  data[8] = 1;
  return 0;
}
int h2_bk_image_publish_native_footer(uint32_t id) {
  (void)id;
  return 0;
}
int h2_bk_image_execution_partition(uint32_t id, uint32_t* out) {
  *out = native_execution ? native_execution : id;
  return 0;
}
unsigned h2_bk_flash_aes_state(void) { return aes_state; }
static h2_pal_result_t collect_app(
    void* user, const h2_pal_power_boot_partition_t* partition) {
  if (partition->id == 2u) *(uint32_t*)user = partition->flags;
  return H2_PAL_OK;
}

static void test_rollback_bootability(const h2_pal_power_api_t* api) {
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
  assert(h2_pal_power_list_boot_partitions(api, collect_app, &flags) ==
         H2_PAL_ERR_IO);
  fail = 0;
}

int g_fixed_active, g_fixed_app_failed, g_fixed_relay_failed;
uint8_t g_fixed_slot;

/* Fixed layout: a trial App that never confirmed, or a candidate Loader the
 * ROM bootloader rolled back from native B, leaves Partition 2 not bootable
 * while Loader runs, which stops the Loader relaunching it. */
static void test_fixed_layout_rollback_bootability(
    const h2_pal_power_api_t* api) {
  uint32_t flags = 0u;
  memset(data, 255, sizeof(data));
  g_fixed_active = 1;
  g_fixed_slot = 0u;
  assert(h2_pal_power_list_boot_partitions(api, collect_app, &flags) == 0);
  assert(flags & H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE);
  g_fixed_relay_failed = 1;
  assert(h2_pal_power_list_boot_partitions(api, collect_app, &flags) == 0);
  assert(flags & H2_PAL_POWER_BOOT_PARTITION_FLAG_APP);
  assert(!(flags & H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE));
  g_fixed_relay_failed = 0;
  g_fixed_app_failed = 1;
  assert(h2_pal_power_list_boot_partitions(api, collect_app, &flags) == 0);
  assert(!(flags & H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE));
  /* Running from Partition 2 itself, the evidence does not apply. */
  g_fixed_relay_failed = 1;
  g_fixed_slot = 1u;
  assert(h2_pal_power_list_boot_partitions(api, collect_app, &flags) == 0);
  assert(flags & H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE);
  g_fixed_active = g_fixed_app_failed = g_fixed_relay_failed = 0;
  g_fixed_slot = 0u;
}

int main(void) {
  const h2_pal_power_api_t* api = h2_bk_platform_power_api();
  test_rollback_bootability(api);
  test_fixed_layout_rollback_bootability(api);
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
  missing = 0;
  running = 1;
  h2_pal_power_native_evidence_t evidence;
  h2_pal_power_boot_request_t request = {
      .partition_id = 2, .policy = H2_PAL_POWER_BOOT_POLICY_TRIAL};
  assert(h2_pal_power_prepare_next_boot_partition(api, &request, &evidence) ==
         0);
  assert(!evidence.format && !evidence.size);
  assert(h2_pal_power_set_next_boot_partition(api, 2) == 0 && data[8] == 1);
  h2_pal_power_image_state_t state;
  assert(h2_pal_power_get_image_state(api, 2, &state) == 0 &&
         state.state == H2_PAL_POWER_IMAGE_STATE_PENDING_VERIFY);
  assert(h2_bk_platform_boot_startup() == 0 && data[0] == 0 && data[4] == 1 &&
         data[8] == 3);
  h2_pal_power_image_confirmation_t confirmation;
  assert(h2_pal_power_confirm_running_image(api, NULL, &confirmation) == 0 &&
         confirmation.partition_id == 2 && confirmation.was_pending &&
         data[8] == 4);
  unsigned confirmed_erases = flag_erases;
  assert(h2_pal_power_confirm_running_image(api, NULL, &confirmation) == 0 &&
         !confirmation.was_pending);
  assert(sdk_confirms == 2 && flag_erases == confirmed_erases);
  /* Returning to the installed canonical window never engages the remap. */
  aes_state = H2_BK_FLASH_AES_UNKNOWN;
  native_execution = 1;
  request.partition_id = 1;
  request.policy = H2_PAL_POWER_BOOT_POLICY_EXISTING;
  g_fixed_active = 0;
  assert(h2_pal_power_prepare_next_boot_partition(api, &request, &evidence) ==
         0);
  g_fixed_active = 1;
  assert(h2_pal_power_prepare_next_boot_partition(api, &request, &evidence) ==
         0);
  /* Only fixed native B selection of a primary-linked image needs AES off. */
  request.partition_id = 2;
  request.policy = H2_PAL_POWER_BOOT_POLICY_TRIAL;
  assert(h2_pal_power_prepare_next_boot_partition(api, &request, &evidence) ==
         H2_PAL_ERR_UNSUPPORTED);
  native_execution = 2;
  assert(h2_pal_power_prepare_next_boot_partition(api, &request, &evidence) ==
         0);
  g_fixed_active = 0;
  native_execution = 1;
  assert(h2_pal_power_prepare_next_boot_partition(api, &request, &evidence) ==
         0);
  native_execution = 0;
  aes_state = H2_BK_FLASH_AES_OFF;
  h2_pal_power_capabilities_t capabilities;
  assert(h2_pal_power_get_capabilities(api, &capabilities) == 0 &&
         (capabilities.flags & H2_PAL_POWER_CAPABILITY_WATCHDOG_FEED));
  assert(h2_pal_power_feed_watchdog(api) == 0 && feeds == 1);
  feed_error = -1;
  assert(h2_pal_power_feed_watchdog(api) == H2_PAL_ERR_IO && feeds == 2);
  assert(h2_pal_power_reboot(api, 0) == 0 && releases == 1 && reboots == 1);
  /* Legacy SDK A/B: every B boot is a trial until that boot confirms. */
  memset(data, 255, sizeof(data));
  data[0] = data[4] = EXEC_B_PART;
  data[8] = CONFIRM_EXEC_B;
  running = EXEC_B_PART;
  assert(h2_bk_platform_boot_startup() == 0 && data[0] == EXEX_A_PART &&
         data[4] == EXEC_B_PART && data[8] == CONFIRM_EXEC_A);
  unsigned erases = flag_erases;
  assert(h2_bk_platform_boot_startup() == 0 && flag_erases == erases);
  assert(h2_pal_power_get_image_state(api, 2, &state) == 0 &&
         state.state == H2_PAL_POWER_IMAGE_STATE_PENDING_VERIFY);
  assert(h2_pal_power_confirm_running_image(api, NULL, &confirmation) == 0 &&
         confirmation.partition_id == 2 && confirmation.was_pending &&
         data[0] == EXEC_B_PART && data[4] == EXEC_B_PART &&
         data[8] == CONFIRM_EXEC_B);
  /* Confirming Loader A is the SDK operation alone and keeps the
   * A/B/confirmed-A evidence of an unconfirmed B attempt. */
  running = EXEX_A_PART;
  data[0] = EXEX_A_PART;
  data[8] = CONFIRM_EXEC_A;
  erases = flag_erases;
  const unsigned confirms = sdk_confirms;
  assert(h2_pal_power_confirm_running_image(api, NULL, &confirmation) == 0 &&
         confirmation.partition_id == 1 && !confirmation.was_pending);
  assert(sdk_confirms == confirms + 1 && flag_erases == erases &&
         data[0] == EXEX_A_PART && data[4] == EXEC_B_PART &&
         data[8] == CONFIRM_EXEC_A);
  uint32_t app_flags = 0u;
  assert(h2_pal_power_list_boot_partitions(api, collect_app, &app_flags) == 0 &&
         (app_flags & H2_PAL_POWER_BOOT_PARTITION_FLAG_APP) &&
         !(app_flags & H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE));
  /* Fresh legacy B commits through the SDK, retains ready on failure, and
   * consumes it only after success. The SDK update global follows selection. */
  staged_partition = 2;
  sdk_commit_error = 1;
  const unsigned before_feeds = feeds;
  assert(h2_pal_power_set_next_boot_partition(api, 2) == H2_PAL_ERR_IO &&
         staged_partition == 2 && sdk_commits == 1 &&
         update_part_flag == UPDATE_B_PART);
  sdk_commit_error = 0;
  assert(h2_pal_power_set_next_boot_partition(api, 2) == 0 &&
         !staged_partition && sdk_commits == 2 && data[8] == 1);
  assert(feeds == before_feeds + 4);
  assert(h2_pal_power_set_next_boot_partition(api, 1) == 0 &&
         update_part_flag == UPDATE_A_PART && sdk_commits == 2);
  puts("power provider tests PASS");
  return 0;
}
