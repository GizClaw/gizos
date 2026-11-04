#include "h2_bk_platform_storage.h"
#ifdef H2_BK_DISK_HOST_TEST
#include "h2_bk_disk_test_sdk.h"
#else
#include "driver/flash.h"
#include "os/os.h"
#endif
#include <stdio.h>
#include <string.h>
#define SECTOR_SIZE 4096u
static h2_bk_disk_region_t s_region;
static char s_name[H2_PAL_DISK_PARTITION_NAME_MAX];

int h2_bk_platform_disk_configure(const h2_bk_disk_region_t* region) {
  if (!region || !region->name || !region->size ||
      region->offset % SECTOR_SIZE || region->size % SECTOR_SIZE ||
      region->offset >= 0x1000000u ||
      region->size > 0x1000000u - region->offset)
    return H2_PAL_ERR_INVALID_ARG;
  if (s_region.size)
    return s_region.id == region->id && s_region.offset == region->offset &&
                   s_region.size == region->size &&
                   !strcmp(s_name, region->name)
               ? H2_PAL_OK
               : H2_PAL_ERR_INVALID_STATE;
  if (strlen(region->name) >= sizeof(s_name)) return H2_PAL_ERR_INVALID_ARG;
  s_region = *region;
  (void)snprintf(s_name, sizeof(s_name), "%s", region->name);
  s_region.name = s_name;
  return H2_PAL_OK;
}
static int range(uint32_t id, uint64_t offset, uint64_t len) {
  return s_region.size && id == s_region.id && offset <= s_region.size &&
         len <= s_region.size - offset;
}
static h2_pal_result_t get_partition(void* user, uint32_t id,
                                     h2_pal_disk_partition_t* out) {
  (void)user;
  if (!out) return H2_PAL_ERR_INVALID_ARG;
  if (!range(id, 0, 0)) return H2_PAL_ERR_NOT_FOUND;
  memset(out, 0, sizeof(*out));
  out->id = id;
  out->flags = H2_PAL_DISK_PARTITION_FLAG_READABLE |
               H2_PAL_DISK_PARTITION_FLAG_WRITABLE |
               H2_PAL_DISK_PARTITION_FLAG_ERASABLE;
  out->size = s_region.size;
  out->erase_block_size = SECTOR_SIZE;
  out->write_alignment = 1u;
  (void)snprintf(out->name, sizeof(out->name), "%s", s_name);
  return H2_PAL_OK;
}
static h2_pal_result_t read_bytes(void* user, uint32_t id, uint64_t offset,
                                  void* data, size_t len) {
  (void)user;
  if ((!data && len) || !range(id, offset, len)) return H2_PAL_ERR_INVALID_ARG;
  return bk_flash_read_bytes(s_region.offset + (uint32_t)offset, data,
                             (uint32_t)len) == 0
             ? H2_PAL_OK
             : H2_PAL_ERR_IO;
}
static h2_pal_result_t erase_bytes(void* user, uint32_t id, uint64_t offset,
                                   uint64_t len) {
  (void)user;
  if (!range(id, offset, len) || offset % SECTOR_SIZE || len % SECTOR_SIZE)
    return H2_PAL_ERR_INVALID_ARG;
  flash_protect_type_t protect = bk_flash_get_protect_type();
  int rc = bk_flash_set_protect_type(FLASH_PROTECT_NONE);
  for (uint64_t off = offset; rc == 0 && off < offset + len;
       off += SECTOR_SIZE) {
    rc = bk_flash_erase_sector(s_region.offset + (uint32_t)off);
    /* Preserve the connection-service interval of the existing dump eraser. */
    if (rc == 0) rtos_delay_milliseconds(10);
  }
  (void)bk_flash_set_protect_type(protect);
  return rc == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}
static h2_pal_result_t write_bytes(void* user, uint32_t id, uint64_t offset,
                                   const void* data, size_t len) {
  (void)user;
  if ((!data && len) || !range(id, offset, len)) return H2_PAL_ERR_INVALID_ARG;
  flash_protect_type_t protect = bk_flash_get_protect_type();
  int rc = bk_flash_set_protect_type(FLASH_PROTECT_NONE);
  if (rc == 0)
    rc = bk_flash_write_bytes(s_region.offset + (uint32_t)offset, data,
                              (uint32_t)len);
  (void)bk_flash_set_protect_type(protect);
  return rc == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}
static h2_pal_result_t flush(void* user, uint32_t id) {
  (void)user;
  return range(id, 0, 0) ? H2_PAL_OK : H2_PAL_ERR_NOT_FOUND;
}
const h2_pal_disk_api_t* h2_bk_platform_disk_api(void) {
  static const h2_pal_disk_vtable_t vtable = {
      .get_partition = get_partition,
      .read = read_bytes,
      .write = write_bytes,
      .erase = erase_bytes,
      .flush = flush,
  };
  static const h2_pal_disk_api_t api = {.vtable = &vtable};
  return &api;
}
