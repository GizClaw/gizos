#include "asm/sfc_norflash_api.h"
#include "device/ioctl_cmds.h"

#include "h2_jieli_ac791n_devkit.h"
#include "h2_jieli_ac791n_devkit_partitions.h"

#include <stdint.h>
#include <string.h>

typedef struct h2_jieli_partition_definition {
  uint32_t id;
  uint32_t address;
  uint32_t size;
  uint32_t flags;
  const char *name;
} h2_jieli_partition_definition_t;

static const h2_jieli_partition_definition_t partitions[] = {
    {H2_JIELI_PARTITION_PREF, H2_JIELI_PREF_ADDRESS, H2_JIELI_PREF_SIZE,
     H2_PAL_DISK_PARTITION_FLAG_READABLE |
         H2_PAL_DISK_PARTITION_FLAG_WRITABLE |
         H2_PAL_DISK_PARTITION_FLAG_ERASABLE,
     "pref"},
    {H2_JIELI_PARTITION_COREDUMP, H2_JIELI_COREDUMP_ADDRESS,
     H2_JIELI_COREDUMP_SIZE,
     H2_PAL_DISK_PARTITION_FLAG_READABLE |
         H2_PAL_DISK_PARTITION_FLAG_WRITABLE |
         H2_PAL_DISK_PARTITION_FLAG_ERASABLE,
     "coredump"},
    {H2_JIELI_PARTITION_VENDOR, H2_JIELI_VENDOR_ADDRESS,
     H2_JIELI_VENDOR_SIZE, H2_PAL_DISK_PARTITION_FLAG_READABLE, "vendor"},
    {H2_JIELI_PARTITION_BOOT, H2_JIELI_BOOT_ADDRESS, H2_JIELI_BOOT_SIZE,
     H2_PAL_DISK_PARTITION_FLAG_READABLE |
         H2_PAL_DISK_PARTITION_FLAG_BOOTABLE,
     "boot"},
};

static const h2_jieli_partition_definition_t *find_partition(uint32_t id) {
  for (size_t i = 0u; i < sizeof(partitions) / sizeof(partitions[0]); ++i) {
    if (partitions[i].id == id) return &partitions[i];
  }
  return NULL;
}

static h2_pal_result_t describe_partition(
    const h2_jieli_partition_definition_t *definition,
    h2_pal_disk_partition_t *out_partition) {
  if (definition == NULL || out_partition == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_partition, 0, sizeof(*out_partition));
  out_partition->id = definition->id;
  out_partition->flags = definition->flags;
  out_partition->size = definition->size;
  out_partition->erase_block_size = H2_JIELI_FLASH_SECTOR_SIZE;
  out_partition->write_alignment = 1u;
  (void)strncpy(
      out_partition->name, definition->name,
      H2_PAL_DISK_PARTITION_NAME_MAX - 1u);
  return H2_PAL_OK;
}

static h2_pal_result_t list_partitions(
    void *user, h2_pal_disk_partition_cb_t callback, void *callback_user) {
  (void)user;
  if (callback == NULL) return H2_PAL_ERR_INVALID_ARG;
  for (size_t i = 0u; i < sizeof(partitions) / sizeof(partitions[0]); ++i) {
    h2_pal_disk_partition_t partition;
    h2_pal_result_t result = describe_partition(&partitions[i], &partition);
    if (result == H2_PAL_OK) result = callback(callback_user, &partition);
    if (result != H2_PAL_OK) return result;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t get_partition(
    void *user, uint32_t partition_id,
    h2_pal_disk_partition_t *out_partition) {
  (void)user;
  const h2_jieli_partition_definition_t *definition =
      find_partition(partition_id);
  if (definition == NULL) return H2_PAL_ERR_NOT_FOUND;
  return describe_partition(definition, out_partition);
}

static int range_valid(
    const h2_jieli_partition_definition_t *partition, uint64_t offset,
    uint64_t length) {
  return partition != NULL && offset <= partition->size &&
         length <= (uint64_t)partition->size - offset;
}

static h2_pal_result_t read_partition(
    void *user, uint32_t partition_id, uint64_t offset, void *data,
    size_t length) {
  (void)user;
  const h2_jieli_partition_definition_t *partition =
      find_partition(partition_id);
  if (!range_valid(partition, offset, length) ||
      (data == NULL && length != 0u)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (length == 0u) return H2_PAL_OK;
  if (length > UINT32_MAX) return H2_PAL_ERR_INVALID_ARG;
  int result = norflash_read(
      NULL, data, (uint32_t)length, partition->address + (uint32_t)offset);
  return result == (int)length ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_pal_result_t erase_partition(
    void *user, uint32_t partition_id, uint64_t offset, uint64_t length) {
  (void)user;
  const h2_jieli_partition_definition_t *partition =
      find_partition(partition_id);
  if (!range_valid(partition, offset, length) ||
      (partition->flags & H2_PAL_DISK_PARTITION_FLAG_ERASABLE) == 0u ||
      (offset % H2_JIELI_FLASH_SECTOR_SIZE) != 0u ||
      (length % H2_JIELI_FLASH_SECTOR_SIZE) != 0u || length > UINT32_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (length == 0u) return H2_PAL_OK;
  (void)norflash_ioctl(NULL, IOCTL_SET_WRITE_PROTECT, 0u);
  uint32_t address = partition->address + (uint32_t)offset;
  uint32_t end = address + (uint32_t)length;
  while (address < end) {
    if (norflash_ioctl(NULL, IOCTL_ERASE_SECTOR, address) != 0) {
      return H2_PAL_ERR_IO;
    }
    address += H2_JIELI_FLASH_SECTOR_SIZE;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t write_partition(
    void *user, uint32_t partition_id, uint64_t offset, const void *data,
    size_t length) {
  (void)user;
  const h2_jieli_partition_definition_t *partition =
      find_partition(partition_id);
  if (!range_valid(partition, offset, length) ||
      (partition->flags & H2_PAL_DISK_PARTITION_FLAG_WRITABLE) == 0u ||
      (data == NULL && length != 0u) || length > UINT32_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (length == 0u) return H2_PAL_OK;
  (void)norflash_ioctl(NULL, IOCTL_SET_WRITE_PROTECT, 0u);
  int result = norflash_write(
      NULL, (void *)data, (uint32_t)length,
      partition->address + (uint32_t)offset);
  return result == (int)length ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_pal_result_t flush_partition(void *user, uint32_t partition_id) {
  (void)user;
  return find_partition(partition_id) == NULL ? H2_PAL_ERR_NOT_FOUND
                                               : H2_PAL_OK;
}

const h2_pal_disk_api_t *h2_jieli_ac791n_devkit_disk_api(void) {
  static const h2_pal_disk_vtable_t vtable = {
      .list_partitions = list_partitions,
      .get_partition = get_partition,
      .read = read_partition,
      .erase = erase_partition,
      .write = write_partition,
      .flush = flush_partition,
  };
  static const h2_pal_disk_api_t api = {.user = NULL, .vtable = &vtable};
  return &api;
}
