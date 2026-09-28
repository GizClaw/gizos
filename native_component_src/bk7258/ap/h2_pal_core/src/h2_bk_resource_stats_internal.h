#ifndef H2_BK_RESOURCE_STATS_INTERNAL_H
#define H2_BK_RESOURCE_STATS_INTERNAL_H
#include <stddef.h>
typedef enum h2_bk_resource_kind {
    H2_BK_RESOURCE_TASK, H2_BK_RESOURCE_QUEUE, H2_BK_RESOURCE_MUTEX,
    H2_BK_RESOURCE_SEMAPHORE, H2_BK_RESOURCE_CONDITION, H2_BK_RESOURCE_TIMER,
    H2_BK_RESOURCE_COUNT
} h2_bk_resource_kind_t;
void h2_bk_resource_acquire(h2_bk_resource_kind_t kind, size_t stack_bytes);
void h2_bk_resource_release(h2_bk_resource_kind_t kind, size_t stack_bytes);
void h2_bk_memory_acquire(size_t bytes);
void h2_bk_memory_release(size_t bytes);
#endif
