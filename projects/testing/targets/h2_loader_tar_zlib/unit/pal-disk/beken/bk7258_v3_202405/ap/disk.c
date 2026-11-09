#include "launcher.h"

#include <driver/flash.h>
#include <driver/flash_partition.h>
#include <sdkconfig.h>
#include <string.h>

#if !CONFIG_SOC_SMP || !CONFIG_FLASH_CP_AP_DIRECT_ACCESS
#error "PAL Disk E2E requires the native AP SMP/direct Flash path"
#endif
#ifndef CONFIG_PAL_DISK_PARTITION_OFFSET
#error "Select a board layout with an App-owned pal_disk partition"
#endif

enum { SECTOR = 4096 };

static int get_partition(void* user, uint32_t id,
                         h2_pal_disk_partition_t* out) {
  (void)user;
  memset(out, 0, sizeof(*out));
  if (id != H2_BK_PAL_DISK_PARTITION) return H2_PAL_ERR_NOT_FOUND;
  const bk_logic_partition_t* region =
      bk_flash_partition_get_info(BK_PARTITION_PAL_DISK);
  const bk_logic_partition_t* app =
      bk_flash_partition_get_info(BK_PARTITION_APPLICATION1);
  if (!region || !app || !region->partition_description ||
      strcmp(region->partition_description, "pal_disk") ||
      region->partition_start_addr != CONFIG_PAL_DISK_PARTITION_OFFSET ||
      region->partition_start_addr % SECTOR ||
      region->partition_length < 2u * SECTOR ||
      region->partition_length % SECTOR ||
      (uint64_t)app->partition_start_addr + app->partition_length !=
          region->partition_start_addr ||
      (uint64_t)region->partition_start_addr + region->partition_length !=
          CONFIG_OTA_FINA_EXECUTIVE_PARTITION_OFFSET)
    return H2_PAL_ERR_INVALID_STATE;
  *out = (h2_pal_disk_partition_t){
      .id = id,
      .flags = H2_PAL_DISK_PARTITION_FLAG_READABLE |
               H2_PAL_DISK_PARTITION_FLAG_WRITABLE |
               H2_PAL_DISK_PARTITION_FLAG_ERASABLE,
      .size = region->partition_length,
      .erase_block_size = SECTOR,
      .write_alignment = 1,
      .name = "pal_disk",
  };
  return 0;
}

static int list_partitions(void* user, h2_pal_disk_partition_cb_t cb,
                           void* cb_user) {
  h2_pal_disk_partition_t partition;
  int rc = get_partition(user, H2_BK_PAL_DISK_PARTITION, &partition);
  return rc ? rc : cb(cb_user, &partition);
}

static int range(uint32_t id, uint64_t offset, uint64_t len,
                 uint32_t* address) {
  h2_pal_disk_partition_t partition;
  int rc = get_partition(NULL, id, &partition);
  if (rc) return rc;
  if (offset > partition.size || len > partition.size - offset)
    return H2_PAL_ERR_INVALID_ARG;
  *address = CONFIG_PAL_DISK_PARTITION_OFFSET + (uint32_t)offset;
  return 0;
}

static int read_bytes(void* user, uint32_t id, uint64_t offset, void* data,
                      size_t len) {
  (void)user;
  uint32_t address = 0;
  int rc = range(id, offset, len, &address);
  if (rc || !len) return rc;
  /* Preserve the real SDK locking path; no adapter lock wraps these calls. */
  return bk_flash_read_bytes(address, data, (uint32_t)len);
}

static int write_bytes(void* user, uint32_t id, uint64_t offset,
                       const void* data, size_t len) {
  (void)user;
  uint32_t address = 0;
  int rc = range(id, offset, len, &address);
  if (rc || !len) return rc;
  return bk_flash_write_bytes(address, (const uint8_t*)data, (uint32_t)len);
}

static int erase(void* user, uint32_t id, uint64_t offset, uint64_t len) {
  (void)user;
  uint32_t address = 0;
  int rc = range(id, offset, len, &address);
  if (rc) return rc;
  if (offset % SECTOR || len % SECTOR) return H2_PAL_ERR_INVALID_ARG;
  for (uint64_t erased = 0; erased < len; erased += SECTOR) {
    rc = bk_flash_erase_sector(address + (uint32_t)erased);
    if (rc) return rc;
  }
  return 0;
}

static int flush(void* user, uint32_t id) {
  h2_pal_disk_partition_t partition;
  /* The SDK calls above complete synchronously; this adapter has no cache. */
  return get_partition(user, id, &partition);
}

const h2_pal_disk_api_t* h2_bk_pal_disk_disk(void) {
  static const h2_pal_disk_vtable_t vtable = {
      .list_partitions = list_partitions,
      .get_partition = get_partition,
      .read = read_bytes,
      .write = write_bytes,
      .erase = erase,
      .flush = flush,
  };
  static const h2_pal_disk_api_t api = {.vtable = &vtable};
  return &api;
}
