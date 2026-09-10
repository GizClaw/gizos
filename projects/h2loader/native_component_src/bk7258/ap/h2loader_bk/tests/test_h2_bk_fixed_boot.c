#include "h2_bk_fixed_boot_test_sdk.h"
#include "layout.h"
#include <assert.h>
#include <stddef.h>
#include <string.h>

/* NOR model: erase sets a 4 KiB sector to 0xff, a write can only clear bits. */
#define SECTOR 4096u
#define NATIVE_CONTROL 0x0075f000u
static uint8_t native_sector[SECTOR], request_sector[SECTOR];
static unsigned native_erases, request_erases;
static bk_logic_partition_t cp = {H2_FIXED_LOADER_OFFSET, H2_FIXED_CP_SIZE};
static bk_logic_partition_t ap = {H2_FIXED_LOADER_OFFSET + H2_FIXED_CP_SIZE,
                                  H2_FIXED_LOADER_AP_SIZE};
static bk_logic_partition_t native = {NATIVE_CONTROL, SECTOR};

static uint8_t *sector_at(uint32_t address, uint32_t size) {
  if (address >= NATIVE_CONTROL && address + size <= NATIVE_CONTROL + SECTOR)
    return native_sector + (address - NATIVE_CONTROL);
  if (address >= H2_FIXED_CONTROL_OFFSET &&
      address + size <= H2_FIXED_CONTROL_OFFSET + SECTOR)
    return request_sector + (address - H2_FIXED_CONTROL_OFFSET);
  assert(!"flash access outside modeled sectors");
  return NULL;
}
const bk_logic_partition_t *bk_flash_partition_get_info(bk_partition_t id) {
  return id == BK_PARTITION_APPLICATION    ? &cp
         : id == BK_PARTITION_APPLICATION1 ? &ap
                                           : &native;
}
int bk_flash_read_bytes(uint32_t address, uint8_t *out, uint32_t size) {
  memcpy(out, sector_at(address, size), size);
  return BK_OK;
}
int bk_flash_write_bytes(uint32_t address, const uint8_t *in, uint32_t size) {
  uint8_t *cell = sector_at(address, size);
  for (uint32_t i = 0; i < size; ++i) cell[i] &= in[i];
  return BK_OK;
}
int bk_flash_erase_sector(uint32_t address) {
  memset(sector_at(address, SECTOR), 0xff, SECTOR);
  if (address == NATIVE_CONTROL) ++native_erases;
  else ++request_erases;
  return BK_OK;
}
int bk_flash_set_protect_type(flash_protect_type_t type) { (void)type; return BK_OK; }
flash_protect_type_t bk_flash_get_protect_type(void) { return FLASH_PROTECT_ALL; }
uint8_t bk_ota_get_current_partition(void) { return 0u; }

/* Mirrors h2loader_cp_try_fixed_app: returns whether CP enters App. */
static int cp_boot(void) {
  h2_fixed_boot_request_t r;
  uint32_t zero = 0u;
  bk_flash_read_bytes(H2_FIXED_CONTROL_OFFSET, (uint8_t *)&r, sizeof(r));
  if (!h2_fixed_request_boots_app(&r)) return 0;
  bk_flash_write_bytes(H2_FIXED_CONTROL_OFFSET, (const uint8_t *)&zero, sizeof(zero));
  return 1;
}

static void test_confirmed_app_boots_without_writes(void) {
  static const uint32_t loader_flags[3] = {0u, 0u, 3u};
  memset(request_sector, 0xff, sizeof(request_sector));
  memset(native_sector, 0xff, sizeof(native_sector));
  assert(h2_bk_fixed_layout_active());
  assert(h2_bk_fixed_current_slot() == 0u);
  assert(!h2_bk_fixed_next_app() && !h2_bk_fixed_app_failed());

  /* Selection migrates the SDK flags to A/A/A once and arms one trial. */
  assert(h2_bk_fixed_select(H2_BK_H2LOADER_APP_PARTITION_ID) == H2_PAL_OK);
  assert(memcmp(native_sector, loader_flags, sizeof(loader_flags)) == 0);
  assert(native_erases == 1u);
  assert(cp_boot());
  assert(h2_bk_fixed_app_failed());
  assert(h2_bk_fixed_confirm_app() == H2_PAL_OK);
  assert(!h2_bk_fixed_app_failed() && h2_bk_fixed_next_app());

  /* Later resets enter App directly; rewriting zero bits changes nothing. */
  uint8_t before[sizeof(request_sector)];
  unsigned erases = request_erases;
  memcpy(before, request_sector, sizeof(before));
  for (unsigned boot = 0; boot < 3; ++boot) {
    assert(cp_boot());
    assert(h2_bk_fixed_confirm_app() == H2_PAL_OK);
  }
  assert(memcmp(before, request_sector, sizeof(before)) == 0);
  assert(request_erases == erases && native_erases == 1u);

  /* Loader only runs over a confirmed record if CP rejected App: selecting
   * App again arms a fresh trial so the rejection becomes a failed attempt. */
  assert(h2_bk_fixed_select(H2_BK_H2LOADER_APP_PARTITION_ID) == H2_PAL_OK);
  assert(request_erases == erases + 1u);
  assert(cp_boot() && h2_bk_fixed_app_failed());
}

static void test_unconfirmed_attempt_and_loader_selection(void) {
  memset(request_sector, 0xff, sizeof(request_sector));
  assert(h2_bk_fixed_select(H2_BK_H2LOADER_APP_PARTITION_ID) == H2_PAL_OK);
  /* Re-selecting a pending request does not erase it again. */
  unsigned erases = request_erases;
  assert(h2_bk_fixed_select(H2_BK_H2LOADER_APP_PARTITION_ID) == H2_PAL_OK);
  assert(request_erases == erases);
  assert(cp_boot());
  /* Reset before confirmation: CP returns to Loader, which sees the failure. */
  assert(!cp_boot() && h2_bk_fixed_app_failed() && !h2_bk_fixed_next_app());
  assert(h2_bk_fixed_select(H2_BK_H2LOADER_PRIMARY_PARTITION_ID) == H2_PAL_OK);
  assert(!h2_bk_fixed_app_failed() && !h2_bk_fixed_next_app());
  erases = request_erases;
  assert(h2_bk_fixed_select(H2_BK_H2LOADER_PRIMARY_PARTITION_ID) == H2_PAL_OK);
  assert(request_erases == erases);
  assert(h2_bk_fixed_confirm_app() == H2_PAL_OK);
  assert(!h2_bk_fixed_app_failed() && !cp_boot());
  assert(h2_bk_fixed_select(3u) == H2_PAL_ERR_INVALID_ARG);
}

static void test_invalidate_before_app_write(void) {
  memset(request_sector, 0xff, sizeof(request_sector));
  assert(h2_bk_fixed_select(H2_BK_H2LOADER_APP_PARTITION_ID) == H2_PAL_OK);
  assert(cp_boot() && h2_bk_fixed_confirm_app() == H2_PAL_OK);
  assert(h2_bk_fixed_invalidate_app() == H2_PAL_OK);
  assert(!cp_boot() && !h2_bk_fixed_app_failed());
}

static void test_torn_request_is_not_bootable(void) {
  h2_fixed_boot_request_t r;
  memset(request_sector, 0xff, sizeof(request_sector));
  assert(h2_bk_fixed_select(H2_BK_H2LOADER_APP_PARTITION_ID) == H2_PAL_OK);
  memcpy(&r, request_sector, sizeof(r));
  /* Reset before magic was published: body present, magic still erased. */
  request_sector[0] = request_sector[1] = request_sector[2] = request_sector[3] = 0xff;
  assert(!h2_bk_fixed_next_app() && !h2_bk_fixed_app_failed());
  memcpy(request_sector, &r, sizeof(r));
  request_sector[offsetof(h2_fixed_boot_request_t, confirmed)] = 0u;
  assert(!h2_bk_fixed_next_app());
}

static void test_app_layout_slot(void) {
  cp.partition_start_addr = H2_FIXED_APP_OFFSET;
  ap.partition_length = H2_FIXED_APP_SIZE - H2_FIXED_CP_SIZE;
  assert(h2_bk_fixed_layout_active());
  assert(h2_bk_fixed_current_slot() == 1u);
  ap.partition_length = H2_FIXED_LOADER_AP_SIZE;
  assert(!h2_bk_fixed_layout_active());
}

int main(void) {
  test_confirmed_app_boots_without_writes();
  test_unconfirmed_attempt_and_loader_selection();
  test_invalidate_before_app_write();
  test_torn_request_is_not_bootable();
  test_app_layout_slot();
  return 0;
}
