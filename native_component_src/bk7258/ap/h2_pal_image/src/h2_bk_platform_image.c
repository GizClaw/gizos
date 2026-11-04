#include "h2_bk_platform_image.h"
#include "h2_bk_image_internal.h"
#include "h2_bk_fixed_boot.h"
#include "h2_bk_flash_access.h"
#include "flash_access.h"
#include "layout.h"

#ifdef H2_BK_OTA_WRITER_HOST_TEST
#include "h2_bk_h2loader_ota_writer_test_sdk.h"
#else
#include "bk_private/bk_ota_private.h"
#include "common/bk_err.h"
#include "driver/flash.h"
#include "driver/flash_partition.h"
#include "driver/wdt.h"
#include "modules/ota.h"
#include "os/os.h"
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern part_flag update_part_flag;
extern uint32_t ota_calc_hash(uint32_t hash, const void* buf, size_t len);
extern int32_t ota_read_partition(const bk_logic_partition_t* part,
                                  uint32_t addr, uint8_t* buf, size_t size,
                                  uint32_t offset);

#define PROGRESS_STEP (256u * 1024u)
#define SECTOR_SIZE 4096u
#define VERIFY_CHUNK_SIZE 4096u
#define RBL_FOOTER_PHYSICAL_SIZE ((RBL_HEAD_POS * 34u) / 32u)

static const bk_logic_partition_t* s_partition;
static flash_protect_type_t s_protect_type;
static uint32_t s_erased, s_received, s_total, s_next_progress,
    s_next_erase_progress;
static uint32_t s_partition_id, s_execution_partition_id;
static int s_flash_open, s_prepared, s_cleanup_required;
static uint8_t s_verify_buffer[VERIFY_CHUNK_SIZE];
static bk_logic_partition_t s_primary_window_partition, s_fixed_partitions[2];

static void feed_watchdogs(void) {
  /* force_feed enables an AON watchdog not serviced by the CP feed. */
  (void)bk_wdt_feed();
}

static void log_progress(const char* stage, uint32_t offset) {
  char line[128];
  snprintf(line, sizeof(line),
           "H2_BK_OTA_WRITER stage=%s offset=%lu total=%lu\r\n", stage,
           (unsigned long)offset, (unsigned long)s_total);
  os_printf("%s", line);
}

static const bk_logic_partition_t* image_partition(uint32_t id) {
  const h2_fixed_layout_t* layout = h2_bk_fixed_layout();
  if (id != H2_BK_PRIMARY_PARTITION_ID && id != H2_BK_APP_PARTITION_ID)
    return NULL;
  if (layout != NULL) {
    const bk_logic_partition_t* cp =
        bk_flash_partition_get_info(BK_PARTITION_APPLICATION);
    if (cp == NULL) return NULL;
    const h2_fixed_window_t* w =
        id == H2_BK_PRIMARY_PARTITION_ID ? &layout->loader : &layout->app;
    bk_logic_partition_t* p = &s_fixed_partitions[id == H2_BK_APP_PARTITION_ID];
    *p = *cp;
    p->partition_start_addr = w->offset;
    p->partition_length = w->size;
    return p;
  }
  if (id == H2_BK_APP_PARTITION_ID)
    return bk_flash_partition_get_info(BK_PARTITION_S_APP);
  const bk_logic_partition_t* cp =
      bk_flash_partition_get_info(BK_PARTITION_APPLICATION);
  const bk_logic_partition_t* ap =
      bk_flash_partition_get_info(BK_PARTITION_APPLICATION1);
  const bk_logic_partition_t* other =
      bk_flash_partition_get_info(BK_PARTITION_S_APP);
  if (!cp || !ap || !other ||
      cp->partition_length > UINT32_MAX - ap->partition_length ||
      cp->partition_start_addr > UINT32_MAX - cp->partition_length ||
      ap->partition_start_addr > UINT32_MAX - ap->partition_length ||
      cp->partition_start_addr + cp->partition_length !=
          ap->partition_start_addr ||
      ap->partition_start_addr + ap->partition_length !=
          other->partition_start_addr ||
      cp->partition_length + ap->partition_length != other->partition_length)
    return NULL;
  s_primary_window_partition = *cp;
  s_primary_window_partition.partition_length =
      cp->partition_length + ap->partition_length;
  return &s_primary_window_partition;
}

static uint32_t running_partition(void) {
  return h2_bk_fixed_current_slot() == 0u ? H2_BK_PRIMARY_PARTITION_ID
                                          : H2_BK_APP_PARTITION_ID;
}

/* Release protection without discarding prepared-candidate facts. */
static int close_flash(void) {
  if (s_flash_open) {
    if (bk_flash_set_protect_type(s_protect_type) != BK_OK) {
      s_cleanup_required = 1;
      return H2_PAL_ERR_IO;
    }
    s_cleanup_required = 0;
    s_flash_open = 0;
  }
  s_partition = NULL;
  return H2_PAL_OK;
}

int h2_bk_image_write_active(void) { return s_flash_open; }
int h2_bk_image_prepared(uint32_t id) {
  return s_prepared && s_partition_id == id;
}
void h2_bk_image_selected(uint32_t id) {
  if (s_partition_id == id) s_prepared = 0;
}

static int read_vector(uint32_t id, uint32_t vector[2]) {
  const bk_logic_partition_t* p = image_partition(id);
  if (!p || p->partition_length < 34u) return H2_PAL_ERR_NOT_FOUND;
  if (h2_bk_flash_access_ready() == H2_PAL_OK)
    return h2_bk_flash_read_plain(p->partition_start_addr, p->partition_length,
                                  0u, vector, 8u);
  /* A remapped primary-linked candidate cannot use the fixed XIP view. Raw
   * vector bytes are executable facts only when AES is positively disabled. */
  if (h2_bk_flash_aes_state() != H2_BK_FLASH_AES_OFF)
    return H2_PAL_ERR_UNSUPPORTED;
  uint8_t packet[34];
  if (bk_flash_read_bytes(p->partition_start_addr, packet, sizeof(packet)) !=
      BK_OK)
    return H2_PAL_ERR_IO;
  if (h2_bk_flash_erased(packet) || !h2_bk_flash_crc_valid(packet))
    return H2_PAL_ERR_FORMAT;
  memcpy(vector, packet, 8u);
  return H2_PAL_OK;
}

int h2_bk_image_execution_partition(uint32_t id, uint32_t* out) {
  if (!out) return H2_PAL_ERR_INVALID_ARG;
  *out = 0;
  if (h2_bk_image_prepared(id)) {
    *out = s_execution_partition_id;
    return H2_PAL_OK;
  }
  /* Only fixed windows map a reset vector one-to-one onto a native partition.
   * Legacy SDK A/B images are linked for A and run from B through the SDK's
   * remap or copy, and their stored form may be a packaged RBL. */
  if (!h2_bk_fixed_layout_active()) return H2_PAL_ERR_UNSUPPORTED;
  uint32_t vector[2];
  int rc = read_vector(id, vector);
  if (rc != H2_PAL_OK) return rc;
  if (vector[0] < 0x28000000u || vector[0] > 0x280a0000u || (vector[0] & 7u) ||
      !(vector[1] & 1u))
    return H2_PAL_ERR_FORMAT;
  for (uint32_t candidate = H2_BK_PRIMARY_PARTITION_ID;
       candidate <= H2_BK_APP_PARTITION_ID; ++candidate) {
    const bk_logic_partition_t* p = image_partition(candidate);
    if (!p) continue;
    const uint32_t base = H2_FIXED_XIP_ADDRESS(p->partition_start_addr);
    const uint32_t capacity = p->partition_length / 34u * 32u;
    if (vector[1] >= base && vector[1] - base < capacity) {
      *out = candidate;
      return H2_PAL_OK;
    }
  }
  return H2_PAL_ERR_FORMAT;
}

static h2_pal_result_t image_get_info(void* user, uint32_t id,
                                      h2_pal_image_view_t view,
                                      h2_pal_image_info_t* out) {
  (void)user;
  if (!out ||
      (view != H2_PAL_IMAGE_VIEW_STORED && view != H2_PAL_IMAGE_VIEW_NATIVE))
    return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  const bk_logic_partition_t* p = image_partition(id);
  if (!p) return H2_PAL_ERR_NOT_FOUND;
  out->capacity = view == H2_PAL_IMAGE_VIEW_STORED
                      ? p->partition_length
                      : p->partition_length / 34u * 32u;
  out->flags = H2_PAL_IMAGE_FLAG_READABLE;
  if (view == H2_PAL_IMAGE_VIEW_STORED && id != running_partition())
    out->flags |= H2_PAL_IMAGE_FLAG_WRITABLE;
  if (view == H2_PAL_IMAGE_VIEW_NATIVE &&
      h2_bk_flash_access_ready() != H2_PAL_OK)
    return H2_PAL_ERR_UNSUPPORTED;
  if (h2_bk_image_prepared(id)) {
    out->flags |= H2_PAL_IMAGE_FLAG_PREPARED;
    if (view == H2_PAL_IMAGE_VIEW_STORED) {
      out->flags |= H2_PAL_IMAGE_FLAG_SIZE_KNOWN;
      out->image_size = s_total;
    }
  }
  uint32_t execution;
  if (h2_bk_image_execution_partition(id, &execution) == H2_PAL_OK) {
    out->execution_partition_id = execution;
    out->flags |= H2_PAL_IMAGE_FLAG_EXECUTION_PARTITION_KNOWN;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t image_read(void* user, uint32_t id,
                                  h2_pal_image_view_t view, uint64_t offset,
                                  void* data, size_t len) {
  (void)user;
  const bk_logic_partition_t* p = image_partition(id);
  if (!p || (!data && len)) return H2_PAL_ERR_INVALID_ARG;
  const uint32_t capacity = view == H2_PAL_IMAGE_VIEW_STORED
                                ? p->partition_length
                                : p->partition_length / 34u * 32u;
  if (offset > capacity || len > capacity - offset)
    return H2_PAL_ERR_INVALID_ARG;
  if (view == H2_PAL_IMAGE_VIEW_NATIVE)
    return h2_bk_flash_read_plain(p->partition_start_addr, p->partition_length,
                                  (uint32_t)offset, data, len);
  if (view != H2_PAL_IMAGE_VIEW_STORED) return H2_PAL_ERR_INVALID_ARG;
  return bk_flash_read_bytes(p->partition_start_addr + (uint32_t)offset, data,
                             (uint32_t)len) == BK_OK
             ? H2_PAL_OK
             : H2_PAL_ERR_IO;
}

static h2_pal_result_t image_begin(
    void* user, const h2_pal_image_write_request_t* request) {
  (void)user;
  if (!request || !request->image_size || request->image_size > UINT32_MAX ||
      (request->partition_id != H2_BK_PRIMARY_PARTITION_ID &&
       request->partition_id != H2_BK_APP_PARTITION_ID) ||
      (request->execution_partition_id != H2_BK_PRIMARY_PARTITION_ID &&
       request->execution_partition_id != H2_BK_APP_PARTITION_ID))
    return H2_PAL_ERR_INVALID_ARG;
  if (s_flash_open || s_prepared) return H2_PAL_ERR_BUSY;
  if (request->partition_id == running_partition())
    return H2_PAL_ERR_INVALID_STATE;
  /* No erase or control-record mutation before these immutable guards. The
   * requested link window is a constraint; finish validates the actual vector.
   */
  const int primary_linked =
      request->execution_partition_id == H2_BK_PRIMARY_PARTITION_ID;
  if (primary_linked && h2_bk_flash_aes_state() != H2_BK_FLASH_AES_OFF)
    return H2_PAL_ERR_UNSUPPORTED;
  const int fixed = h2_bk_fixed_layout_active();
  if (fixed && primary_linked &&
      request->partition_id == H2_BK_PRIMARY_PARTITION_ID &&
      !h2_bk_fixed_recovery_ready())
    return H2_PAL_ERR_INVALID_STATE;
  if (fixed && !primary_linked) {
    /* A fixed App window executes in place from whole 34-byte CRC units. */
    if (request->partition_id != H2_BK_APP_PARTITION_ID)
      return H2_PAL_ERR_UNSUPPORTED;
    int rc = h2_bk_flash_access_ready();
    if (rc != H2_PAL_OK) return rc;
    if (request->image_size % 34u) return H2_PAL_ERR_FORMAT;
  }
  const bk_logic_partition_t* target = image_partition(request->partition_id);
  const bk_logic_partition_t* execution =
      image_partition(request->execution_partition_id);
  if (!target || !execution || request->image_size > target->partition_length ||
      request->image_size > execution->partition_length)
    return H2_PAL_ERR_INVALID_ARG;
  os_printf("H2_BK_OTA_WRITER stage=begin_enter\r\n");
  feed_watchdogs();
  if (fixed && request->partition_id == H2_BK_APP_PARTITION_ID) {
    int rc = h2_bk_fixed_invalidate_app();
    if (rc != H2_PAL_OK) return rc;
  }
  s_protect_type = bk_flash_get_protect_type();
  s_flash_open = 1;
  if (bk_flash_set_protect_type(FLASH_PROTECT_NONE) != BK_OK) {
    /* If restoration also fails, retain the open transaction as a poison
     * barrier. Explicit abort can retry cleanup; begin must remain busy. */
    (void)close_flash();
    return H2_PAL_ERR_IO;
  }
  s_partition = target;
  s_partition_id = request->partition_id;
  s_execution_partition_id = request->execution_partition_id;
  s_total = (uint32_t)request->image_size;
  s_received = s_erased = s_next_progress = s_next_erase_progress = 0;
  update_part_flag = s_partition_id == H2_BK_PRIMARY_PARTITION_ID
                         ? UPDATE_A_PART
                         : UPDATE_B_PART;
  char line[128];
  snprintf(line, sizeof(line),
           "H2_BK_OTA_WRITER stage=begin bytes=%lu target=%lu\r\n",
           (unsigned long)s_total, (unsigned long)s_partition_id);
  os_printf("%s", line);
  return H2_PAL_OK;
}

static h2_pal_result_t image_write(void* user, const void* data, size_t len) {
  (void)user;
  if (s_cleanup_required || !s_flash_open || !s_partition)
    return H2_PAL_ERR_INVALID_STATE;
  if ((!data && len) || len > s_total - s_received)
    return H2_PAL_ERR_INVALID_ARG;
  if (!len) return H2_PAL_OK;
  const uint32_t end = s_received + (uint32_t)len;
  int rc = BK_OK;
  while (s_erased < end) {
    if (s_erased >= s_next_erase_progress) {
      log_progress("erase", s_erased);
      while (s_next_erase_progress <= s_erased &&
             UINT32_MAX - s_next_erase_progress >= PROGRESS_STEP)
        s_next_erase_progress += PROGRESS_STEP;
    }
    rc = bk_flash_erase_sector(s_partition->partition_start_addr + s_erased);
    feed_watchdogs();
    if (rc != BK_OK) break;
    s_erased += SECTOR_SIZE;
  }
  if (rc == BK_OK) {
    if (s_received >= s_next_progress) {
      log_progress("write", s_received);
      while (s_next_progress <= s_received &&
             UINT32_MAX - s_next_progress >= PROGRESS_STEP)
        s_next_progress += PROGRESS_STEP;
    }
    rc = bk_flash_write_bytes(s_partition->partition_start_addr + s_received,
                              data, (uint32_t)len);
  }
  feed_watchdogs();
  if (end == s_total) {
    char line[96];
    snprintf(line, sizeof(line), "H2_BK_OTA_WRITER stage=final_write rc=%d\r\n",
             rc);
    os_printf("%s", line);
  }
  if (rc != BK_OK) {
    close_flash();
    return H2_PAL_ERR_IO;
  }
  s_received = end;
  return H2_PAL_OK;
}

static int validate_execution_vector(void) {
  const bk_logic_partition_t* expected =
      image_partition(s_execution_partition_id);
  uint32_t vector[2];
  int rc = read_vector(s_partition_id, vector);
  if (rc != H2_PAL_OK) return rc;
  const uint32_t base = H2_FIXED_XIP_ADDRESS(expected->partition_start_addr);
  const uint32_t logical_size = s_total / 34u * 32u;
  if (vector[0] < 0x28000000u || vector[0] > 0x280a0000u || (vector[0] & 7u) ||
      !(vector[1] & 1u) || vector[1] < base || vector[1] - base >= logical_size)
    return H2_PAL_ERR_FORMAT;
  return H2_PAL_OK;
}

static int verify_legacy_rbl(void) {
  struct ota_rbl_head header;
  uint32_t hash = RT_OTA_HASH_FNV_SEED;
  const uint32_t capacity = s_total / 34u * 32u;
  /* SDK RBL acceptance keeps its established IO result for every mismatch. */
  if (s_total < RBL_FOOTER_PHYSICAL_SIZE) return H2_PAL_ERR_IO;
  memset(&header, 0, sizeof(header));
  if (ota_get_rbl_head(s_partition, &header, s_total) != BK_OK ||
      memcmp(header.magic, "RBL", 3u) || !header.size_raw ||
      header.size_raw > capacity)
    return H2_PAL_ERR_IO;
  uint32_t next_progress = 0u;
  for (uint32_t offset = 0; offset < header.size_raw;) {
    uint32_t take = header.size_raw - offset;
    if (take > sizeof(s_verify_buffer)) take = sizeof(s_verify_buffer);
    if (offset >= next_progress) {
      char line[128];
      snprintf(line, sizeof(line),
               "H2_BK_OTA_WRITER stage=verify offset=%lu total=%lu\r\n",
               (unsigned long)offset, (unsigned long)header.size_raw);
      os_printf("%s", line);
      while (next_progress <= offset &&
             UINT32_MAX - next_progress >= PROGRESS_STEP)
        next_progress += PROGRESS_STEP;
    }
    if (ota_read_partition(s_partition, offset, s_verify_buffer, take, 0u) !=
        (int32_t)take)
      return H2_PAL_ERR_IO;
    hash = ota_calc_hash(hash, s_verify_buffer, take);
    offset += take;
    feed_watchdogs();
  }
  return hash == header.hash ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static int verify_fixed_rbl(void) {
  struct ota_rbl_head header;
  _Static_assert(sizeof(header) == 96u, "Pinned RBL ABI changed");
  const uint32_t capacity = s_total / 34u * 32u;
  if (s_total % 34u || capacity < RBL_HEAD_POS) return H2_PAL_ERR_FORMAT;
  int rc =
      h2_bk_flash_read_plain(s_partition->partition_start_addr, s_total,
                             capacity - RBL_HEAD_POS, &header, sizeof(header));
  if (rc != H2_PAL_OK) return rc;
  if (memcmp(header.magic, "RBL\0", 4u) || header.algo != 0u ||
      !header.size_raw || header.size_raw % 32u ||
      header.size_package != header.size_raw ||
      header.size_raw > capacity - RBL_HEAD_POS ||
      h2_bk_flash_crc32(0u, (const uint8_t*)&header, 92u) != header.info_crc32)
    return H2_PAL_ERR_FORMAT;
  uint32_t vector[2];
  rc = h2_bk_flash_read_plain(s_partition->partition_start_addr, s_total, 0u,
                              vector, sizeof(vector));
  if (rc != H2_PAL_OK) return rc;
  const bk_logic_partition_t* expected =
      image_partition(s_execution_partition_id);
  const uint32_t base = H2_FIXED_XIP_ADDRESS(expected->partition_start_addr);
  if (vector[1] < base || vector[1] - base >= header.size_raw)
    return H2_PAL_ERR_FORMAT;
  uint32_t hash = RT_OTA_HASH_FNV_SEED, crc = 0;
  for (uint32_t offset = 0; offset < header.size_raw;) {
    uint32_t take = header.size_raw - offset;
    if (take > sizeof(s_verify_buffer)) take = sizeof(s_verify_buffer);
    rc = h2_bk_flash_read_plain(s_partition->partition_start_addr, s_total,
                                offset, s_verify_buffer, take);
    if (rc != H2_PAL_OK) return rc;
    hash = ota_calc_hash(hash, s_verify_buffer, take);
    crc = h2_bk_flash_crc32(crc, s_verify_buffer, take);
    offset += take;
    feed_watchdogs();
  }
  return hash == header.hash && crc == header.crc32 ? H2_PAL_OK
                                                    : H2_PAL_ERR_FORMAT;
}

static h2_pal_result_t image_finish(void* user) {
  (void)user;
  if (s_cleanup_required) return H2_PAL_ERR_INVALID_STATE;
  if (!s_flash_open || !s_partition || s_received != s_total) {
    close_flash();
    return H2_PAL_ERR_INVALID_STATE;
  }
  char line[96];
  snprintf(line, sizeof(line), "H2_BK_OTA_WRITER stage=end bytes=%lu\r\n",
           (unsigned long)s_received);
  os_printf("%s", line);
  feed_watchdogs();
  const int fixed = h2_bk_fixed_layout_active();
  /* A fixed window executes in place: its reset vector must lie in the
   * requested window. Legacy SDK A/B keeps the SDK's own acceptance check. */
  int rc = fixed ? validate_execution_vector() : H2_PAL_OK;
  if (rc == H2_PAL_OK)
    rc = fixed && s_execution_partition_id == H2_BK_APP_PARTITION_ID
             ? verify_fixed_rbl()
             : verify_legacy_rbl();
  const int close_rc = close_flash();
  if (close_rc != H2_PAL_OK) return close_rc;
  if (rc == H2_PAL_OK) s_prepared = 1;
  return rc;
}

int h2_bk_image_publish_native_footer(uint32_t id) {
  if (!h2_bk_image_prepared(id) || id != H2_BK_APP_PARTITION_ID ||
      s_execution_partition_id != H2_BK_PRIMARY_PARTITION_ID)
    return H2_PAL_OK;
  if (h2_bk_flash_aes_state() != H2_BK_FLASH_AES_OFF)
    return H2_PAL_ERR_UNSUPPORTED;
  const bk_logic_partition_t* p = image_partition(id);
  if (!p) return H2_PAL_ERR_NOT_FOUND;
  /* Selection is the publication barrier. A successful finish leaves all boot
   * headers untouched until stored-byte verification has succeeded. */
  return h2_bk_fixed_publish_relay_head(p->partition_start_addr,
                                        p->partition_length, s_total,
                                        RBL_FOOTER_PHYSICAL_SIZE);
}

static h2_pal_result_t image_abort(void* user) {
  (void)user;
  int rc = close_flash();
  if (rc != H2_PAL_OK) return rc;
  s_prepared = 0;
  s_partition_id = s_execution_partition_id = s_total = s_received = s_erased =
      0;
  return H2_PAL_OK;
}

const h2_pal_image_api_t* h2_bk_platform_image_api(void) {
  static const h2_pal_image_vtable_t vtable = {
      .get_info = image_get_info,
      .read = image_read,
      .begin = image_begin,
      .write = image_write,
      .finish = image_finish,
      .abort = image_abort,
  };
  static const h2_pal_image_api_t api = {.vtable = &vtable};
  return &api;
}
