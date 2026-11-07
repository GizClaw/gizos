#include "h2_bk_h2loader.h"
#include "h2_bk_h2loader_internal.h"
#include "h2_bk_fixed_boot.h"
#include "h2_bk_flash_access.h"
#include "h2_bk_h2loader_ota_writer_test_sdk.h"
#include "flash_access.h"
#include <assert.h>
#include <string.h>
#define BASE 0x264000u
#define LOGICAL 12288u
#define STORED (LOGICAL / 32u * 34u)
#define LOADER_BASE 0x11000u
#define WINDOW 69632u
#define WINDOW_LOGICAL (WINDOW / 34u * 32u)
static uint8_t storage[WINDOW], image[STORED], plain[LOGICAL];
static uint8_t loader_storage[WINDOW], loader_image[WINDOW];
static uint8_t loader_plain[WINDOW_LOGICAL];
static unsigned mode, erased, invalidated, committed, fail_write, relay_heads;
static unsigned writes, selects, mapped_reads, fail_read;
static uint8_t current_slot;
/* 0 = stock ABI, 1 = verified mapped_ab_v1, negative = refused. */
static int mapped_check, relay_confirmed_rc, boot_context_rc;
static uint64_t checked_size;
part_flag update_part_flag;
static const h2_fixed_layout_t layout = {.loader = {0x11000, 69632},
                                         .app = {BASE, 69632},
                                         .control_offset = 0x770000};
static const bk_logic_partition_t parts[] = {
    {0}, {0x11000, 34816}, {0x19800, 34816}, {BASE, 69632}, {0x770000, 4096}};
const bk_logic_partition_t* bk_flash_partition_get_info(unsigned i) {
  return i < 5 ? &parts[i] : NULL;
}
static uint8_t* flash_at(uint32_t a, uint32_t n) {
  if (a >= LOADER_BASE && a + n <= LOADER_BASE + sizeof(loader_storage))
    return loader_storage + (a - LOADER_BASE);
  assert(a >= BASE && a + n <= BASE + sizeof(storage));
  return storage + (a - BASE);
}
int bk_flash_read_bytes(uint32_t a, uint8_t* p, uint32_t n) {
  if (fail_read) return -1;
  memcpy(p, flash_at(a, n), n);
  return 0;
}
int bk_flash_write_bytes(uint32_t a, const uint8_t* p, uint32_t n) {
  uint8_t* cell = flash_at(a, n);
  writes++;
  if (fail_write) return -1;
  for (unsigned i = 0; i < n; i++) cell[i] &= p[i];
  return 0;
}
int bk_flash_erase_sector(uint32_t a) {
  memset(flash_at(a, 4096), 255, 4096);
  erased++;
  return 0;
}
int bk_flash_set_protect_type(flash_protect_type_t t) {
  (void)t;
  return 0;
}
flash_protect_type_t bk_flash_get_protect_type(void) {
  return FLASH_PROTECT_ALL;
}
int bk_wdt_feed(void) { return 0; }
int bk_ota_update_partition_flag(int r) {
  committed++;
  return r;
}
void bk_ota_double_check_for_execution(void) {}
void bk_ota_confirm_update_partition(unsigned v) { (void)v; }
int ota_get_rbl_head(const bk_logic_partition_t* p, struct ota_rbl_head* h,
                     uint32_t n) {
  (void)p;
  (void)h;
  (void)n;
  return -1;
}
int32_t ota_read_partition(const bk_logic_partition_t* p, uint32_t a,
                           uint8_t* b, size_t n, uint32_t o) {
  (void)p;
  (void)a;
  (void)b;
  (void)n;
  (void)o;
  return -1;
}
uint32_t ota_calc_hash(uint32_t h, const void* data, size_t n) {
  const uint8_t* p = data;
  while (n--) h = (h ^ *p++) * 0x1000193u;
  return h;
}
const h2_fixed_layout_t* h2_bk_fixed_layout(void) { return &layout; }
int h2_bk_fixed_layout_active(void) { return 1; }
uint8_t h2_bk_fixed_current_slot(void) { return current_slot; }
int h2_bk_fixed_invalidate_app(void) {
  invalidated++;
  return 0;
}
int h2_bk_fixed_confirm_app(void) { return 0; }
int h2_bk_fixed_confirm_loader(void) { return 0; }
int h2_bk_fixed_relay_confirmed(void) { return relay_confirmed_rc; }
int h2_bk_fixed_boot_context_check(void) { return boot_context_rc; }
int h2_bk_fixed_loader_update_check(uint64_t image_size) {
  checked_size = image_size;
  if (mapped_check) return mapped_check;
  return mode == 0 ? 0 : H2_PAL_ERR_UNSUPPORTED;
}
int h2_bk_fixed_select(uint32_t p) {
  (void)p;
  selects++;
  return 0;
}
int h2_bk_fixed_publish_relay_head(uint32_t a, uint32_t b, uint32_t c,
                                   uint32_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  relay_heads++;
  return 0;
}
unsigned h2_bk_flash_aes_state(void) { return mode; }
int h2_bk_flash_access_ready(void) { return 0; }
int h2_bk_flash_read_plain(uint32_t p, uint32_t c, uint32_t o, void* dst,
                           size_t n) {
  mapped_reads++;
  assert(p == BASE && c == STORED && o + n <= LOGICAL);
  uint8_t* d = dst;
  while (n--) {
    uint32_t b = o / 32u;
    assert(h2_bk_flash_crc_valid(storage + b * 34u));
    *d++ = storage[b * 34u + o % 32u] ^ (mode == 1 ? 0x5a : 0);
    o++;
  }
  return 0;
}
int h2_bk_h2loader_current_app_identity(h2_runtime_t* r, const char* v,
                                        h2_loader_image_identity_t* i) {
  (void)r;
  (void)v;
  (void)i;
  return -1;
}
int h2_bk_h2loader_prepare_app_operation(h2_runtime_t* r) {
  (void)r;
  return 0;
}
int h2_bk_h2loader_app_operation_lock(void) { return 0; }
void h2_bk_h2loader_app_operation_unlock(void) {}
static void fixture(void) {
  memset(plain, 255, sizeof(plain));
  for (unsigned i = 0; i < 8192; i++) plain[i] = (uint8_t)i;
  const uint32_t cp_vector[2] = {0x28030000, 0x02240101},
                 ap_vector[2] = {0x28070000, 0x02241101};
  memcpy(plain, cp_vector, 8);
  memcpy(plain + 4096, ap_vector, 8);
  struct ota_rbl_head h = {
      .magic = {'R', 'B', 'L', 0}, .size_raw = 8192, .size_package = 8192};
  h.hash = ota_calc_hash(RT_OTA_HASH_FNV_SEED, plain, 8192);
  h.crc32 = h2_bk_flash_crc32(0, plain, 8192);
  h.info_crc32 = h2_bk_flash_crc32(0, (const uint8_t*)&h, 92);
  memcpy(plain + 8192, &h, sizeof(h));
  for (unsigned b = 0; b < LOGICAL / 32; b++) {
    uint8_t* p = image + b * 34;
    for (unsigned i = 0; i < 32; i++)
      p[i] = plain[b * 32 + i] ^ (mode == 1 ? 0x5a : 0);
    uint16_t crc = 0xffff;
    for (unsigned i = 0; i < 32; i++) {
      crc ^= (uint16_t)p[i] << 8;
      for (unsigned k = 0; k < 8; k++)
        crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x8005 : 0));
    }
    p[32] = (uint8_t)(crc >> 8);
    p[33] = (uint8_t)crc;
  }
}
static void seal_packet(uint8_t* p) {
  uint16_t crc = 0xffff;
  for (unsigned i = 0; i < 32; i++) {
    crc ^= (uint16_t)p[i] << 8;
    for (unsigned k = 0; k < 8; k++)
      crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x8005 : 0));
  }
  p[32] = (uint8_t)(crc >> 8);
  p[33] = (uint8_t)crc;
}

/* A canonical A-length Loader: linked for the Loader window, with its RBL head
 * in the last logical page and fully erased (not CRC-sealed) padding. */
static void loader_fixture(uint32_t reset_pc) {
  const uint32_t size_raw = 8192u, head_offset = WINDOW_LOGICAL - 4096u;
  memset(loader_plain, 255, sizeof(loader_plain));
  for (unsigned i = 0; i < size_raw; i++) loader_plain[i] = (uint8_t)(i * 5u);
  const uint32_t vector[2] = {0x28030000u, reset_pc};
  memcpy(loader_plain, vector, sizeof(vector));
  struct ota_rbl_head h = {.magic = {'R', 'B', 'L', 0},
                           .size_raw = size_raw,
                           .size_package = size_raw};
  h.hash = ota_calc_hash(RT_OTA_HASH_FNV_SEED, loader_plain, size_raw);
  h.crc32 = h2_bk_flash_crc32(0, loader_plain, size_raw);
  h.info_crc32 = h2_bk_flash_crc32(0, (const uint8_t*)&h, 92);
  memcpy(loader_plain + head_offset, &h, sizeof(h));
  memset(loader_image, 255, sizeof(loader_image));
  for (unsigned b = 0; b < WINDOW_LOGICAL / 32u; b++) {
    const uint8_t* source = loader_plain + b * 32u;
    int blank = 1;
    for (unsigned i = 0; i < 32; i++) blank &= source[i] == 255;
    if (blank) continue;
    memcpy(loader_image + b * 34u, source, 32);
    seal_packet(loader_image + b * 34u);
  }
}

#define LOADER_RESET_PC (0x02000000u + LOADER_BASE / 34u * 32u + 0x101u)

static void write_loader(const h2_loader_image_writer_api_t* w,
                         const uint8_t* bytes) {
  for (unsigned i = 0; i < WINDOW; i += 4096u) {
    const unsigned n = WINDOW - i < 4096u ? WINDOW - i : 4096u;
    assert(w->vtable->write(w->user, bytes + i, n) == 0);
  }
}

static void reset_mapped_case(void) {
  erased = invalidated = writes = selects = mapped_reads = relay_heads = 0;
  fail_write = fail_read = 0;
  current_slot = 0;
  relay_confirmed_rc = H2_PAL_OK;
  memset(storage, 255, sizeof(storage));
  memset(loader_storage, 255, sizeof(loader_storage));
}

static void test_mapped_loader_rejections_touch_nothing(
    const h2_loader_image_writer_api_t* w) {
  static const int refusals[] = {H2_PAL_ERR_FORMAT, H2_PAL_ERR_UNSUPPORTED,
                                 H2_PAL_ERR_IO, H2_PAL_ERR_INVALID_STATE};
  h2_loader_image_identity_t id = {.role = H2_LOADER_IMAGE_ROLE_H2LOADER,
                                   .image_size = WINDOW};
  for (mode = 0; mode < 2; mode++)
    for (size_t i = 0; i < sizeof(refusals) / sizeof(refusals[0]); i++) {
      reset_mapped_case();
      mapped_check = refusals[i];
      assert(w->vtable->begin(w->user, 2, &id) == refusals[i]);
      assert(!erased && !invalidated && !writes && !selects);
      assert(checked_size == WINDOW);
      assert(w->vtable->write(w->user, loader_image, 34) ==
             H2_PAL_ERR_INVALID_ARG);
      assert(!erased && !writes);
    }
  /* The Loader window is erased only under a confirmed native B. */
  reset_mapped_case();
  mode = 1;
  mapped_check = 1;
  current_slot = 1;
  relay_confirmed_rc = H2_PAL_ERR_INVALID_STATE;
  assert(w->vtable->begin(w->user, 1, &id) == H2_PAL_ERR_INVALID_STATE);
  relay_confirmed_rc = H2_PAL_ERR_IO;
  assert(w->vtable->begin(w->user, 1, &id) == H2_PAL_ERR_IO);
  assert(!erased && !invalidated && !writes && !selects);
  /* Neither window accepts a Loader from the wrong execution slot. */
  relay_confirmed_rc = H2_PAL_OK;
  assert(w->vtable->begin(w->user, 2, &id) == H2_PAL_ERR_UNSUPPORTED);
  current_slot = 0;
  assert(w->vtable->begin(w->user, 1, &id) == H2_PAL_ERR_UNSUPPORTED);
  assert(!erased && !invalidated && !writes);
  mapped_check = 0;
  mode = 0;
}

static void test_unusable_boot_context_touches_nothing(
    const h2_loader_image_writer_api_t* w) {
  static const h2_loader_image_role_t roles[] = {H2_LOADER_IMAGE_ROLE_APP,
                                                 H2_LOADER_IMAGE_ROLE_H2LOADER};
  reset_mapped_case();
  for (size_t i = 0; i < sizeof(roles) / sizeof(roles[0]); ++i) {
    h2_loader_image_identity_t id = {.role = roles[i], .image_size = WINDOW};
    erased = invalidated = selects = 0;
    boot_context_rc = H2_PAL_ERR_INVALID_STATE;
    assert(w->vtable->begin(w->user, 2, &id) == H2_PAL_ERR_INVALID_STATE);
    boot_context_rc = H2_PAL_ERR_IO;
    assert(w->vtable->begin(w->user, 2, &id) == H2_PAL_ERR_IO);
    assert(!erased && !invalidated && !selects);
  }
  boot_context_rc = H2_PAL_OK;
}

static void test_mapped_aes_on_loader_defers_plaintext(
    const h2_loader_image_writer_api_t* w,
    const h2_loader_image_reader_api_t* r) {
  static uint8_t ciphertext[WINDOW], copy[WINDOW];
  h2_loader_image_identity_t id = {.role = H2_LOADER_IMAGE_ROLE_H2LOADER,
                                   .image_size = WINDOW};
  /* Sealed noise: no RBL head or decodable vector exists in raw storage. */
  for (unsigned b = 0; b < WINDOW / 34u; b++) {
    for (unsigned i = 0; i < 32; i++)
      ciphertext[b * 34u + i] = (uint8_t)(b * 13u + i * 7u + 1u);
    seal_packet(ciphertext + b * 34u);
  }
  reset_mapped_case();
  mode = 1;
  mapped_check = 1;

  /* Stage into B on A: raw bytes unchanged, no relocated head, no plaintext
   * read of data the running image could not decrypt at that address. */
  assert(w->vtable->begin(w->user, 2, &id) == 0 && invalidated == 1);
  write_loader(w, ciphertext);
  assert(w->vtable->finish(w->user, &id) == 0);
  assert(!relay_heads && !mapped_reads && !memcmp(storage, ciphertext, WINDOW));
  assert(erased == 17);

  /* Copy back on confirmed B: exact raw bytes, no selection change. */
  current_slot = 1;
  invalidated = selects = 0;
  assert(r->vtable->read(r->user, 2, 0, copy, sizeof(copy)) == 0);
  assert(w->vtable->begin(w->user, 1, &id) == 0);
  write_loader(w, copy);
  assert(w->vtable->finish(w->user, &id) == 0);
  assert(!memcmp(loader_storage, ciphertext, WINDOW));
  assert(!invalidated && !selects && !relay_heads && !mapped_reads);

  /* A rejected write fails the copy; no success is reported afterwards. */
  fail_write = 1;
  assert(w->vtable->begin(w->user, 1, &id) == 0);
  assert(w->vtable->write(w->user, copy, 34) == H2_PAL_ERR_IO);
  assert(w->vtable->finish(w->user, &id) != 0);
  mapped_check = 0;
  mode = 0;
  fail_write = 0;
  current_slot = 0;
}

static void test_mapped_aes_off_loader_validates_canonical_rbl(
    const h2_loader_image_writer_api_t* w) {
  h2_loader_image_identity_t id = {.role = H2_LOADER_IMAGE_ROLE_H2LOADER,
                                   .image_size = WINDOW};
  reset_mapped_case();
  mode = 0;
  mapped_check = 1;
  loader_fixture(LOADER_RESET_PC);

  /* In the App window it still validates against its Loader link base, from
   * raw storage, and leaves no relocated head. */
  assert(w->vtable->begin(w->user, 2, &id) == 0);
  write_loader(w, loader_image);
  assert(w->vtable->finish(w->user, &id) == 0);
  assert(!relay_heads && !mapped_reads &&
         !memcmp(storage, loader_image, WINDOW));

  /* Same check on B, where the generic mapped reader is unavailable. */
  current_slot = 1;
  assert(w->vtable->begin(w->user, 1, &id) == 0);
  write_loader(w, loader_image);
  assert(w->vtable->finish(w->user, &id) == 0);
  assert(!mapped_reads && !memcmp(loader_storage, loader_image, WINDOW));

  /* Raw read failure. */
  assert(w->vtable->begin(w->user, 1, &id) == 0);
  write_loader(w, loader_image);
  fail_read = 1;
  assert(w->vtable->finish(w->user, &id) == H2_PAL_ERR_IO);
  fail_read = 0;

  /* A storage CRC failure inside the image. */
  loader_image[34] ^= 1u;
  assert(w->vtable->begin(w->user, 1, &id) == 0);
  write_loader(w, loader_image);
  assert(w->vtable->finish(w->user, &id) == H2_PAL_ERR_IO);

  /* Valid storage CRC around a changed payload byte. */
  loader_fixture(LOADER_RESET_PC);
  loader_image[34] ^= 1u;
  seal_packet(loader_image + 34);
  assert(w->vtable->begin(w->user, 1, &id) == 0);
  write_loader(w, loader_image);
  assert(w->vtable->finish(w->user, &id) == H2_PAL_ERR_FORMAT);

  /* An image linked for the App window is not a mapped Loader. */
  loader_fixture(0x02000000u + BASE / 34u * 32u + 0x101u);
  assert(w->vtable->begin(w->user, 1, &id) == 0);
  write_loader(w, loader_image);
  assert(w->vtable->finish(w->user, &id) == H2_PAL_ERR_FORMAT);

  /* No canonical head at the end of the A-length image. */
  loader_fixture(LOADER_RESET_PC);
  memset(loader_image + (WINDOW_LOGICAL - 4096u) / 32u * 34u, 255, 34);
  assert(w->vtable->begin(w->user, 1, &id) == 0);
  write_loader(w, loader_image);
  assert(w->vtable->finish(w->user, &id) == H2_PAL_ERR_FORMAT);
  mapped_check = 0;
  current_slot = 0;
}

static void test_deferral_never_reaches_app_images(
    const h2_loader_image_writer_api_t* w) {
  h2_loader_image_identity_t loader = {.role = H2_LOADER_IMAGE_ROLE_H2LOADER,
                                       .image_size = WINDOW};
  h2_loader_image_identity_t app = {.role = H2_LOADER_IMAGE_ROLE_APP,
                                    .image_size = STORED};
  const h2_bundle_app_writer_t* legacy = h2_bk_h2loader_ota_app_writer();
  h2_bundle_entry_t entry = {.path = H2_BK_H2LOADER_APP_ENTRY_PATH,
                             .size = STORED};
  reset_mapped_case();
  mode = 0;
  fixture();
  /* Past the vectors, so only the RBL hash can catch it. */
  image[34] ^= 1u;
  seal_packet(image + 34);

  for (unsigned round = 0; round < 2; round++) {
    mode = 1;
    mapped_check = 1;
    assert(w->vtable->begin(w->user, 2, &loader) == 0);
    w->vtable->abort(w->user);
    mode = 0;
    mapped_check = 0;
    if (round == 0) {
      assert(w->vtable->begin(w->user, 2, &app) == 0);
      assert(w->vtable->write(w->user, image, sizeof(image)) == 0);
      assert(w->vtable->finish(w->user, &app) == H2_PAL_ERR_FORMAT);
    } else {
      assert(legacy->begin(legacy->user, &entry) == 0);
      assert(legacy->write(legacy->user, &entry, image, sizeof(image)) == 0);
      assert(legacy->end(legacy->user, &entry) == H2_PAL_ERR_FORMAT);
    }
    assert(mapped_reads != 0u);
    mapped_reads = 0;
  }
}

int main(void) {
  const h2_loader_image_writer_api_t* w = h2_bk_h2loader_image_writer();
  const h2_loader_image_reader_api_t* r = h2_bk_h2loader_image_reader();
  test_unusable_boot_context_touches_nothing(w);
  test_mapped_loader_rejections_touch_nothing(w);
  test_mapped_aes_on_loader_defers_plaintext(w, r);
  test_mapped_aes_off_loader_validates_canonical_rbl(w);
  test_deferral_never_reaches_app_images(w);
  reset_mapped_case();
  h2_loader_image_identity_t id = {.role = H2_LOADER_IMAGE_ROLE_APP,
                                   .image_size = STORED};
  const unsigned chunks[] = {1, 31, 32, 33, 34, 35, 511, 512, 4096};
  for (mode = 0; mode < 2; mode++)
    for (unsigned c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
      fixture();
      erased = invalidated = 0;
      assert(w->vtable->begin(w->user, 2, &id) == 0);
      for (unsigned i = 0; i < STORED; i += chunks[c]) {
        unsigned n = STORED - i < chunks[c] ? STORED - i : chunks[c];
        assert(w->vtable->write(w->user, image + i, n) == 0);
      }
      assert(w->vtable->finish(w->user, &id) == 0 && invalidated == 1 &&
             erased == 4);
      uint8_t copy[STORED];
      assert(r->vtable->read(r->user, 2, 0, copy, sizeof(copy)) == 0 &&
             !memcmp(copy, image, sizeof(copy)));
    }
  /* A cancelled Loader relay cannot leak into the legacy App entrypoint. */
  mode = 0;
  id.role = H2_LOADER_IMAGE_ROLE_H2LOADER;
  assert(w->vtable->begin(w->user, 2, &id) == 0);
  w->vtable->abort(w->user);
  fixture();
  const h2_bundle_app_writer_t* legacy = h2_bk_h2loader_ota_app_writer();
  h2_bundle_entry_t entry = {.path = H2_BK_H2LOADER_APP_ENTRY_PATH,
                             .size = STORED};
  assert(legacy->begin(legacy->user, &entry) == 0);
  assert(legacy->write(legacy->user, &entry, image, sizeof(image)) == 0);
  assert(legacy->end(legacy->user, &entry) == 0 && !relay_heads);
  /* Valid storage CRC with the wrong mapped plaintext must not become ready. */
  mode = 1;
  fixture();
  mode = 0;
  id.role = H2_LOADER_IMAGE_ROLE_APP;
  assert(w->vtable->begin(w->user, 2, &id) == 0);
  assert(w->vtable->write(w->user, image, sizeof(image)) == 0);
  assert(w->vtable->finish(w->user, &id) != 0 && !committed);
  for (mode = 1; mode <= 2; mode++) {
    id.role = H2_LOADER_IMAGE_ROLE_H2LOADER;
    erased = invalidated = 0;
    assert(w->vtable->begin(w->user, 2, &id) == H2_PAL_ERR_UNSUPPORTED &&
           !erased && !invalidated);
  }
  id.role = H2_LOADER_IMAGE_ROLE_APP;
  mode = 0;
  fixture();
  fail_write = 1;
  assert(w->vtable->begin(w->user, 2, &id) == 0);
  assert(w->vtable->write(w->user, image, 34) != 0);
  assert(w->vtable->finish(w->user, &id) != 0 && !committed);
  return 0;
}
