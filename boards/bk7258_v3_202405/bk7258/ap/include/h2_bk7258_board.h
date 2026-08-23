#ifndef H2_BK7258_BOARD_H
#define H2_BK7258_BOARD_H

#include "h2_runtime.h"
#include "h2/pal/os/h2_pal_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*h2_bk7258_board_entry_task_fn)(void *user);

h2_pal_result_t h2_bk7258_board_runtime_config(h2_runtime_config_t *out_config);

/**
 * Tears down board-owned Runtime providers after h2_runtime_deinit().
 *
 * All HTTP requests must have returned before this call. The teardown destroys
 * the HTTP provider before releasing its filesystem dependency. A caller whose
 * h2_runtime_init() fails after runtime_config() succeeds must also call this
 * function. Repeated calls after teardown return H2_PAL_OK.
 */
h2_pal_result_t h2_bk7258_board_runtime_deinit(void);

/** Board PSRAM allocator for large target-specific working sets. */
h2_pal_mem_api_t *h2_bk7258_board_psram_allocator(void);

/**
 * Starts the board image entry point on a detached board-owned worker task.
 *
 * The caller must return from the SDK user entry after this function succeeds.
 * The supplied user pointer must remain valid until the entry function returns.
 * The entry function must release Runtime before returning; the task wrapper
 * then calls h2_bk7258_board_runtime_deinit() before deleting itself.
 */
h2_pal_result_t h2_bk7258_board_start_entry_task(
    const char *name,
    h2_bk7258_board_entry_task_fn entry,
    void *user);

#ifdef __cplusplus
}
#endif

#endif
