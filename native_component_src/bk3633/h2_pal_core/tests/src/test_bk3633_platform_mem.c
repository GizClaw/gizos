#include "h2_bk3633_platform_core.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define H2_BK3633_MEM_TEST_STORAGE_SIZE 512u

int main(void)
{
    static union {
        max_align_t alignment;
        uint8_t bytes[H2_BK3633_MEM_TEST_STORAGE_SIZE];
    } storage;
    h2_bk3633_platform_mem_stats_t stats = {
        .capacity = 1u,
    };
    const h2_pal_mem_api_t *mem = h2_bk3633_platform_mem_api();
    h2_bk3633_platform_mem_config_t config = {
        .storage = storage.bytes,
        .storage_size = sizeof(storage.bytes),
    };

    assert(h2_bk3633_platform_mem_get_stats(&stats) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(stats.capacity == 0u);
    assert(h2_pal_mem_alloc(mem, 8u) == NULL);
    assert(h2_bk3633_platform_mem_init(NULL) == H2_PAL_ERR_INVALID_ARG);
    config.storage = &storage.bytes[1];
    assert(h2_bk3633_platform_mem_init(&config) == H2_PAL_ERR_INVALID_ARG);
    config.storage = storage.bytes;
    config.storage_size = 1u;
    assert(h2_bk3633_platform_mem_init(&config) == H2_PAL_ERR_INVALID_ARG);
    config.storage_size = sizeof(storage.bytes);
    assert(h2_bk3633_platform_mem_init(&config) == H2_PAL_OK);
    assert(h2_bk3633_platform_mem_init(&config) ==
           H2_PAL_ERR_INVALID_STATE);

    uint8_t *first = h2_pal_mem_alloc(mem, 32u);
    uint8_t *second = h2_pal_mem_alloc(mem, 64u);
    assert(first != NULL && second != NULL);
    memset(first, 0x5au, 32u);
    h2_pal_mem_free(mem, second);
    first = h2_pal_mem_realloc(mem, first, 96u);
    assert(first != NULL);
    for (size_t index = 0u; index < 32u; ++index) {
        assert(first[index] == 0x5au);
    }

    assert(h2_pal_mem_alloc(mem, sizeof(storage.bytes)) == NULL);
    assert(h2_bk3633_platform_mem_get_stats(&stats) == H2_PAL_OK);
    assert(stats.capacity == sizeof(storage.bytes));
    assert(stats.used_bytes >= 96u);
    assert(stats.free_bytes < stats.capacity);
    assert(stats.largest_free_block <= stats.free_bytes);
    assert(stats.failed_allocations == 1u);
    assert(stats.last_failed_request == sizeof(storage.bytes));

    h2_pal_mem_free(mem, first);
    assert(h2_bk3633_platform_mem_get_stats(&stats) == H2_PAL_OK);
    assert(stats.used_bytes == 0u);
    assert(stats.free_bytes < stats.capacity);
    assert(stats.largest_free_block == stats.free_bytes);
    return 0;
}
