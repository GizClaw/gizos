#ifndef H2_H2LOADER_HOST_SCHEDULER_H
#define H2_H2LOADER_HOST_SCHEDULER_H

#include "h2_h2loader_host.h"

#ifdef __cplusplus
extern "C" {
#endif

#define H2_H2LOADER_HOST_FIXTURE_SLOT_MAX_LEN 96u
#define H2_H2LOADER_HOST_ERROR_DETAIL_MAX_LEN 256u

typedef enum h2_h2loader_host_job_state {
    H2_H2LOADER_HOST_JOB_QUEUED = 1,
    H2_H2LOADER_HOST_JOB_RUNNING = 2,
    H2_H2LOADER_HOST_JOB_SUCCEEDED = 3,
    H2_H2LOADER_HOST_JOB_FAILED = 4,
    H2_H2LOADER_HOST_JOB_CANCELLED = 5,
} h2_h2loader_host_job_state_t;

typedef struct h2_h2loader_host_job_input {
    char fixture_slot[H2_H2LOADER_HOST_FIXTURE_SLOT_MAX_LEN];
    h2_h2loader_host_candidate_t candidate;
    h2_h2loader_host_catalog_entry_t asset;
} h2_h2loader_host_job_input_t;

typedef struct h2_h2loader_host_job_result {
    h2_h2loader_host_job_input_t input;
    h2_h2loader_host_status_t final_status;
    h2_h2loader_host_job_state_t state;
    h2_pal_result_t result;
    uint64_t started_ms;
    uint64_t finished_ms;
    uint32_t retry_count;
    char error_detail[H2_H2LOADER_HOST_ERROR_DETAIL_MAX_LEN];
} h2_h2loader_host_job_result_t;

typedef struct h2_h2loader_host_scheduler h2_h2loader_host_scheduler_t;

typedef struct h2_h2loader_host_scheduler_config {
    const h2_pal_mem_api_t *allocator;
    const h2_h2loader_host_job_input_t *jobs;
    size_t job_count;
    size_t max_concurrency;
} h2_h2loader_host_scheduler_config_t;

typedef h2_pal_result_t (*h2_h2loader_host_export_write_fn)(
    void *user,
    const uint8_t *bytes,
    size_t byte_count);

/**
 * @brief Freeze one operation's asset and candidate snapshots.
 *
 * Inputs are copied. max_concurrency is enforced by claim(); execution and
 * worker ownership remain with the caller so each active job can use an
 * isolated transport state machine. Scheduler API calls are serialized by the
 * caller (normally the controller/LVGL thread); workers return completions
 * through the caller's bounded queue and do not mutate the scheduler directly.
 */
h2_pal_result_t h2_h2loader_host_scheduler_open(
    const h2_h2loader_host_scheduler_config_t *config,
    h2_h2loader_host_scheduler_t **out_scheduler);

h2_pal_result_t h2_h2loader_host_scheduler_close(
    h2_h2loader_host_scheduler_t **inout_scheduler);

/** Claim the next queued job, or return WOULD_BLOCK/FULL. */
h2_pal_result_t h2_h2loader_host_scheduler_claim(
    h2_h2loader_host_scheduler_t *scheduler,
    uint64_t now_ms,
    size_t *out_index,
    h2_h2loader_host_job_input_t *out_input);

/** Complete one running job without affecting any other slot. */
h2_pal_result_t h2_h2loader_host_scheduler_complete(
    h2_h2loader_host_scheduler_t *scheduler,
    size_t index,
    h2_pal_result_t result,
    const h2_h2loader_host_status_t *final_status,
    const char *error_detail,
    uint64_t now_ms);

/**
 * Pause or resume new claims.
 *
 * Running jobs are not interrupted. The controller remains responsible for
 * safely cancelling an active transport at a documented I/O boundary.
 */
h2_pal_result_t h2_h2loader_host_scheduler_set_paused(
    h2_h2loader_host_scheduler_t *scheduler,
    int paused);

/** Cancel all queued jobs. Active workers remain caller-owned and isolated. */
h2_pal_result_t h2_h2loader_host_scheduler_cancel_queued(
    h2_h2loader_host_scheduler_t *scheduler,
    uint64_t now_ms,
    size_t *out_cancelled);

/** Requeue exactly one failed/cancelled job and increment its retry count. */
h2_pal_result_t h2_h2loader_host_scheduler_retry(
    h2_h2loader_host_scheduler_t *scheduler,
    size_t index);

h2_pal_result_t h2_h2loader_host_scheduler_get(
    const h2_h2loader_host_scheduler_t *scheduler,
    size_t index,
    h2_h2loader_host_job_result_t *out_result);

h2_pal_result_t h2_h2loader_host_scheduler_export_json(
    const h2_h2loader_host_scheduler_t *scheduler,
    h2_h2loader_host_export_write_fn write,
    void *write_user);

h2_pal_result_t h2_h2loader_host_scheduler_export_csv(
    const h2_h2loader_host_scheduler_t *scheduler,
    h2_h2loader_host_export_write_fn write,
    void *write_user);

#ifdef __cplusplus
}
#endif

#endif
