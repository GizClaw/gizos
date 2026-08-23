#include "h2_bk3633_mem_test_support.h"

#include "h2_bk3633_platform_core.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#define H2_BK3633_MEM_TEST_ARENA_SIZE (64u * 1024u)

void h2_bk3633_mem_test_support_init(void)
{
    static union {
        max_align_t alignment;
        uint8_t bytes[H2_BK3633_MEM_TEST_ARENA_SIZE];
    } arena;
    const h2_bk3633_platform_mem_config_t config = {
        .storage = arena.bytes,
        .storage_size = sizeof(arena.bytes),
    };

    assert(h2_bk3633_platform_mem_init(&config) == H2_PAL_OK);
}
