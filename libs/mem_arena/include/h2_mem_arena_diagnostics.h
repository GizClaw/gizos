#ifndef H2_MEM_ARENA_DIAGNOSTICS_H
#define H2_MEM_ARENA_DIAGNOSTICS_H

#include "h2_mem_arena.h"
#include "h2/pal/os/h2_pal_fs.h"
#include "h2/pal/os/h2_pal_log.h"
#include "h2/pal/os/h2_pal_queue.h"
#include "h2/pal/os/h2_pal_sync.h"
#include "h2/pal/os/h2_pal_task.h"
#include "h2/pal/os/h2_pal_time.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_mem_arena_diagnostics h2_mem_arena_diagnostics_t;

/* Optional platform probe. safe_copy must tolerate concurrent application
 * writes without a C data race. The diagnostics lock prevents free/realloc
 * during each bounded copy. capture_frames returns caller-first addresses;
 * absent/unsupported callbacks lose only content/site evidence. */
typedef struct h2_mem_arena_diagnostics_probe {
    void *user;
    h2_pal_result_t (*safe_copy)(void *user, const void *source,
                                  void *destination, size_t bytes);
    h2_pal_result_t (*capture_frames)(void *user, uintptr_t *out_frames,
                                       size_t capacity, size_t *out_count);
} h2_mem_arena_diagnostics_probe_t;

typedef struct h2_mem_arena_diagnostics_config {
    const char *name; /* Borrowed until destroy. */
    void *backing; /* Caller-owned region, borrowed until destroy. */
    size_t backing_bytes;
    size_t small_request_max;
    size_t small_pool_bytes;
    const h2_pal_mem_api_t *fallback; /* Borrowed until destroy. */
    const h2_pal_mem_api_t *metadata; /* Outside measured arena. */
    const h2_pal_sync_api_t *sync;
    const h2_pal_task_api_t *task; /* NULL selects synchronous reports. */
    const h2_pal_queue_api_t *queue; /* Required when task is non-NULL. */
    const h2_pal_time_api_t *time; /* Optional peak-sampling clock. */
    const h2_pal_fs_api_t *report_fs; /* Restricted caller-owned mount. */
    const char *report_path; /* Virtual path within report_fs, not host root. */
    const h2_pal_log_api_t *log; /* Optional console summary sink. */
    const char *log_scope; /* Borrowed until destroy. */
    const char *const *tags; /* Unique borrowed names until destroy. */
    size_t tag_count;
    size_t stack_tag_index; /* Exempt from pattern/content scans. */
    h2_mem_arena_diagnostics_probe_t probe;
    bool trace;
    size_t block_capacity;
    size_t site_capacity;
    size_t peak_capacity;
    size_t duplicate_capacity;
    size_t report_queue_capacity;
    size_t top_count;
    const char *report_task_name; /* Borrowed until destroy. */
    size_t report_task_stack_size;
    const char *case_end_prefix; /* NULL disables case-end retention analysis. */
    uintptr_t symbol_anchor;
    const char *symbol_anchor_name;
} h2_mem_arena_diagnostics_config_t;

/** Create optional C11 diagnostics. All metadata/queue storage is bounded and
 * allocated outside the arena. On failure *out is NULL and no borrowed owner
 * is released. The caller may continue using its normal Runtime allocator. */
h2_pal_result_t h2_mem_arena_diagnostics_create(
    const h2_mem_arena_diagnostics_config_t *config,
    h2_mem_arena_diagnostics_t **out);

/** Borrow a tag Memory PAL until destroy; unknown tags map to index zero.
 * Any view may free/realloc a block even after diagnostic metadata overflow. */
const h2_pal_mem_api_t *h2_mem_arena_diagnostics_mem(
    h2_mem_arena_diagnostics_t *diagnostics, const char *tag);

/** Copy core pool statistics. NULL diagnostics zeros output and returns
 * INVALID_STATE. */
h2_pal_result_t h2_mem_arena_diagnostics_stats(
    h2_mem_arena_diagnostics_t *diagnostics,
    h2_mem_arena_stats_t *out_stats);

/** Capture metadata for one phase; content/report formatting runs on the
 * supplied Task PAL when available. Queue capacity is finite; full queues
 * backpressure the report caller, never grow without bound. */
h2_pal_result_t h2_mem_arena_diagnostics_report(
    h2_mem_arena_diagnostics_t *diagnostics, const char *phase);

/** Wait for all reports submitted before this call. */
h2_pal_result_t h2_mem_arena_diagnostics_flush(
    h2_mem_arena_diagnostics_t *diagnostics);

/** At most once per second, request a content sample after the aggregate
 * live peak grows by at least 32 KiB. No-op when trace is disabled. */
h2_pal_result_t h2_mem_arena_diagnostics_poll(
    h2_mem_arena_diagnostics_t *diagnostics);

/** Stop/join reporter, emit END and release metadata only after every
 * borrower has stopped. Live blocks return INVALID_STATE and keep the handle;
 * NULL succeeds. The caller frees backing only after successful destroy. */
h2_pal_result_t h2_mem_arena_diagnostics_destroy(
    h2_mem_arena_diagnostics_t *diagnostics);

#ifdef __cplusplus
}
#endif
#endif
