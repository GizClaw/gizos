#include "fake_flash.h"

#include "h2/pal/h2_pal_unsupported.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static const h2_bk3633_flash_partition_config_t s_partitions[] = {
    {
        .id = 7u,
        .name = "records",
        .offset = 0x2000u,
        .size = 0x2000u,
        .erase_block_size = 0x1000u,
        .write_alignment = 4u,
        .flags = H2_PAL_DISK_PARTITION_FLAG_READABLE |
            H2_PAL_DISK_PARTITION_FLAG_WRITABLE |
            H2_PAL_DISK_PARTITION_FLAG_ERASABLE,
    },
    {
        .id = 8u,
        .name = "readonly",
        .offset = 0x5000u,
        .size = 0x1000u,
        .erase_block_size = 0x1000u,
        .write_alignment = 1u,
        .flags = H2_PAL_DISK_PARTITION_FLAG_READABLE,
    },
};

static h2_pal_result_t count_partition(
    void *user,
    const h2_pal_disk_partition_t *partition)
{
    size_t *count = (size_t *)user;
    assert(partition->name[0] != '\0');
    ++*count;
    return H2_PAL_OK;
}

static h2_pal_result_t reject_partition(
    void *user,
    const h2_pal_disk_partition_t *partition)
{
    (void)user;
    (void)partition;
    return H2_PAL_ERR_IO;
}

static void test_config_validation(fake_flash_t *fake)
{
    h2_bk3633_flash_partition_config_t invalid = s_partitions[0];
    h2_bk3633_flash_partition_config_t duplicate[2] = {
        s_partitions[0], s_partitions[1],
    };
    invalid.offset = UINT32_MAX - 0x1000u;
    assert(h2_bk3633_flash_disk_init_with_driver(
        &invalid, 1u, fake_flash_driver(fake)) == H2_PAL_ERR_INVALID_ARG);
    invalid = s_partitions[0];
    invalid.write_alignment = 3u;
    assert(h2_bk3633_flash_disk_init_with_driver(
        &invalid, 1u, fake_flash_driver(fake)) == H2_PAL_ERR_INVALID_ARG);
    duplicate[1].offset = 0x3000u;
    assert(h2_bk3633_flash_disk_init_with_driver(
        duplicate, 2u, fake_flash_driver(fake)) == H2_PAL_ERR_INVALID_ARG);
    duplicate[1] = s_partitions[1];
    duplicate[1].id = duplicate[0].id;
    assert(h2_bk3633_flash_disk_init_with_driver(
        duplicate, 2u, fake_flash_driver(fake)) == H2_PAL_ERR_INVALID_ARG);
    invalid = s_partitions[0];
    invalid.flags = UINT32_MAX;
    assert(h2_bk3633_flash_disk_init_with_driver(
        &invalid, 1u, fake_flash_driver(fake)) == H2_PAL_ERR_INVALID_ARG);
}

int main(void)
{
    static fake_flash_t fake;
    const h2_pal_disk_api_t *api;
    h2_pal_disk_partition_t partition;
    const uint8_t written[8] = {0xa5u, 0x5au, 1u, 2u, 3u, 4u, 5u, 6u};
    uint8_t readback[sizeof(written)] = {0};
    size_t count = 0u;

    fake_flash_init(&fake);
    test_config_validation(&fake);
    assert(h2_bk3633_flash_disk_init_with_driver(
        s_partitions,
        sizeof(s_partitions) / sizeof(s_partitions[0]),
        fake_flash_driver(&fake)) == H2_PAL_OK);
    assert(h2_bk3633_flash_disk_init_with_driver(
        s_partitions,
        sizeof(s_partitions) / sizeof(s_partitions[0]),
        fake_flash_driver(&fake)) == H2_PAL_ERR_INVALID_STATE);
    api = h2_bk3633_flash_disk_api();
    assert(api != h2_pal_unsupported_disk_api());
    assert(h2_pal_disk_list_partitions(api, count_partition, &count) == H2_PAL_OK);
    assert(count == 2u);
    assert(h2_pal_disk_list_partitions(api, reject_partition, NULL) ==
        H2_PAL_ERR_IO);
    assert(h2_pal_disk_get_partition(api, 7u, &partition) == H2_PAL_OK);
    assert(partition.size == 0x2000u && strcmp(partition.name, "records") == 0);
    assert(h2_pal_disk_get_partition(api, 99u, &partition) == H2_PAL_ERR_NOT_FOUND);

    assert(h2_pal_disk_read(api, 7u, 0u, NULL, 0u) == H2_PAL_OK);
    assert(h2_pal_disk_write(api, 7u, 0x2000u, NULL, 0u) == H2_PAL_OK);
    assert(h2_pal_disk_erase(api, 7u, 0x2000u, 0u) == H2_PAL_OK);
    assert(h2_pal_disk_write(api, 7u, 1u, written, sizeof(written)) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_disk_write(api, 7u, 0u, written, 3u) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_disk_erase(api, 7u, 1u, 0x1000u) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_disk_read(api, 7u, UINT64_MAX, readback, 1u) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_disk_read(api, 7u, 0x1fffu, readback, 2u) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_disk_write(api, 8u, 0u, written, sizeof(written)) == H2_PAL_ERR_INVALID_STATE);
    assert(h2_pal_disk_erase(api, 8u, 0u, 0x1000u) == H2_PAL_ERR_INVALID_STATE);
    assert(h2_pal_disk_read(api, 99u, 0u, readback, sizeof(readback)) == H2_PAL_ERR_NOT_FOUND);

    assert(h2_pal_disk_erase(api, 7u, 0u, 0x1000u) == H2_PAL_OK);
    assert(h2_pal_disk_write(api, 7u, 0u, written, sizeof(written)) == H2_PAL_OK);
    assert(h2_pal_disk_read(api, 7u, 0u, readback, sizeof(readback)) == H2_PAL_OK);
    assert(memcmp(readback, written, sizeof(written)) == 0);
    assert(h2_pal_disk_flush(api, 7u) == H2_PAL_OK);
    h2_bk3633_flash_disk_deinit();
    assert(h2_bk3633_flash_disk_api() == h2_pal_unsupported_disk_api());

    assert(h2_bk3633_flash_disk_init_with_driver(
        s_partitions,
        sizeof(s_partitions) / sizeof(s_partitions[0]),
        fake_flash_driver(&fake)) == H2_PAL_OK);
    api = h2_bk3633_flash_disk_api();
    memset(readback, 0, sizeof(readback));
    assert(h2_pal_disk_read(api, 7u, 0u, readback, sizeof(readback)) == H2_PAL_OK);
    assert(memcmp(readback, written, sizeof(written)) == 0);
    fake.next_read_result = H2_PAL_ERR_IO;
    assert(h2_pal_disk_read(api, 7u, 0u, readback, sizeof(readback)) == H2_PAL_ERR_IO);
    fake.next_write_result = H2_PAL_ERR_IO;
    assert(h2_pal_disk_write(api, 7u, 0u, written, sizeof(written)) == H2_PAL_ERR_IO);
    fake.next_erase_result = H2_PAL_ERR_IO;
    assert(h2_pal_disk_erase(api, 7u, 0u, 0x1000u) == H2_PAL_ERR_IO);
    h2_bk3633_flash_disk_deinit();
    return 0;
}
