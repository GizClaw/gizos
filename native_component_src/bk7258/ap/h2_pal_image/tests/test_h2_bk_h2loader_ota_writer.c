#include "h2_bk_platform_image.h"
#include "h2_bk_image_internal.h"
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
static unsigned erased, invalidated, committed, fail_write, relay_heads;
static int fail_restore, primary_fixture;
static int fixed_layout = 1;
static uint8_t running_slot;
part_flag update_part_flag;
static h2_fixed_layout_t layout = {.loader = {0x11000, 69632},
                                   .app = {BASE, 69632},
                                   .control_offset = 0x770000};
static bk_logic_partition_t parts[] = {
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
  return t == FLASH_PROTECT_ALL && fail_restore ? -1 : 0;
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
  return h2_bk_flash_read_plain(p->partition_start_addr, n,
                                n / 34u * 32u - RBL_HEAD_POS, h, sizeof(*h));
}
int32_t ota_read_partition(const bk_logic_partition_t* p, uint32_t a,
                           uint8_t* b, size_t n, uint32_t o) {
  (void)o;
  return h2_bk_flash_read_plain(p->partition_start_addr, p->partition_length, a,
                                b, n) == 0
             ? (int32_t)n
             : -1;
}
uint32_t ota_calc_hash(uint32_t h, const void* data, size_t n) {
  const uint8_t* p = data;
  while (n--) h = (h ^ *p++) * 0x1000193u;
  return h;
}
const h2_fixed_layout_t* h2_bk_fixed_layout(void) {
  return fixed_layout ? &layout : NULL;
}
int h2_bk_fixed_layout_active(void) { return fixed_layout; }
uint8_t h2_bk_fixed_current_slot(void) { return running_slot; }
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
int h2_bk_flash_access_ready(void) { return 0; }
int h2_bk_flash_read_plain(uint32_t p, uint32_t c, uint32_t o, void* dst,
                           size_t n) {
  if (p != BASE) return H2_PAL_ERR_NOT_FOUND;
  assert(c >= STORED && o + n <= LOGICAL);
  uint8_t* d = dst;
  while (n--) {
    uint32_t b = o / 32u;
    assert(h2_bk_flash_crc_valid(storage + b * 34u));
    *d++ = storage[b * 34u + o % 32u];
    o++;
  }
  return 0;
}
int h2_bk_fixed_recovery_ready(void) { return 0; }
static void fixture(void) {
  memset(plain, 255, sizeof(plain));
  for (unsigned i = 0; i < 8192; i++) plain[i] = (uint8_t)i;
  const uint32_t cp_vector[2] = {0x28030000,
                                 primary_fixture ? 0x02010101 : 0x02240101},
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
    for (unsigned i = 0; i < 32; i++) p[i] = plain[b * 32 + i];
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
static void test_running_image_size_excludes_data_tail(void) {
  const h2_pal_image_api_t* api = h2_bk_platform_image_api();
  const h2_fixed_layout_t saved_layout = layout;
  bk_logic_partition_t saved_parts[5];
  memcpy(saved_parts, parts, sizeof(parts));
  fixture();
  memcpy(storage, image, sizeof(image));
  h2_pal_image_info_t info;

  /* Only the running SDK table describes an exact, padded artifact length. */
  assert(h2_pal_image_get_info(api, 1, H2_PAL_IMAGE_VIEW_STORED, &info) == 0);
  assert((info.flags & H2_PAL_IMAGE_FLAG_SIZE_KNOWN) &&
         info.image_size == 69632u && info.capacity == 69632u);
  assert(h2_pal_image_get_info(api, 2, H2_PAL_IMAGE_VIEW_STORED, &info) == 0);
  assert(!(info.flags & H2_PAL_IMAGE_FLAG_SIZE_KNOWN));

  layout.app.size = 2u * 69632u;
  layout.app_table = 1;
  parts[1] = (bk_logic_partition_t){BASE, 34816};
  parts[2] = (bk_logic_partition_t){BASE + 34816u, 34816};
  parts[3] = (bk_logic_partition_t){0x11000, 69632};
  running_slot = 1u;
  assert(h2_pal_image_get_info(api, 2, H2_PAL_IMAGE_VIEW_STORED, &info) == 0);
  assert((info.flags & H2_PAL_IMAGE_FLAG_SIZE_KNOWN) &&
         !(info.flags & H2_PAL_IMAGE_FLAG_PREPARED) &&
         info.image_size == 69632u && info.capacity == 2u * 69632u &&
         info.execution_partition_id == 2u);
  assert(h2_pal_image_get_info(api, 2, H2_PAL_IMAGE_VIEW_NATIVE, &info) == 0);
  assert(!(info.flags & H2_PAL_IMAGE_FLAG_SIZE_KNOWN) &&
         info.capacity == 2u * 65536u);
  assert(h2_pal_image_get_info(api, 1, H2_PAL_IMAGE_VIEW_STORED, &info) == 0);
  assert(!(info.flags & H2_PAL_IMAGE_FLAG_SIZE_KNOWN));

  /* A Loader executing through native B still owns only its Loader image
   * bytes, even though its temporary destination has App-slot capacity. */
  layout = saved_layout;
  layout.app.size = 2u * 69632u;
  memcpy(parts, saved_parts, sizeof(parts));
  assert(h2_pal_image_get_info(api, 2, H2_PAL_IMAGE_VIEW_STORED, &info) == 0);
  assert((info.flags & H2_PAL_IMAGE_FLAG_SIZE_KNOWN) &&
         info.image_size == 69632u && info.capacity == 2u * 69632u);
  fixed_layout = 0;
  assert(h2_pal_image_get_info(api, 2, H2_PAL_IMAGE_VIEW_STORED, &info) == 0);
  assert(!(info.flags & H2_PAL_IMAGE_FLAG_SIZE_KNOWN));
  fixed_layout = 1;
  running_slot = 0u;
  layout = saved_layout;
}

int main(void) {
  test_running_image_size_excludes_data_tail();
  const h2_pal_image_api_t* api = h2_bk_platform_image_api();
  h2_pal_image_write_request_t request = {
      .partition_id = 2, .execution_partition_id = 2, .image_size = STORED};
  const unsigned chunks[] = {1, 31, 32, 33, 34, 35, 511, 512, 4096};
  for (unsigned c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
    fixture();
    erased = invalidated = 0;
    assert(h2_pal_image_begin(api, &request) == 0 &&
           update_part_flag == UPDATE_B_PART);
    assert(h2_pal_image_begin(api, &request) == H2_PAL_ERR_BUSY);
    for (unsigned i = 0; i < STORED; i += chunks[c]) {
      unsigned n = STORED - i < chunks[c] ? STORED - i : chunks[c];
      assert(h2_pal_image_write(api, image + i, n) == 0);
    }
    assert(h2_pal_image_finish(api) == 0 && invalidated == 1 && erased == 4);
    assert(!committed && !relay_heads);
    h2_pal_image_info_t info;
    assert(h2_pal_image_get_info(api, 2, H2_PAL_IMAGE_VIEW_STORED, &info) == 0);
    assert(info.capacity == sizeof(storage) && info.image_size == STORED &&
           (info.flags & H2_PAL_IMAGE_FLAG_PREPARED) &&
           info.execution_partition_id == 2);
    assert(h2_pal_image_begin(api, &request) == H2_PAL_ERR_BUSY);
    uint8_t copy[STORED];
    assert(h2_pal_image_read(api, 2, H2_PAL_IMAGE_VIEW_STORED, 0, copy,
                             sizeof(copy)) == 0 &&
           !memcmp(copy, image, sizeof(copy)));
    assert(h2_pal_image_abort(api) == 0);
    assert(h2_pal_image_read(api, 2, H2_PAL_IMAGE_VIEW_STORED, 0, copy,
                             sizeof(copy)) == 0 &&
           !memcmp(copy, image, sizeof(copy)));
  }
  /* Native linkage is checked independently of the caller's constraint. */
  fixture();
  request.execution_partition_id = 1;
  assert(h2_pal_image_begin(api, &request) == 0);
  assert(h2_pal_image_write(api, image, sizeof(image)) == 0);
  assert(h2_pal_image_finish(api) == H2_PAL_ERR_FORMAT && !relay_heads);
  assert(h2_pal_image_abort(api) == 0);
  /* Primary-linked images use the SDK footer/FNV view. No relay footer is
   * published until Power selects after independent stored-byte verification.
   */
  primary_fixture = 1;
  fixture();
  assert(h2_pal_image_begin(api, &request) == 0);
  assert(h2_pal_image_write(api, image, sizeof(image)) == 0);
  assert(h2_pal_image_finish(api) == 0 && !relay_heads);
  assert(h2_bk_image_publish_native_footer(2) == 0 && relay_heads == 1);
  h2_bk_image_selected(2);
  assert(!h2_bk_image_prepared(2));
  assert(h2_pal_image_abort(api) == 0);
  relay_heads = 0;
  primary_fixture = 0;
  request.execution_partition_id = 2;
  fixture();
  /* Uncertain protection cleanup must stay busy until explicit abort succeeds.
   */
  assert(h2_pal_image_begin(api, &request) == 0);
  assert(h2_pal_image_write(api, image, sizeof(image)) == 0);
  fail_restore = 1;
  assert(h2_pal_image_finish(api) == H2_PAL_ERR_IO);
  assert(h2_pal_image_abort(api) == H2_PAL_ERR_IO);
  assert(h2_pal_image_finish(api) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_pal_image_write(api, image, 0) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_pal_image_begin(api, &request) == H2_PAL_ERR_BUSY);
  fail_restore = 0;
  assert(h2_pal_image_abort(api) == 0);
  request.execution_partition_id = 0;
  assert(h2_pal_image_begin(api, &request) == H2_PAL_ERR_INVALID_ARG);
  request.execution_partition_id = 2;
  request.partition_id = 1;
  assert(h2_pal_image_begin(api, &request) == H2_PAL_ERR_INVALID_STATE);
  request.partition_id = 2;
  fixture();
  fail_write = 1;
  assert(h2_pal_image_begin(api, &request) == 0);
  assert(h2_pal_image_write(api, image, 34) != 0);
  assert(h2_pal_image_finish(api) != 0 && !committed);
  assert(h2_pal_image_abort(api) == 0);
  return 0;
}
