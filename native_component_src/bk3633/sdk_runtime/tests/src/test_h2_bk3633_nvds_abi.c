#include "flash.h"
#include "h2_bk3633_sdk_runtime.h"
#include "nvds.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TEST_NVDS_BASE 0x1000u
#define TEST_NVDS_SIZE 0x0400u
#define TEST_NVDS_MAGIC_SIZE 4u
#define TEST_NVDS_HEADER_SIZE 3u
#define TEST_NVDS_STATUS_OK 0x06u

h2_bk3633_nvds_abi_test_flash_env_t flash_env = {
    .nvds_def_addr_abs = TEST_NVDS_BASE,
};

static uint8_t s_flash[TEST_NVDS_SIZE];
static bool s_fail_read;
static size_t s_alloc_count;

static void *test_mem_alloc(void *user, size_t len) {
    (void)user;
    ++s_alloc_count;
    return malloc(len);
}

static void *test_mem_realloc(void *user, void *ptr, size_t len) {
    (void)user;
    return realloc(ptr, len);
}

static void test_mem_free(void *user, void *ptr) {
    (void)user;
    free(ptr);
}

const h2_pal_mem_api_t *h2_bk3633_sdk_runtime_nvds_mem_api(void) {
    static const h2_pal_mem_vtable_t vtable = {
        .alloc = test_mem_alloc,
        .realloc = test_mem_realloc,
        .free = test_mem_free,
    };
    static const h2_pal_mem_api_t api = {
        .user = NULL,
        .vtable = &vtable,
    };
    return &api;
}

bool h2_bk3633_sdk_runtime_nvds_oversize_is_missing(
    uint8_t tag, size_t stored_size, size_t requested_capacity)
{
    (void)stored_size;
    (void)requested_capacity;
    return tag == 0xb0u || tag == 0xb1u;
}

static bool flash_range_valid(uint32_t address, uint32_t length) {
    return address >= TEST_NVDS_BASE &&
           address - TEST_NVDS_BASE <= TEST_NVDS_SIZE &&
           length <= TEST_NVDS_SIZE - (address - TEST_NVDS_BASE);
}

uint8_t flash_read(uint8_t flash_id, uint32_t address, uint32_t length,
                   uint8_t *data, void *callback) {
    (void)flash_id;
    (void)callback;
    if (s_fail_read || data == NULL || !flash_range_valid(address, length)) {
        return 1u;
    }
    memcpy(data, s_flash + (address - TEST_NVDS_BASE), length);
    return 0u;
}

uint8_t flash_write(uint8_t flash_id, uint32_t address, uint32_t length,
                    uint8_t *data, void *callback) {
    (void)flash_id;
    (void)callback;
    if (data == NULL || !flash_range_valid(address, length)) {
        return 1u;
    }
    memcpy(s_flash + (address - TEST_NVDS_BASE), data, length);
    return 0u;
}

uint8_t flash_erase(uint8_t flash_id, uint32_t address, uint32_t length,
                    void *callback) {
    (void)flash_id;
    (void)callback;
    if (!flash_range_valid(address, length)) {
        return 1u;
    }
    memset(s_flash + (address - TEST_NVDS_BASE), 0xff, length);
    return 0u;
}

static void test_reset(void) {
    memset(s_flash, 0xff, sizeof(s_flash));
    s_fail_read = false;
    s_alloc_count = 0u;
    assert(nvds_init() == NVDS_OK);
}

static void write_header(uint32_t offset, uint8_t tag, uint8_t status,
                         uint8_t length) {
    assert(offset <= TEST_NVDS_SIZE - TEST_NVDS_HEADER_SIZE);
    s_flash[offset] = tag;
    s_flash[offset + 1u] = status;
    s_flash[offset + 2u] = length;
}

static void test_empty_store_reports_missing(void) {
    uint8_t data[8];
    nvds_tag_len_t len = sizeof(data);

    test_reset();
    assert(memcmp(s_flash, "NVDS", TEST_NVDS_MAGIC_SIZE) == 0);
    assert(nvds_get(0xb0u, &len, data) == NVDS_TAG_NOT_DEFINED);
    assert(len == 0u);
}

static void test_corrupt_tail_is_compacted_before_identity_write(void) {
    static const uint8_t legacy[] = {0x31u, 0x32u, 0x33u};
    static const uint8_t identity[] = {0xa1u, 0xb2u, 0xc3u, 0xd4u};
    uint8_t data[8];
    nvds_tag_len_t len;
    uint32_t offset;

    test_reset();
    assert(nvds_put(0xa8u, sizeof(legacy), (uint8_t *)legacy) == NVDS_OK);

    offset = TEST_NVDS_MAGIC_SIZE + TEST_NVDS_HEADER_SIZE + sizeof(legacy);
    write_header(offset, 0x20u, 0x01u, 253u);
    offset += TEST_NVDS_HEADER_SIZE + 253u;
    write_header(offset, 0x21u, 0x01u, 253u);
    offset += TEST_NVDS_HEADER_SIZE + 253u;
    write_header(offset, 0x22u, 0x01u, 253u);
    offset += TEST_NVDS_HEADER_SIZE + 253u;
    write_header(offset, 0x23u, 0x01u, 253u);

    len = sizeof(data);
    assert(nvds_get(0xb0u, &len, data) == NVDS_TAG_NOT_DEFINED);
    assert(len == 0u);
    assert(nvds_put(0xb0u, sizeof(identity), (uint8_t *)identity) == NVDS_OK);

    len = sizeof(data);
    assert(nvds_get(0xa8u, &len, data) == NVDS_OK);
    assert(len == sizeof(legacy));
    assert(memcmp(data, legacy, sizeof(legacy)) == 0);
    len = sizeof(data);
    assert(nvds_get(0xb0u, &len, data) == NVDS_OK);
    assert(len == sizeof(identity));
    assert(memcmp(data, identity, sizeof(identity)) == 0);
}

static void test_oversized_legacy_identity_is_replaced(void) {
    uint8_t legacy_identity[120];
    uint8_t identity[110];
    uint8_t data[110];
    nvds_tag_len_t len;

    test_reset();
    memset(legacy_identity, 0xa5, sizeof(legacy_identity));
    memset(identity, 0x5a, sizeof(identity));
    assert(nvds_put(0xb0u, sizeof(legacy_identity), legacy_identity) ==
           NVDS_OK);

    len = sizeof(data);
    assert(nvds_get(0xb0u, &len, data) == NVDS_TAG_NOT_DEFINED);
    assert(len == 0u);
    assert(nvds_put(0xb0u, sizeof(identity), identity) == NVDS_OK);

    len = sizeof(data);
    assert(nvds_get(0xb0u, &len, data) == NVDS_OK);
    assert(len == sizeof(identity));
    assert(memcmp(data, identity, sizeof(identity)) == 0);
}

static void test_flash_read_failure_remains_failure(void) {
    uint8_t data[8];
    nvds_tag_len_t len = sizeof(data);

    test_reset();
    s_fail_read = true;
    assert(nvds_get(0xb0u, &len, data) == NVDS_FAIL);
    assert(len == 0u);
}

static void test_put_invalidates_all_older_duplicate_records(void) {
    static const uint8_t stale = 0x31u;
    static const uint8_t current = 0x42u;
    uint8_t data = 0u;
    nvds_tag_len_t len = sizeof(data);
    const uint32_t first = TEST_NVDS_MAGIC_SIZE;
    const uint32_t second = first + TEST_NVDS_HEADER_SIZE + sizeof(stale);

    test_reset();
    write_header(first, 0xb0u, TEST_NVDS_STATUS_OK, sizeof(stale));
    s_flash[first + TEST_NVDS_HEADER_SIZE] = stale;
    write_header(second, 0xb0u, TEST_NVDS_STATUS_OK, sizeof(current));
    s_flash[second + TEST_NVDS_HEADER_SIZE] = current;

    assert(nvds_put(0xb0u, sizeof(current), (uint8_t *)&current) == NVDS_OK);
    assert((s_flash[first + 1u] & 0x04u) != 0u);
    assert(s_flash[first + TEST_NVDS_HEADER_SIZE] == current);
    assert(s_flash[second] == 0xffu);
    assert(nvds_get(0xb0u, &len, &data) == NVDS_OK);
    assert(len == sizeof(data));
    assert(data == current);
}

static void test_maintenance_compacts_only_when_tail_is_small(void) {
    uint8_t value[240];
    uint8_t data[sizeof(value)];
    nvds_tag_len_t len;

    test_reset();
    memset(value, 0x5au, sizeof(value));
    assert(nvds_put(0xb0u, sizeof(value), value) == NVDS_OK);
    assert(h2_bk3633_sdk_runtime_nvds_maintain() == H2_PAL_OK);
    assert(s_alloc_count == 0u);

    for (uint8_t tag = 0xb1u; tag < 0xb3u; ++tag) {
        value[0] = tag;
        assert(nvds_put(tag, sizeof(value), value) == NVDS_OK);
    }
    value[0] = 0xa5u;
    assert(nvds_put(0xb0u, sizeof(value), value) == NVDS_OK);
    assert(h2_bk3633_sdk_runtime_nvds_maintain() == H2_PAL_OK);
    assert(s_alloc_count == 1u);
    for (uint8_t tag = 0xb0u; tag < 0xb3u; ++tag) {
        len = sizeof(data);
        assert(nvds_get(tag, &len, data) == NVDS_OK);
        assert(len == sizeof(value));
        assert(data[0] == (tag == 0xb0u ? 0xa5u : tag));
    }
}

int main(void) {
    test_empty_store_reports_missing();
    test_corrupt_tail_is_compacted_before_identity_write();
    test_oversized_legacy_identity_is_replaced();
    test_flash_read_failure_remains_failure();
    test_put_invalidates_all_older_duplicate_records();
    test_maintenance_compacts_only_when_tail_is_small();
    return 0;
}
