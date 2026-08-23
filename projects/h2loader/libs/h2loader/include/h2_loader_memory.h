#ifndef H2_LOADER_MEMORY_H
#define H2_LOADER_MEMORY_H

#include "h2_pal.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_loader_memory_region_stats {
    size_t total_bytes;
    size_t free_bytes;
    size_t minimum_free_bytes;
    size_t largest_free_block_bytes;
} h2_loader_memory_region_stats_t;

typedef struct h2_loader_memory_stats {
    h2_loader_memory_region_stats_t internal;
    h2_loader_memory_region_stats_t iram;
    h2_loader_memory_region_stats_t psram;
} h2_loader_memory_stats_t;

typedef h2_pal_result_t (*h2_loader_memory_stats_read_fn)(
    void *user,
    h2_loader_memory_stats_t *out_stats);

typedef struct h2_loader_memory_stats_api {
    void *user;
    h2_loader_memory_stats_read_fn read;
} h2_loader_memory_stats_api_t;

#ifdef __cplusplus
}
#endif

#endif
