#include "h2_bk3633_platform_core.h"
#include "h2_bk3633_pal_storage_internal.h"

#include "h2/pal/h2_pal_unsupported.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(BK3633)
#include "flash.h"
#endif

typedef struct h2_bk3633_flash_disk_state {
    h2_pal_disk_api_t api;
    const h2_bk3633_flash_partition_config_t *partitions;
    size_t partition_count;
    h2_bk3633_flash_driver_t driver;
    bool ready;
} h2_bk3633_flash_disk_state_t;

static h2_bk3633_flash_disk_state_t s_flash_disk;

static size_t disk_bounded_strlen(const char *text, size_t limit)
{
    size_t len = 0u;
    while (len < limit && text[len] != '\0') {
        ++len;
    }
    return len;
}

static bool disk_power_of_two(uint32_t value)
{
    return value != 0u && (value & (value - 1u)) == 0u;
}

static h2_pal_result_t disk_partitions_validate(
    const h2_bk3633_flash_partition_config_t *partitions,
    size_t partition_count)
{
    const uint32_t valid_flags = H2_PAL_DISK_PARTITION_FLAG_READABLE |
        H2_PAL_DISK_PARTITION_FLAG_WRITABLE |
        H2_PAL_DISK_PARTITION_FLAG_ERASABLE |
        H2_PAL_DISK_PARTITION_FLAG_BOOTABLE;
    size_t index;
    size_t other;
    if (partitions == NULL || partition_count == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    for (index = 0u; index < partition_count; ++index) {
        const h2_bk3633_flash_partition_config_t *partition =
            &partitions[index];
        uint64_t end = (uint64_t)partition->offset + partition->size;
        if (partition->name == NULL || partition->name[0] == '\0' ||
            disk_bounded_strlen(
                partition->name, H2_PAL_DISK_PARTITION_NAME_MAX) >=
                H2_PAL_DISK_PARTITION_NAME_MAX ||
            partition->size == 0u ||
            end > (uint64_t)UINT32_MAX + 1u ||
            !disk_power_of_two(partition->erase_block_size) ||
            !disk_power_of_two(partition->write_alignment) ||
            (partition->offset % partition->erase_block_size) != 0u ||
            (partition->size % partition->erase_block_size) != 0u ||
            (partition->flags & ~valid_flags) != 0u) {
            return H2_PAL_ERR_INVALID_ARG;
        }
        for (other = 0u; other < index; ++other) {
            const h2_bk3633_flash_partition_config_t *prior =
                &partitions[other];
            uint64_t prior_end = (uint64_t)prior->offset + prior->size;
            if (prior->id == partition->id ||
                strcmp(prior->name, partition->name) == 0 ||
                ((uint64_t)partition->offset < prior_end &&
                 (uint64_t)prior->offset < end)) {
                return H2_PAL_ERR_INVALID_ARG;
            }
        }
    }
    return H2_PAL_OK;
}

static const h2_bk3633_flash_partition_config_t *disk_find_partition(
    const h2_bk3633_flash_disk_state_t *disk,
    uint32_t partition_id)
{
    size_t index;
    for (index = 0u; index < disk->partition_count; ++index) {
        if (disk->partitions[index].id == partition_id) {
            return &disk->partitions[index];
        }
    }
    return NULL;
}

static h2_pal_result_t disk_validate_range(
    const h2_bk3633_flash_partition_config_t *partition,
    uint64_t offset,
    uint64_t len,
    uint32_t *out_address)
{
    uint64_t end;
    uint64_t address;
    if (offset > partition->size || len > (uint64_t)partition->size - offset) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    end = offset + len;
    address = (uint64_t)partition->offset + offset;
    if (end > partition->size || address > UINT32_MAX ||
        len > UINT32_MAX || address + len > (uint64_t)UINT32_MAX + 1u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_address = (uint32_t)address;
    return H2_PAL_OK;
}

static h2_pal_result_t disk_list_partitions(
    void *user,
    h2_pal_disk_partition_cb_t callback,
    void *callback_user)
{
    h2_bk3633_flash_disk_state_t *disk =
        (h2_bk3633_flash_disk_state_t *)user;
    size_t index;
    if (disk == NULL || !disk->ready || callback == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    for (index = 0u; index < disk->partition_count; ++index) {
        const h2_bk3633_flash_partition_config_t *config =
            &disk->partitions[index];
        h2_pal_disk_partition_t partition = {
            .id = config->id,
            .flags = config->flags,
            .size = config->size,
            .erase_block_size = config->erase_block_size,
            .write_alignment = config->write_alignment,
        };
        h2_pal_result_t rc;
        memcpy(partition.name, config->name, strlen(config->name) + 1u);
        rc = callback(callback_user, &partition);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    return H2_PAL_OK;
}

static h2_pal_result_t disk_get_partition(
    void *user,
    uint32_t partition_id,
    h2_pal_disk_partition_t *out_partition)
{
    h2_bk3633_flash_disk_state_t *disk =
        (h2_bk3633_flash_disk_state_t *)user;
    const h2_bk3633_flash_partition_config_t *config;
    if (disk == NULL || !disk->ready || out_partition == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_partition, 0, sizeof(*out_partition));
    config = disk_find_partition(disk, partition_id);
    if (config == NULL) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    out_partition->id = config->id;
    out_partition->flags = config->flags;
    out_partition->size = config->size;
    out_partition->erase_block_size = config->erase_block_size;
    out_partition->write_alignment = config->write_alignment;
    memcpy(out_partition->name, config->name, strlen(config->name) + 1u);
    return H2_PAL_OK;
}

static h2_pal_result_t disk_read(
    void *user,
    uint32_t partition_id,
    uint64_t offset,
    void *data,
    size_t len)
{
    h2_bk3633_flash_disk_state_t *disk =
        (h2_bk3633_flash_disk_state_t *)user;
    const h2_bk3633_flash_partition_config_t *partition;
    uint32_t address;
    h2_pal_result_t rc;
    if (disk == NULL || !disk->ready || (data == NULL && len != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    partition = disk_find_partition(disk, partition_id);
    if (partition == NULL) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    if ((partition->flags & H2_PAL_DISK_PARTITION_FLAG_READABLE) == 0u) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    rc = disk_validate_range(partition, offset, len, &address);
    if (rc != H2_PAL_OK || len == 0u) {
        return rc;
    }
    return disk->driver.read(disk->driver.user, address, data, len);
}

static h2_pal_result_t disk_erase(
    void *user,
    uint32_t partition_id,
    uint64_t offset,
    uint64_t len)
{
    h2_bk3633_flash_disk_state_t *disk =
        (h2_bk3633_flash_disk_state_t *)user;
    const h2_bk3633_flash_partition_config_t *partition;
    uint32_t address;
    h2_pal_result_t rc;
    if (disk == NULL || !disk->ready) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    partition = disk_find_partition(disk, partition_id);
    if (partition == NULL) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    if ((partition->flags & H2_PAL_DISK_PARTITION_FLAG_ERASABLE) == 0u) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if ((offset % partition->erase_block_size) != 0u ||
        (len % partition->erase_block_size) != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = disk_validate_range(partition, offset, len, &address);
    if (rc != H2_PAL_OK || len == 0u) {
        return rc;
    }
    return disk->driver.erase(disk->driver.user, address, (size_t)len);
}

static h2_pal_result_t disk_write(
    void *user,
    uint32_t partition_id,
    uint64_t offset,
    const void *data,
    size_t len)
{
    h2_bk3633_flash_disk_state_t *disk =
        (h2_bk3633_flash_disk_state_t *)user;
    const h2_bk3633_flash_partition_config_t *partition;
    uint32_t address;
    h2_pal_result_t rc;
    if (disk == NULL || !disk->ready || (data == NULL && len != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    partition = disk_find_partition(disk, partition_id);
    if (partition == NULL) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    if ((partition->flags & H2_PAL_DISK_PARTITION_FLAG_WRITABLE) == 0u) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if ((offset % partition->write_alignment) != 0u ||
        (len % partition->write_alignment) != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = disk_validate_range(partition, offset, len, &address);
    if (rc != H2_PAL_OK || len == 0u) {
        return rc;
    }
    return disk->driver.write(disk->driver.user, address, data, len);
}

static h2_pal_result_t disk_flush(void *user, uint32_t partition_id)
{
    h2_bk3633_flash_disk_state_t *disk =
        (h2_bk3633_flash_disk_state_t *)user;
    if (disk == NULL || !disk->ready) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return disk_find_partition(disk, partition_id) != NULL ?
        H2_PAL_OK : H2_PAL_ERR_NOT_FOUND;
}

static const h2_pal_disk_vtable_t s_disk_vtable = {
    .list_partitions = disk_list_partitions,
    .get_partition = disk_get_partition,
    .read = disk_read,
    .erase = disk_erase,
    .write = disk_write,
    .flush = disk_flush,
};

h2_pal_result_t h2_bk3633_flash_disk_init_with_driver(
    const h2_bk3633_flash_partition_config_t *partitions,
    size_t partition_count,
    const h2_bk3633_flash_driver_t *driver)
{
    h2_pal_result_t rc;
    if (s_flash_disk.ready) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (driver == NULL || driver->read == NULL || driver->erase == NULL ||
        driver->write == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = disk_partitions_validate(partitions, partition_count);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    memset(&s_flash_disk, 0, sizeof(s_flash_disk));
    s_flash_disk.partitions = partitions;
    s_flash_disk.partition_count = partition_count;
    s_flash_disk.driver = *driver;
    s_flash_disk.api.user = &s_flash_disk;
    s_flash_disk.api.vtable = &s_disk_vtable;
    s_flash_disk.ready = true;
    return H2_PAL_OK;
}

#if defined(BK3633)
static h2_pal_result_t sdk_flash_read(
    void *user,
    uint32_t address,
    void *data,
    size_t len)
{
    (void)user;
    return flash_read(
        0u,
        address,
        (uint32_t)len,
        (uint8_t *)data,
        NULL) == 0u ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_pal_result_t sdk_flash_erase(
    void *user,
    uint32_t address,
    size_t len)
{
    (void)user;
    return flash_erase(
        0u,
        address,
        (uint32_t)len,
        NULL) == 0u ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_pal_result_t sdk_flash_write(
    void *user,
    uint32_t address,
    const void *data,
    size_t len)
{
    (void)user;
    return flash_write(
        0u,
        address,
        (uint32_t)len,
        (uint8_t *)data,
        NULL) == 0u ? H2_PAL_OK : H2_PAL_ERR_IO;
}
#endif

h2_pal_result_t h2_bk3633_flash_disk_init(
    const h2_bk3633_flash_partition_config_t *partitions,
    size_t partition_count)
{
#if defined(BK3633)
    static const h2_bk3633_flash_driver_t driver = {
        .read = sdk_flash_read,
        .erase = sdk_flash_erase,
        .write = sdk_flash_write,
        .user = NULL,
    };
    return h2_bk3633_flash_disk_init_with_driver(
        partitions, partition_count, &driver);
#else
    (void)partitions;
    (void)partition_count;
    return H2_PAL_ERR_UNAVAILABLE;
#endif
}

void h2_bk3633_flash_disk_deinit(void)
{
    memset(&s_flash_disk, 0, sizeof(s_flash_disk));
}

const h2_pal_disk_api_t *h2_bk3633_flash_disk_api(void)
{
    return s_flash_disk.ready ?
        &s_flash_disk.api : h2_pal_unsupported_disk_api();
}
