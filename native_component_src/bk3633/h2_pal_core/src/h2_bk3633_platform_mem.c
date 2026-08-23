#include "h2_bk3633_platform_core.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef union h2_bk3633_mem_block h2_bk3633_mem_block_t;

union h2_bk3633_mem_block {
    max_align_t alignment;
    struct {
        size_t size;
        h2_bk3633_mem_block_t *next;
        bool free;
    } info;
};

static h2_bk3633_mem_block_t *s_first_block;
static size_t s_capacity;
static size_t s_failed_allocations;
static size_t s_last_failed_request;

static bool mem_align_size(size_t length, size_t *out_length)
{
    const size_t alignment = _Alignof(max_align_t);

    if (out_length == NULL || length == 0u ||
        length > SIZE_MAX - (alignment - 1u)) {
        return false;
    }
    *out_length = (length + (alignment - 1u)) & ~(alignment - 1u);
    return true;
}

static void mem_record_failure(size_t length)
{
    if (s_failed_allocations != SIZE_MAX) {
        ++s_failed_allocations;
    }
    s_last_failed_request = length;
}

static void mem_split_block(
    h2_bk3633_mem_block_t *block,
    size_t length)
{
    if (block->info.size < length + sizeof(*block) +
                            _Alignof(max_align_t)) {
        return;
    }

    h2_bk3633_mem_block_t *remainder =
        (h2_bk3633_mem_block_t *)((uint8_t *)(block + 1) + length);
    remainder->info.size =
        block->info.size - length - sizeof(*remainder);
    remainder->info.next = block->info.next;
    remainder->info.free = true;
    block->info.size = length;
    block->info.next = remainder;
}

static void mem_coalesce(void)
{
    h2_bk3633_mem_block_t *block = s_first_block;

    while (block != NULL && block->info.next != NULL) {
        if (block->info.free && block->info.next->info.free) {
            block->info.size +=
                sizeof(*block) + block->info.next->info.size;
            block->info.next = block->info.next->info.next;
        } else {
            block = block->info.next;
        }
    }
}

static h2_bk3633_mem_block_t *mem_find_block(void *ptr)
{
    h2_bk3633_mem_block_t *block = s_first_block;

    while (block != NULL) {
        if ((void *)(block + 1) == ptr) {
            return block;
        }
        block = block->info.next;
    }
    return NULL;
}

static void *mem_alloc(void *user, size_t length)
{
    size_t aligned_length;
    h2_bk3633_mem_block_t *block;

    (void)user;
    if (!mem_align_size(length, &aligned_length)) {
        mem_record_failure(length);
        return NULL;
    }

    for (block = s_first_block; block != NULL; block = block->info.next) {
        if (block->info.free && block->info.size >= aligned_length) {
            mem_split_block(block, aligned_length);
            block->info.free = false;
            return block + 1;
        }
    }
    mem_record_failure(length);
    return NULL;
}

static void mem_free(void *user, void *ptr)
{
    h2_bk3633_mem_block_t *block;

    (void)user;
    if (ptr == NULL) {
        return;
    }
    block = mem_find_block(ptr);
    if (block == NULL || block->info.free) {
        return;
    }
    block->info.free = true;
    mem_coalesce();
}

static void *mem_realloc(void *user, void *ptr, size_t length)
{
    size_t aligned_length;
    h2_bk3633_mem_block_t *block;

    if (ptr == NULL) {
        return mem_alloc(user, length);
    }
    if (length == 0u) {
        mem_free(user, ptr);
        return NULL;
    }
    if (!mem_align_size(length, &aligned_length)) {
        mem_record_failure(length);
        return NULL;
    }

    block = mem_find_block(ptr);
    if (block == NULL || block->info.free) {
        return NULL;
    }
    if (block->info.size >= aligned_length) {
        mem_split_block(block, aligned_length);
        return ptr;
    }
    if (block->info.next != NULL && block->info.next->info.free &&
        block->info.size + sizeof(*block) + block->info.next->info.size >=
            aligned_length) {
        block->info.size +=
            sizeof(*block) + block->info.next->info.size;
        block->info.next = block->info.next->info.next;
        mem_split_block(block, aligned_length);
        return ptr;
    }

    void *replacement = mem_alloc(user, length);
    if (replacement == NULL) {
        return NULL;
    }
    memcpy(replacement, ptr, block->info.size);
    mem_free(user, ptr);
    return replacement;
}

h2_pal_result_t h2_bk3633_platform_mem_init(
    const h2_bk3633_platform_mem_config_t *config)
{
    const size_t alignment = _Alignof(max_align_t);
    size_t capacity;

    if (config == NULL || config->storage == NULL ||
        (uintptr_t)config->storage % alignment != 0u ||
        config->storage_size >
            UINTPTR_MAX - (uintptr_t)config->storage) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    capacity = config->storage_size & ~(alignment - 1u);
    if (capacity < sizeof(*s_first_block) + alignment) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (s_first_block != NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }

    s_capacity = capacity;
    s_failed_allocations = 0u;
    s_last_failed_request = 0u;
    s_first_block = (h2_bk3633_mem_block_t *)config->storage;
    s_first_block->info.size = capacity - sizeof(*s_first_block);
    s_first_block->info.next = NULL;
    s_first_block->info.free = true;
    return H2_PAL_OK;
}

const h2_pal_mem_api_t *h2_bk3633_platform_mem_api(void)
{
    static const h2_pal_mem_vtable_t vtable = {
        .alloc = mem_alloc,
        .realloc = mem_realloc,
        .free = mem_free,
    };
    static const h2_pal_mem_api_t api = {
        .user = NULL,
        .vtable = &vtable,
    };
    return &api;
}

h2_pal_result_t h2_bk3633_platform_mem_get_stats(
    h2_bk3633_platform_mem_stats_t *out_stats)
{
    h2_bk3633_mem_block_t *block;

    if (out_stats == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_stats = (h2_bk3633_platform_mem_stats_t){0};
    if (s_first_block == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }

    out_stats->capacity = s_capacity;
    out_stats->failed_allocations = s_failed_allocations;
    out_stats->last_failed_request = s_last_failed_request;
    for (block = s_first_block; block != NULL; block = block->info.next) {
        if (block->info.free) {
            out_stats->free_bytes += block->info.size;
            if (block->info.size > out_stats->largest_free_block) {
                out_stats->largest_free_block = block->info.size;
            }
        } else {
            out_stats->used_bytes += block->info.size;
        }
    }
    return H2_PAL_OK;
}
