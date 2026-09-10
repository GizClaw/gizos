#include "h2_bk_fixed_boot_test_sdk.h"
#include "layout.h"
#include <assert.h>
#include <stddef.h>
#include <string.h>

/* NOR model: erase sets a 4 KiB sector to 0xff, a write can only clear bits. */
#define SECTOR 4096u
#define NATIVE_CONTROL 0x0075f000u
static uint8_t native_sector[SECTOR], request_sector[SECTOR], app_sector[SECTOR];
static uint8_t native_slot;
static unsigned native_erases, request_erases;
/* Board partition table (Loader image): Loader window 2380 KiB, App window
 * 5100 KiB, boot record sector at 0x77f000. */
#define LOADER_OFFSET 0x11000u
#define LOADER_CP (1156u * 1024u)
#define LOADER_SIZE (2380u * 1024u)
#define APP_OFFSET (LOADER_OFFSET + LOADER_SIZE)
#define APP_SIZE (5100u * 1024u)
#define CONTROL 0x77f000u
static bk_logic_partition_t cp = {LOADER_OFFSET, LOADER_CP};
static bk_logic_partition_t ap = {LOADER_OFFSET + LOADER_CP, LOADER_SIZE - LOADER_CP};
static bk_logic_partition_t s_app = {APP_OFFSET, APP_SIZE};
static bk_logic_partition_t control = {CONTROL, SECTOR};
static bk_logic_partition_t native = {NATIVE_CONTROL, SECTOR};

static uint8_t *sector_at(uint32_t address, uint32_t size) {
  if (address >= NATIVE_CONTROL && address + size <= NATIVE_CONTROL + SECTOR)
    return native_sector + (address - NATIVE_CONTROL);
  if (address >= control.partition_start_addr &&
      address + size <= control.partition_start_addr + SECTOR)
    return request_sector + (address - control.partition_start_addr);
  if (address >= s_app.partition_start_addr &&
      address + size <= s_app.partition_start_addr + SECTOR)
    return app_sector + (address - s_app.partition_start_addr);
  assert(!"flash access outside modeled sectors");
  return NULL;
}
const bk_logic_partition_t *bk_flash_partition_get_info(bk_partition_t id) {
  return id == BK_PARTITION_APPLICATION      ? &cp
         : id == BK_PARTITION_APPLICATION1   ? &ap
         : id == BK_PARTITION_S_APP          ? &s_app
         : id == BK_PARTITION_H2_BOOT_REQUEST ? &control
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
uint8_t bk_ota_get_current_partition(void) { return native_slot; }

/* Mirrors h2loader_cp_try_fixed_app: returns whether CP enters App. */
static int cp_boot(void) {
  h2_fixed_boot_request_t r;
  uint32_t zero = 0u;
  bk_flash_read_bytes(CONTROL, (uint8_t *)&r, sizeof(r));
  if (!h2_fixed_request_boots_app(&r, h2_bk_fixed_layout())) return 0;
  bk_flash_write_bytes(CONTROL, (const uint8_t *)&zero, sizeof(zero));
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

static const uint8_t relay_pending[12] = {1, 0xff, 0xff, 0xff, 1, 0xff,
                                          0xff, 0xff, 1, 0xff, 0xff, 0xff};
static const uint8_t relay_confirmed[12] = {1, 0xff, 0xff, 0xff, 1, 0xff,
                                            0xff, 0xff, 4, 0xff, 0xff, 0xff};

/* Stage a CP vector table in the App window whose reset handler was linked
 * for the window starting at offset. */
static void put_app_window_image(uint32_t offset) {
  const uint32_t words[2] = {0x28080000u, H2_FIXED_XIP_ADDRESS(offset) + 0x201u};
  memset(app_sector, 0xff, sizeof(app_sector));
  memcpy(app_sector, words, sizeof(words));
}

static void test_loader_relay_through_native_b(void) {
  static const uint32_t loader_flags[3] = {0u, 0u, 3u};
  memset(request_sector, 0xff, sizeof(request_sector));
  memset(native_sector, 0xff, sizeof(native_sector));
  native_slot = 0u;

  /* An App image in the App window keeps the CP handoff. */
  put_app_window_image(APP_OFFSET);
  assert(!h2_bk_fixed_app_window_holds_loader());
  assert(h2_bk_fixed_select(H2_BK_H2LOADER_APP_PARTITION_ID) == H2_PAL_OK);
  assert(h2_bk_fixed_next_app() && memcmp(native_sector, loader_flags, 12) == 0);

  /* A Loader image there is booted through native B with a pending confirm,
   * and the CP handoff record is cleared. */
  put_app_window_image(LOADER_OFFSET);
  assert(h2_bk_fixed_app_window_holds_loader());
  assert(h2_bk_fixed_select(H2_BK_H2LOADER_APP_PARTITION_ID) == H2_PAL_OK);
  assert(memcmp(native_sector, relay_pending, 12) == 0);
  assert(!cp_boot() && h2_bk_fixed_next_app() && !h2_bk_fixed_app_failed());

  /* The remapped Loader reports Partition 2 and confirms B. */
  native_slot = 1u;
  assert(h2_bk_fixed_current_slot() == 1u);
  assert(h2_bk_fixed_confirm_loader() == H2_PAL_OK);
  assert(memcmp(native_sector, relay_confirmed, 12) == 0);
  /* Re-selecting App during the copy keeps the confirmed B. */
  assert(h2_bk_fixed_select(H2_BK_H2LOADER_APP_PARTITION_ID) == H2_PAL_OK);
  assert(memcmp(native_sector, relay_confirmed, 12) == 0);

  /* After the copy, Loader selection returns native boot to A. */
  assert(h2_bk_fixed_select(H2_BK_H2LOADER_PRIMARY_PARTITION_ID) == H2_PAL_OK);
  assert(memcmp(native_sector, loader_flags, 12) == 0);
  assert(!h2_bk_fixed_next_app());
  native_slot = 0u;
  assert(h2_bk_fixed_current_slot() == 0u);
  assert(h2_bk_fixed_confirm_loader() == H2_PAL_OK);
  assert(memcmp(native_sector, loader_flags, 12) == 0);
}

static void test_app_layout_slot(void) {
  /* The App image's table swaps the roles: own window is App, s_app is the
   * Loader window. */
  cp = (bk_logic_partition_t){APP_OFFSET, LOADER_CP};
  ap = (bk_logic_partition_t){APP_OFFSET + LOADER_CP, APP_SIZE - LOADER_CP};
  s_app = (bk_logic_partition_t){LOADER_OFFSET, LOADER_SIZE};
  assert(h2_bk_fixed_layout_active());
  assert(h2_bk_fixed_current_slot() == 1u);
  assert(h2_bk_fixed_layout()->loader.size == LOADER_SIZE &&
         h2_bk_fixed_layout()->app.offset == APP_OFFSET);
  /* A table without matching windows is not a fixed layout. */
  ap.partition_start_addr += SECTOR;
  assert(!h2_bk_fixed_layout_active());
}

static void test_board_owned_loader_size(void) {
  /* A board with a 1904 KiB Loader window: the same code follows its table. */
  const uint32_t loader_size = 1904u * 1024u, app_offset = LOADER_OFFSET + loader_size;
  cp = (bk_logic_partition_t){LOADER_OFFSET, 952u * 1024u};
  ap = (bk_logic_partition_t){LOADER_OFFSET + 952u * 1024u, loader_size - 952u * 1024u};
  s_app = (bk_logic_partition_t){app_offset, 0x75f000u - app_offset};
  native_slot = 0u;
  assert(h2_bk_fixed_current_slot() == 0u);
  assert(h2_bk_fixed_layout()->app.offset == app_offset);
  put_app_window_image(app_offset);
  memset(native_sector, 0xff, sizeof(native_sector));
  memset(request_sector, 0xff, sizeof(request_sector));
  assert(h2_bk_fixed_select(H2_BK_H2LOADER_APP_PARTITION_ID) == H2_PAL_OK);
  h2_fixed_boot_request_t r;
  memcpy(&r, request_sector, sizeof(r));
  assert(r.app_offset == app_offset && r.app_size == 0x75f000u - app_offset);
  assert(cp_boot() && h2_bk_fixed_confirm_app() == H2_PAL_OK && h2_bk_fixed_next_app());
}

int main(void) {
  test_confirmed_app_boots_without_writes();
  test_unconfirmed_attempt_and_loader_selection();
  test_invalidate_before_app_write();
  test_torn_request_is_not_bootable();
  test_loader_relay_through_native_b();
  test_board_owned_loader_size();
  test_app_layout_slot();
  return 0;
}
