#include "h2_bk3633_platform_core.h"

#include <stdint.h>
#include <string.h>

int main(void)
{
    static union {
        max_align_t alignment;
        uint8_t bytes[16384u];
    } arena;
    const h2_bk3633_platform_mem_config_t config = {
        .storage = arena.bytes,
        .storage_size = sizeof(arena.bytes),
    };
    if (h2_bk3633_platform_mem_init(&config) != H2_PAL_OK) {
        return 1;
    }
    const h2_pal_mem_api_t *mem = h2_bk3633_platform_mem_api();
    uint8_t *first = h2_pal_mem_alloc(mem, 32u);
    uint8_t *second = h2_pal_mem_alloc(mem, 64u);

    if (first == NULL || second == NULL) {
        return 2;
    }
    memset(first, 0x5au, 32u);
    h2_pal_mem_free(mem, second);
    first = h2_pal_mem_realloc(mem, first, 96u);
    if (first == NULL) {
        return 3;
    }
    for (size_t i = 0u; i < 32u; ++i) {
        if (first[i] != 0x5au) {
            return 4;
        }
    }

    h2_pal_mem_free(mem, first);
    first = h2_pal_mem_alloc(mem, 12000u);
    if (first == NULL) {
        return 5;
    }
    h2_pal_mem_free(mem, first);
    return 0;
}
