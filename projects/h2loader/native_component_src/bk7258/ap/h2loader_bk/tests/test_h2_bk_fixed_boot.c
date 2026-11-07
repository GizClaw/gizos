#include "h2_bk_fixed_boot_test_sdk.h"
#include "layout.h"
#include "flash_access.h"
#include "contract.h"
#include "h2_bk_boot_context.h"
#include <assert.h>
#include <stddef.h>
#include <string.h>

/* NOR model: erase sets a 4 KiB sector to 0xff, a write can only clear bits. */
#define SECTOR 4096u
#define NATIVE_CONTROL 0x0075f000u
/* The first four App window sectors; the relay-head test uses them all. */
#define APP_MODEL (4u * SECTOR)
static uint8_t native_sector[SECTOR], request_sector[SECTOR],
    app_sector[APP_MODEL];
static uint8_t mapped_app[APP_MODEL / 34u * 32u];
static uint8_t native_slot;
static unsigned native_erases, request_erases;
static unsigned aes_state, flash_mutations;
static int mapped_view, plain_read_error;
static int flash_read_error, flash_write_error;
/* Logical boot window: descriptor at 0x7f00 and payload from 0x8000. */
#define BOOT_LOGICAL 0x10000u
#define PAYLOAD_SIZE 300u
static uint8_t boot_logical[BOOT_LOGICAL];
static int boot_read_error;
/* Committed Partition 2 metadata as bound through the boot context. */
static int candidate_role = H2_PAL_ERR_INVALID_STATE;
static uint64_t candidate_size;
static int boot_context_rc;
static int loader_window_committed = 1;
/* Board partition table (Loader image): Loader window 2380 KiB, App window
 * 5100 KiB, boot record sector at 0x77f000. */
#define LOADER_OFFSET 0x11000u
#define LOADER_CP (1156u * 1024u)
#define LOADER_SIZE (2380u * 1024u)
#define APP_OFFSET (LOADER_OFFSET + LOADER_SIZE)
#define APP_SIZE (5100u * 1024u)
#define CONTROL 0x77f000u
static bk_logic_partition_t cp = {LOADER_OFFSET, LOADER_CP};
static bk_logic_partition_t ap = {LOADER_OFFSET + LOADER_CP,
                                  LOADER_SIZE - LOADER_CP};
static bk_logic_partition_t s_app = {APP_OFFSET, APP_SIZE};
static bk_logic_partition_t control = {CONTROL, SECTOR};
static bk_logic_partition_t native = {NATIVE_CONTROL, SECTOR};

static uint8_t* sector_at(uint32_t address, uint32_t size) {
  if (address >= NATIVE_CONTROL && address + size <= NATIVE_CONTROL + SECTOR)
    return native_sector + (address - NATIVE_CONTROL);
  if (address >= control.partition_start_addr &&
      address + size <= control.partition_start_addr + SECTOR)
    return request_sector + (address - control.partition_start_addr);
  const uint32_t app_base = h2_bk_fixed_layout()->app.offset;
  if (address >= app_base && address + size <= app_base + APP_MODEL)
    return app_sector + (address - app_base);
  assert(!"flash access outside modeled sectors");
  return NULL;
}
const bk_logic_partition_t* bk_flash_partition_get_info(bk_partition_t id) {
  return id == BK_PARTITION_APPLICATION       ? &cp
         : id == BK_PARTITION_APPLICATION1    ? &ap
         : id == BK_PARTITION_S_APP           ? &s_app
         : id == BK_PARTITION_H2_BOOT_REQUEST ? &control
                                              : &native;
}
int bk_flash_read_bytes(uint32_t address, uint8_t* out, uint32_t size) {
  if (flash_read_error) return BK_FAIL;
  memcpy(out, sector_at(address, size), size);
  return BK_OK;
}
int bk_flash_write_bytes(uint32_t address, const uint8_t* in, uint32_t size) {
  ++flash_mutations;
  if (flash_write_error) return BK_FAIL;
  uint8_t* cell = sector_at(address, size);
  for (uint32_t i = 0; i < size; ++i) cell[i] &= in[i];
  return BK_OK;
}
int bk_flash_erase_sector(uint32_t address) {
  ++flash_mutations;
  memset(sector_at(address, SECTOR), 0xff, SECTOR);
  if (address == NATIVE_CONTROL)
    ++native_erases;
  else if (address == control.partition_start_addr)
    ++request_erases;
  return BK_OK;
}
int bk_flash_set_protect_type(flash_protect_type_t type) {
  (void)type;
  return BK_OK;
}
flash_protect_type_t bk_flash_get_protect_type(void) {
  return FLASH_PROTECT_ALL;
}
uint8_t bk_ota_get_current_partition(void) { return native_slot; }

unsigned h2_bk_flash_aes_state(void) { return aes_state; }
int h2_bk_flash_read_plain(uint32_t base, uint32_t capacity, uint32_t offset,
                           void* out, size_t size) {
  if (plain_read_error) return plain_read_error;
  assert(offset <= capacity / 34u * 32u &&
         size <= capacity / 34u * 32u - offset);
  if (mapped_view) assert(offset + size <= sizeof(mapped_app));
  uint8_t* dst = out;
  while (size--) {
    uint8_t* packet = sector_at(base + offset / 32u * 34u, 34);
    if (!h2_bk_flash_erased(packet) && !h2_bk_flash_crc_valid(packet))
      return H2_PAL_ERR_IO;
    *dst++ = mapped_view ? mapped_app[offset] : packet[offset % 32u];
    ++offset;
  }
  return H2_PAL_OK;
}

int h2_bk_flash_read_boot_plain(uint32_t boot_size, uint32_t offset, void* out,
                                size_t size) {
  /* Only ever a bounded read of the boot window, from a board-derived size. */
  assert(boot_size == LOADER_OFFSET && size <= 256u &&
         offset + size <= BOOT_LOGICAL);
  if (boot_read_error) return boot_read_error;
  memcpy(out, boot_logical + offset, size);
  return H2_PAL_OK;
}
int h2_bk_boot_candidate_is_loader(uint64_t* out_image_size) {
  *out_image_size = candidate_role < 0 ? 0u : candidate_size;
  return candidate_role;
}

int h2_bk_boot_context_ready(void) { return boot_context_rc; }
int h2_bk_boot_loader_window_committed(void) { return loader_window_committed; }

/* Mirrors h2loader_cp_try_fixed_app: returns whether CP enters App. */
static int cp_boot(void) {
  h2_fixed_boot_request_t r;
  uint32_t zero = 0u;
  bk_flash_read_bytes(CONTROL, (uint8_t*)&r, sizeof(r));
  if (!h2_fixed_request_boots_app(&r, h2_bk_fixed_layout())) return 0;
  bk_flash_write_bytes(CONTROL, (const uint8_t*)&zero, sizeof(zero));
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
  request_sector[0] = request_sector[1] = request_sector[2] =
      request_sector[3] = 0xff;
  assert(!h2_bk_fixed_next_app() && !h2_bk_fixed_app_failed());
  memcpy(request_sector, &r, sizeof(r));
  request_sector[offsetof(h2_fixed_boot_request_t, confirmed)] = 0u;
  assert(!h2_bk_fixed_next_app());
}

static const uint8_t relay_pending[12] = {1,    0xff, 0xff, 0xff, 1,    0xff,
                                          0xff, 0xff, 1,    0xff, 0xff, 0xff};
static const uint8_t relay_confirmed[12] = {1,    0xff, 0xff, 0xff, 1,    0xff,
                                            0xff, 0xff, 4,    0xff, 0xff, 0xff};

static void set_app_packet_crc(void) {
  uint16_t crc = 0xffff;
  for (unsigned i = 0; i < 32; ++i) {
    crc ^= (uint16_t)app_sector[i] << 8;
    for (unsigned b = 0; b < 8; ++b)
      crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x8005 : 0));
  }
  app_sector[32] = (uint8_t)(crc >> 8);
  app_sector[33] = (uint8_t)crc;
}

/* Stage a CP vector table in the App window whose reset handler was linked
 * for the window starting at offset. */
static void put_app_window_image(uint32_t offset) {
  const uint32_t words[2] = {0x28080000u,
                             H2_FIXED_XIP_ADDRESS(offset) + 0x201u};
  memset(app_sector, 0xff, sizeof(app_sector));
  memcpy(app_sector, words, sizeof(words));
  set_app_packet_crc();
}

static void put_encrypted_app_vector(void) {
  /* Public key bytes(range(32)), logical address 0x240000, from #810.
   * Raw reset PC 0x0219975a falls in Loader, mapped PC 0x02240101 in App. */
  static const uint8_t ciphertext[32] = {
      0xed, 0x61, 0x1b, 0x53, 0x5a, 0x97, 0x19, 0x02, 0x42, 0x1b, 0xd5,
      0x2e, 0x64, 0x76, 0x41, 0x95, 0x98, 0x51, 0x6b, 0x10, 0xa0, 0xb5,
      0x6e, 0x62, 0x91, 0x90, 0xb3, 0xf3, 0xdf, 0x2e, 0xc7, 0xb7};
  static const uint8_t plaintext[32] = {0x00, 0x00, 0x03, 0x28, 0x01,
                                        0x01, 0x24, 0x02, 0x0d};
  memset(app_sector, 0xff, sizeof(app_sector));
  memset(mapped_app, 0xff, sizeof(mapped_app));
  memcpy(app_sector, ciphertext, sizeof(ciphertext));
  memcpy(mapped_app, plaintext, sizeof(plaintext));
  set_app_packet_crc();
  mapped_view = 1;
  aes_state = H2_BK_FLASH_AES_ON;
  native_slot = 0u;
}

static void test_ciphertext_uses_mapped_app_role(void) {
  put_encrypted_app_vector();
  const h2_fixed_layout_t* layout = h2_bk_fixed_layout();
  uint32_t raw_pc;
  memcpy(&raw_pc, app_sector + 4, sizeof(raw_pc));
  assert(h2_fixed_xip_in_window(raw_pc, &layout->loader));
  assert(!h2_fixed_xip_in_window(raw_pc, &layout->app));
  assert(h2_bk_fixed_app_window_holds_loader() == 0);
  memset(native_sector, 0xff, sizeof(native_sector));
  memset(request_sector, 0xff, sizeof(request_sector));
  assert(h2_bk_fixed_select(H2_BK_H2LOADER_APP_PARTITION_ID) == H2_PAL_OK);
  static const uint32_t loader_flags[3] = {0u, 0u, 3u};
  assert(memcmp(native_sector, loader_flags, sizeof(loader_flags)) == 0);
  assert(h2_bk_fixed_next_app() && cp_boot());
  assert(h2_bk_fixed_confirm_app() == H2_PAL_OK);
  mapped_view = 0;
  aes_state = H2_BK_FLASH_AES_OFF;
}

static void test_role_read_failure_does_not_select_boot(void) {
  put_encrypted_app_vector();
  memset(native_sector, 0xff, sizeof(native_sector));
  memset(request_sector, 0xff, sizeof(request_sector));
  const unsigned before = flash_mutations;
  const int errors[] = {H2_PAL_ERR_IO, H2_PAL_ERR_UNSUPPORTED};
  for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
    plain_read_error = errors[i];
    assert(h2_bk_fixed_app_window_holds_loader() == errors[i]);
    assert(h2_bk_fixed_select(H2_BK_H2LOADER_APP_PARTITION_ID) == errors[i]);
    assert(flash_mutations == before);
  }
  plain_read_error = 0;
  app_sector[0] ^= 1u;
  assert(h2_bk_fixed_select(H2_BK_H2LOADER_APP_PARTITION_ID) == H2_PAL_ERR_IO);
  app_sector[0] ^= 1u;
  mapped_app[4] &= 0xfeu; /* A non-Thumb mapped reset PC is invalid. */
  assert(h2_bk_fixed_select(H2_BK_H2LOADER_APP_PARTITION_ID) ==
         H2_PAL_ERR_FORMAT);
  assert(flash_mutations == before);
  mapped_view = 0;
  aes_state = H2_BK_FLASH_AES_OFF;
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
  assert(h2_bk_fixed_next_app() &&
         memcmp(native_sector, loader_flags, 12) == 0);

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

  /* A pending B the bootloader rolled back (final=A, temp=B, confirm=1) is a
   * failed candidate seen from the Loader window; it no longer selects B. */
  {
    static const uint8_t rolled_back[12] = {0,    0xff, 0xff, 0xff, 1,    0xff,
                                            0xff, 0xff, 1,    0xff, 0xff, 0xff};
    uint8_t saved[12];
    memcpy(saved, native_sector, sizeof(saved));
    memcpy(native_sector, rolled_back, sizeof(rolled_back));
    native_slot = 0u;
    assert(h2_bk_fixed_relay_failed() && !h2_bk_fixed_next_app());
    native_slot = 1u;
    assert(!h2_bk_fixed_relay_failed());
    memcpy(native_sector, relay_pending, sizeof(relay_pending));
    native_slot = 0u;
    assert(!h2_bk_fixed_relay_failed() && h2_bk_fixed_next_app());
    /* Blank flags (power loss between erase and write) boot A: no failure,
     * no B selection. */
    memset(native_sector, 0xff, 12);
    assert(!h2_bk_fixed_relay_failed() && !h2_bk_fixed_next_app());
    memcpy(native_sector, saved, sizeof(saved));
    native_slot = 1u;
  }

  /* After the copy, Loader selection returns native boot to A. */
  assert(h2_bk_fixed_select(H2_BK_H2LOADER_PRIMARY_PARTITION_ID) == H2_PAL_OK);
  assert(memcmp(native_sector, loader_flags, 12) == 0);
  assert(!h2_bk_fixed_next_app());
  native_slot = 0u;
  assert(h2_bk_fixed_current_slot() == 0u);
  assert(h2_bk_fixed_confirm_loader() == H2_PAL_OK);
  assert(memcmp(native_sector, loader_flags, 12) == 0);
}

static bk_mapped_ab_contract_t board_contract(void) {
  return (bk_mapped_ab_contract_t){
      .magic = BK_MAPPED_AB_MAGIC,
      .version = BK_MAPPED_AB_VERSION,
      .descriptor_size = sizeof(bk_mapped_ab_contract_t),
      .flags = BK_MAPPED_AB_FLAGS,
      .boot_size = LOADER_OFFSET,
      .loader_offset = LOADER_OFFSET,
      .loader_size = LOADER_SIZE,
      .trial_offset = APP_OFFSET,
      .trial_size = APP_SIZE,
      .cp_size = LOADER_CP,
      .payload_offset = BK_MAPPED_AB_PAYLOAD_OFFSET,
      .payload_size = PAYLOAD_SIZE,
      .patch_entry = BK_MAPPED_AB_PATCH_ENTRY,
  };
}

/* Flash a descriptor with correct CRCs, so tests reach the check under test
 * instead of an earlier integrity rejection. */
static void flash_contract(bk_mapped_ab_contract_t c) {
  uint8_t* payload = boot_logical + BK_MAPPED_AB_PAYLOAD_OFFSET;
  for (uint32_t i = 0; i < PAYLOAD_SIZE; ++i) payload[i] = (uint8_t)(i * 3u);
  c.payload_crc32 = bk_mapped_ab_crc32(payload, PAYLOAD_SIZE);
  c.descriptor_crc32 = bk_mapped_ab_crc32(
      &c, offsetof(bk_mapped_ab_contract_t, descriptor_crc32));
  memcpy(boot_logical + BK_MAPPED_AB_DESCRIPTOR_OFFSET, &c, sizeof(c));
}

static void erase_contract(void) {
  memset(boot_logical, 0xff, sizeof(boot_logical));
}

static void reset_boot_records(void) {
  memset(native_sector, 0xff, sizeof(native_sector));
  memset(request_sector, 0xff, sizeof(request_sector));
  native_slot = 0u;
}

static void assert_rejected_without_writes(int expected) {
  const unsigned before = flash_mutations;
  assert(h2_bk_fixed_mapped_abi() == expected);
  assert(h2_bk_fixed_loader_update_check(LOADER_SIZE) == expected);
  assert(flash_mutations == before);
}

static void test_boot_context_check(void) {
  put_encrypted_app_vector();
  reset_boot_records();
  /* Stock boards never need a boot context. */
  boot_context_rc = H2_PAL_ERR_INVALID_STATE;
  assert(h2_bk_fixed_boot_context_check() == H2_PAL_OK);
  flash_contract(board_contract());
  assert(h2_bk_fixed_boot_context_check() == H2_PAL_ERR_INVALID_STATE);
  boot_context_rc = H2_PAL_OK;
  assert(h2_bk_fixed_boot_context_check() == H2_PAL_OK);
  boot_read_error = H2_PAL_ERR_IO;
  assert(h2_bk_fixed_boot_context_check() == H2_PAL_ERR_IO);
  boot_read_error = 0;
  erase_contract();
}

static void test_mapped_descriptor_rejections(void) {
  reset_boot_records();
  aes_state = H2_BK_FLASH_AES_ON;

  /* Missing ABI: a stock bootloader. AES-on has no Loader path; AES-off keeps
   * the stock relay. */
  erase_contract();
  assert(h2_bk_fixed_mapped_abi() == 0);
  assert(h2_bk_fixed_loader_update_check(LOADER_SIZE) ==
         H2_PAL_ERR_UNSUPPORTED);
  aes_state = H2_BK_FLASH_AES_OFF;
  assert(h2_bk_fixed_loader_update_check(LOADER_SIZE) == 0);
  aes_state = H2_BK_FLASH_AES_ON;

  flash_contract(board_contract());
  assert(h2_bk_fixed_mapped_abi() == 1);
  assert(h2_bk_fixed_loader_update_check(LOADER_SIZE) == 1);
  aes_state = H2_BK_FLASH_AES_OFF;
  assert(h2_bk_fixed_loader_update_check(LOADER_SIZE) == 1);
  aes_state = H2_BK_FLASH_AES_ON;

  /* A descriptor that is intact but disagrees with the board's table. */
  bk_mapped_ab_contract_t c = board_contract();
  c.loader_size = LOADER_SIZE - H2_FIXED_WINDOW_ALIGN;
  flash_contract(c);
  assert_rejected_without_writes(H2_PAL_ERR_FORMAT);
  c = board_contract();
  c.trial_size = APP_SIZE - H2_FIXED_WINDOW_ALIGN;
  flash_contract(c);
  assert_rejected_without_writes(H2_PAL_ERR_FORMAT);
  c = board_contract();
  c.trial_offset = APP_OFFSET + H2_FIXED_WINDOW_ALIGN;
  flash_contract(c);
  assert_rejected_without_writes(H2_PAL_ERR_FORMAT);
  c = board_contract();
  c.cp_size = LOADER_CP + H2_FIXED_WINDOW_ALIGN;
  flash_contract(c);
  assert_rejected_without_writes(H2_PAL_ERR_FORMAT);

  /* Unknown version with a matching CRC, then each integrity failure. */
  c = board_contract();
  c.version = BK_MAPPED_AB_VERSION + 1u;
  flash_contract(c);
  assert_rejected_without_writes(H2_PAL_ERR_FORMAT);
  flash_contract(board_contract());
  boot_logical[BK_MAPPED_AB_DESCRIPTOR_OFFSET + 8u] ^= 1u;
  assert_rejected_without_writes(H2_PAL_ERR_FORMAT);
  flash_contract(board_contract());
  boot_logical[BK_MAPPED_AB_PAYLOAD_OFFSET + PAYLOAD_SIZE - 1u] ^= 1u;
  assert_rejected_without_writes(H2_PAL_ERR_FORMAT);

  flash_contract(board_contract());
  boot_read_error = H2_PAL_ERR_IO;
  assert_rejected_without_writes(H2_PAL_ERR_IO);
  boot_read_error = 0;

  /* Unknown AES and a non-canonical image never pass a valid descriptor. */
  const unsigned before = flash_mutations;
  aes_state = H2_BK_FLASH_AES_UNKNOWN;
  assert(h2_bk_fixed_mapped_abi() == 1);
  assert(h2_bk_fixed_loader_update_check(LOADER_SIZE) ==
         H2_PAL_ERR_UNSUPPORTED);
  aes_state = H2_BK_FLASH_AES_ON;
  assert(h2_bk_fixed_loader_update_check(LOADER_SIZE - 34u) ==
         H2_PAL_ERR_FORMAT);
  assert(h2_bk_fixed_loader_update_check(APP_SIZE) == H2_PAL_ERR_FORMAT);
  assert(flash_mutations == before);

  erase_contract();
  aes_state = H2_BK_FLASH_AES_OFF;
}

static void test_mapped_selection_uses_committed_role(void) {
  put_encrypted_app_vector();
  reset_boot_records();
  flash_contract(board_contract());
  unsigned before = flash_mutations;

  /* power_api(void) without a bound context, or a cleared record. */
  candidate_role = H2_PAL_ERR_INVALID_STATE;
  assert(h2_bk_fixed_app_window_holds_loader() == H2_PAL_ERR_INVALID_STATE);
  assert(h2_bk_fixed_select(H2_BK_H2LOADER_APP_PARTITION_ID) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(!h2_bk_fixed_next_app() && flash_mutations == before);

  /* Undecodable vectors say nothing: the record alone decides, in both ways. */
  plain_read_error = H2_PAL_ERR_IO;
  candidate_role = 0;
  candidate_size = APP_SIZE;
  assert(h2_bk_fixed_app_window_holds_loader() == 0);
  candidate_role = 1;
  candidate_size = LOADER_SIZE;
  assert(h2_bk_fixed_app_window_holds_loader() == 1);

  /* A size the mapped bootloader could not validate is never selected. */
  candidate_size = LOADER_SIZE - 34u;
  assert(h2_bk_fixed_select(H2_BK_H2LOADER_APP_PARTITION_ID) ==
         H2_PAL_ERR_FORMAT);
  candidate_role = 0;
  candidate_size = APP_SIZE + 34u;
  assert(h2_bk_fixed_select(H2_BK_H2LOADER_APP_PARTITION_ID) ==
         H2_PAL_ERR_FORMAT);
  candidate_size = 0u;
  assert(h2_bk_fixed_select(H2_BK_H2LOADER_APP_PARTITION_ID) ==
         H2_PAL_ERR_FORMAT);
  assert(flash_mutations == before);

  /* An App record keeps the ordinary CP handoff under AES. */
  static const uint32_t loader_flags[3] = {0u, 0u, 3u};
  candidate_size = APP_SIZE;
  assert(h2_bk_fixed_select(H2_BK_H2LOADER_APP_PARTITION_ID) == H2_PAL_OK);
  assert(memcmp(native_sector, loader_flags, sizeof(loader_flags)) == 0);
  assert(h2_bk_fixed_next_app() && cp_boot());
  assert(h2_bk_fixed_confirm_app() == H2_PAL_OK);

  /* Unknown AES cannot relay a Loader. */
  reset_boot_records();
  before = flash_mutations;
  candidate_role = 1;
  candidate_size = LOADER_SIZE;
  aes_state = H2_BK_FLASH_AES_UNKNOWN;
  assert(h2_bk_fixed_select(H2_BK_H2LOADER_APP_PARTITION_ID) ==
         H2_PAL_ERR_UNSUPPORTED);
  assert(flash_mutations == before);
  aes_state = H2_BK_FLASH_AES_ON;

  plain_read_error = 0;
  mapped_view = 0;
  candidate_role = H2_PAL_ERR_INVALID_STATE;
  aes_state = H2_BK_FLASH_AES_OFF;
  erase_contract();
}

static void test_unknown_aes_keeps_ordinary_app_handoff(void) {
  static const uint32_t loader_flags[3] = {0u, 0u, 3u};
  reset_boot_records();
  erase_contract();
  aes_state = H2_BK_FLASH_AES_UNKNOWN;
  put_app_window_image(APP_OFFSET);
  assert(h2_bk_fixed_select(H2_BK_H2LOADER_APP_PARTITION_ID) == H2_PAL_OK);
  assert(memcmp(native_sector, loader_flags, sizeof(loader_flags)) == 0);
  assert(h2_bk_fixed_next_app());

  /* The same unknown state refuses a stock Loader relay. */
  reset_boot_records();
  put_app_window_image(LOADER_OFFSET);
  const unsigned before = flash_mutations;
  assert(h2_bk_fixed_select(H2_BK_H2LOADER_APP_PARTITION_ID) ==
         H2_PAL_ERR_UNSUPPORTED);
  assert(flash_mutations == before);
  aes_state = H2_BK_FLASH_AES_OFF;
}

static void test_mapped_candidate_confirmation(void) {
  put_encrypted_app_vector();
  reset_boot_records();
  flash_contract(board_contract());
  plain_read_error = H2_PAL_ERR_IO; /* Ciphertext vectors must stay unused. */
  candidate_role = 1;
  candidate_size = LOADER_SIZE;

  /* Selection arms B and clears the CP handoff, with no relocated head. */
  assert(h2_bk_fixed_select(H2_BK_H2LOADER_APP_PARTITION_ID) == H2_PAL_OK);
  assert(memcmp(native_sector, relay_pending, 12) == 0);
  assert(!cp_boot() && h2_bk_fixed_next_app());

  /* Still on A, or pending on B: the Loader window stays off limits. */
  assert(h2_bk_fixed_relay_confirmed() == H2_PAL_ERR_INVALID_STATE);
  assert(h2_bk_fixed_confirm_loader() == H2_PAL_OK);
  assert(memcmp(native_sector, relay_pending, 12) == 0);
  native_slot = 1u;
  assert(h2_bk_fixed_relay_confirmed() == H2_PAL_ERR_INVALID_STATE);

  /* Confirmation re-proves ABI and role from native B; a failure leaves the
   * pending record, so the bootloader can still roll back. */
  unsigned before = flash_mutations;
  boot_logical[BK_MAPPED_AB_PAYLOAD_OFFSET] ^= 1u;
  assert(h2_bk_fixed_confirm_loader() == H2_PAL_ERR_FORMAT);
  boot_logical[BK_MAPPED_AB_PAYLOAD_OFFSET] ^= 1u;
  erase_contract();
  assert(h2_bk_fixed_confirm_loader() == H2_PAL_ERR_UNSUPPORTED);
  flash_contract(board_contract());
  candidate_role = 0;
  assert(h2_bk_fixed_confirm_loader() == H2_PAL_ERR_INVALID_STATE);
  candidate_role = H2_PAL_ERR_INVALID_STATE;
  assert(h2_bk_fixed_confirm_loader() == H2_PAL_ERR_INVALID_STATE);
  candidate_role = 1;
  aes_state = H2_BK_FLASH_AES_UNKNOWN;
  assert(h2_bk_fixed_confirm_loader() == H2_PAL_ERR_UNSUPPORTED);
  aes_state = H2_BK_FLASH_AES_ON;
  assert(flash_mutations == before);
  assert(memcmp(native_sector, relay_pending, 12) == 0);

  /* A failed write is reported and does not count as confirmed. */
  flash_write_error = 1;
  assert(h2_bk_fixed_confirm_loader() == H2_PAL_ERR_IO);
  flash_write_error = 0;
  memcpy(native_sector, relay_pending, sizeof(relay_pending));
  flash_read_error = 1;
  assert(h2_bk_fixed_confirm_loader() == H2_PAL_ERR_IO);
  assert(h2_bk_fixed_relay_confirmed() == H2_PAL_ERR_IO);
  flash_read_error = 0;

  assert(h2_bk_fixed_confirm_loader() == H2_PAL_OK);
  assert(memcmp(native_sector, relay_confirmed, 12) == 0);
  assert(h2_bk_fixed_relay_confirmed() == H2_PAL_OK);

  /* Core re-selects B before copying: B and its confirmation stay untouched. */
  before = flash_mutations;
  assert(h2_bk_fixed_select(H2_BK_H2LOADER_APP_PARTITION_ID) == H2_PAL_OK);
  assert(flash_mutations == before);
  assert(memcmp(native_sector, relay_confirmed, 12) == 0);

  /* Copyback failed or was interrupted: A is partial. Reboot commands succeed
   * but keep the confirmed B, so the restart retries the copy. */
  before = flash_mutations;
  loader_window_committed = 0;
  assert(h2_bk_fixed_select(H2_BK_H2LOADER_PRIMARY_PARTITION_ID) == H2_PAL_OK);
  assert(h2_bk_fixed_invalidate_app() == H2_PAL_OK);
  assert(h2_bk_fixed_select(H2_BK_H2LOADER_APP_PARTITION_ID) == H2_PAL_OK);
  loader_window_committed = H2_PAL_ERR_IO;
  assert(h2_bk_fixed_select(H2_BK_H2LOADER_PRIMARY_PARTITION_ID) == H2_PAL_OK);
  assert(flash_mutations == before);
  assert(memcmp(native_sector, relay_confirmed, 12) == 0);

  /* Without a confirmed B there is nothing safe to keep. */
  memcpy(native_sector, relay_pending, sizeof(relay_pending));
  loader_window_committed = 0;
  assert(h2_bk_fixed_select(H2_BK_H2LOADER_PRIMARY_PARTITION_ID) ==
         H2_PAL_ERR_INVALID_STATE);
  loader_window_committed = H2_PAL_ERR_IO;
  assert(h2_bk_fixed_select(H2_BK_H2LOADER_PRIMARY_PARTITION_ID) ==
         H2_PAL_ERR_IO);
  loader_window_committed = 1;
  assert(memcmp(native_sector, relay_pending, 12) == 0);
  memcpy(native_sector, relay_confirmed, sizeof(relay_confirmed));

  /* Copyback finished: Loader selection returns native boot to A. */
  static const uint32_t loader_flags[3] = {0u, 0u, 3u};
  assert(h2_bk_fixed_select(H2_BK_H2LOADER_PRIMARY_PARTITION_ID) == H2_PAL_OK);
  assert(memcmp(native_sector, loader_flags, 12) == 0);
  assert(h2_bk_fixed_relay_confirmed() == H2_PAL_ERR_INVALID_STATE);

  native_slot = 0u;
  plain_read_error = 0;
  mapped_view = 0;
  candidate_role = H2_PAL_ERR_INVALID_STATE;
  aes_state = H2_BK_FLASH_AES_OFF;
  erase_contract();
}

static void test_relay_head_shares_image_sector(void) {
  /* A 16 KiB window with a 10 KiB image: the head copy lands in the sector
   * that also holds the image's last 2 KiB, which must survive the erase. */
  const uint32_t base = s_app.partition_start_addr, window = APP_MODEL;
  const uint32_t image = 0x2800u, head = 0x1100u;
  for (uint32_t i = 0; i < APP_MODEL; ++i)
    app_sector[i] = i < image ? (uint8_t)(i * 7u + 1u) : 0xffu;
  uint8_t expected[APP_MODEL];
  memcpy(expected, app_sector, sizeof(expected));
  memcpy(&expected[window - head], &app_sector[image - head], head);
  assert(h2_bk_fixed_publish_relay_head(base, window, image, head) ==
         H2_PAL_OK);
  assert(memcmp(app_sector, expected, sizeof(expected)) == 0);
  /* Equal window and image: the head already ends the window. */
  memcpy(expected, app_sector, sizeof(expected));
  assert(h2_bk_fixed_publish_relay_head(base, window, window, head) ==
         H2_PAL_OK);
  assert(memcmp(app_sector, expected, sizeof(expected)) == 0);
  /* A gap smaller than the head would overwrite the image. */
  assert(h2_bk_fixed_publish_relay_head(base, window, window - 1u, head) ==
         H2_PAL_ERR_INVALID_STATE);
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
  /* A pre-confirm App failure explicitly selects Loader. Preserve the trial
   * failure so Loader's AUTO policy rejects the same staged candidate. */
  put_app_window_image(APP_OFFSET);
  native_slot = 0u;
  memset(request_sector, 0xff, sizeof(request_sector));
  assert(h2_bk_fixed_select(H2_BK_H2LOADER_APP_PARTITION_ID) == H2_PAL_OK);
  assert(cp_boot() && h2_bk_fixed_app_failed());
  unsigned erases = request_erases;
  assert(h2_bk_fixed_select(H2_BK_H2LOADER_PRIMARY_PARTITION_ID) == H2_PAL_OK);
  assert(request_erases == erases);
  assert(h2_bk_fixed_app_failed() && !cp_boot());
  /* A healthy App can still clear its confirmed selection on return. */
  assert(h2_bk_fixed_confirm_app() == H2_PAL_OK);
  assert(h2_bk_fixed_select(H2_BK_H2LOADER_PRIMARY_PARTITION_ID) == H2_PAL_OK);
  assert(!h2_bk_fixed_app_failed() && !cp_boot());
  /* The App table does not list the Loader's CP split, so it cannot and must
   * not compare cp_size; the shared windows are still checked. */
  bk_mapped_ab_contract_t c = board_contract();
  c.cp_size = LOADER_CP + H2_FIXED_WINDOW_ALIGN;
  flash_contract(c);
  assert(h2_bk_fixed_mapped_abi() == 1);
  /* An App image never owns a Loader relay, even if the native slot says B. */
  native_slot = 1u;
  aes_state = H2_BK_FLASH_AES_ON;
  candidate_role = 1;
  candidate_size = LOADER_SIZE;
  assert(h2_bk_fixed_relay_confirmed() == H2_PAL_ERR_INVALID_STATE);
  assert(h2_bk_fixed_confirm_loader() == H2_PAL_ERR_INVALID_STATE);
  native_slot = 0u;
  aes_state = H2_BK_FLASH_AES_OFF;
  candidate_role = H2_PAL_ERR_INVALID_STATE;
  c.trial_size = APP_SIZE - H2_FIXED_WINDOW_ALIGN;
  flash_contract(c);
  assert(h2_bk_fixed_mapped_abi() == H2_PAL_ERR_FORMAT);
  erase_contract();
  /* A table without matching windows is not a fixed layout. */
  ap.partition_start_addr += SECTOR;
  assert(!h2_bk_fixed_layout_active());
}

static void test_board_owned_loader_size(void) {
  /* A board with a 1904 KiB Loader window: the same code follows its table. */
  const uint32_t loader_size = 1904u * 1024u,
                 app_offset = LOADER_OFFSET + loader_size;
  cp = (bk_logic_partition_t){LOADER_OFFSET, 952u * 1024u};
  ap = (bk_logic_partition_t){LOADER_OFFSET + 952u * 1024u,
                              loader_size - 952u * 1024u};
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
  assert(cp_boot() && h2_bk_fixed_confirm_app() == H2_PAL_OK &&
         h2_bk_fixed_next_app());
}

int main(void) {
  erase_contract();
  put_app_window_image(APP_OFFSET);
  test_confirmed_app_boots_without_writes();
  test_unconfirmed_attempt_and_loader_selection();
  test_invalidate_before_app_write();
  test_torn_request_is_not_bootable();
  test_loader_relay_through_native_b();
  test_ciphertext_uses_mapped_app_role();
  test_role_read_failure_does_not_select_boot();
  test_boot_context_check();
  test_mapped_descriptor_rejections();
  test_mapped_selection_uses_committed_role();
  test_unknown_aes_keeps_ordinary_app_handoff();
  test_mapped_candidate_confirmation();
  test_relay_head_shares_image_sector();
  test_board_owned_loader_size();
  test_app_layout_slot();
  return 0;
}
