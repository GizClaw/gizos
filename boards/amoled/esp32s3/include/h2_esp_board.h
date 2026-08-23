#ifndef H2_ESP_BOARD_H
#define H2_ESP_BOARD_H

#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*h2_esp_board_entry_task_fn)(void *user);

h2_pal_result_t h2_esp_board_runtime_config(h2_runtime_config_t *out_config);

/**
 * Tears down board-owned Runtime providers after h2_runtime_deinit().
 *
 * All HTTP requests must have returned before this call. The teardown destroys
 * the HTTP provider before releasing its filesystem dependency. A caller whose
 * h2_runtime_init() fails after runtime_config() succeeds must also call this
 * function. Repeated calls after teardown return H2_PAL_OK.
 */
h2_pal_result_t h2_esp_board_runtime_deinit(void);

/**
 * Starts the board image entry point on a detached PSRAM-backed task.
 *
 * The caller must return from app_main after this function succeeds. The
 * supplied user pointer must remain valid until the entry function returns.
 * The entry function must release Runtime before returning; the task wrapper
 * then calls h2_esp_board_runtime_deinit() before deleting itself.
 */
h2_pal_result_t h2_esp_board_start_entry_task(
    const char *name,
    h2_esp_board_entry_task_fn entry,
    void *user);

#ifdef __cplusplus
}
#endif

#endif
