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
static uint8_t storage[69632], image[STORED], plain[LOGICAL];
static unsigned mode, erased, invalidated, committed, fail_write, relay_heads;
part_flag update_part_flag;
static const h2_fixed_layout_t layout = {.loader = {0x11000, 69632},
                                         .app = {BASE, 69632},
                                         .control_offset = 0x770000};
static const bk_logic_partition_t parts[] = {
    {0}, {0x11000, 34816}, {0x19800, 34816}, {BASE, 69632}, {0x770000, 4096}};
const bk_logic_partition_t* bk_flash_partition_get_info(unsigned i) {
  return i < 5 ? &parts[i] : NULL;
}
int bk_flash_read_bytes(uint32_t a, uint8_t* p, uint32_t n) {
  assert(a >= BASE && a + n <= BASE + sizeof(storage));
  memcpy(p, storage + (a - BASE), n);
  return 0;
}
int bk_flash_write_bytes(uint32_t a, const uint8_t* p, uint32_t n) {
  assert(a >= BASE && a + n <= BASE + sizeof(storage));
  if (fail_write) return -1;
  for (unsigned i = 0; i < n; i++) storage[a - BASE + i] &= p[i];
  return 0;
}
int bk_flash_erase_sector(uint32_t a) {
  assert(a >= BASE && a + 4096 <= BASE + sizeof(storage));
  memset(storage + (a - BASE), 255, 4096);
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
uint8_t h2_bk_fixed_current_slot(void) { return 0; }
int h2_bk_fixed_invalidate_app(void) {
  invalidated++;
  return 0;
}
int h2_bk_fixed_confirm_app(void) { return 0; }
int h2_bk_fixed_confirm_loader(void) { return 0; }
int h2_bk_fixed_select(uint32_t p) {
  (void)p;
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
int main(void) {
  const h2_loader_image_writer_api_t* w = h2_bk_h2loader_image_writer();
  const h2_loader_image_reader_api_t* r = h2_bk_h2loader_image_reader();
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
