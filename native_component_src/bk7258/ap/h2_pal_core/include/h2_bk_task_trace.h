#ifndef H2_BK_TASK_TRACE_H
#define H2_BK_TASK_TRACE_H

#include "h2/pal/core/h2_pal_errors.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Print an owned, bounded snapshot of AP task stacks.
 *
 * Call only from a task with interrupts enabled. Task-list traversal, names
 * and code-address scans are copied under the FreeRTOS SMP kernel lock;
 * allocation and output run outside that lock. Running tasks on the other
 * core can still change their stack words: this is a diagnostic address
 * scan, not a frame unwind. No task handle or borrowed name is used during
 * output. All temporary PSRAM storage is released before returning.
 *
 * @return OK on a complete snapshot; NO_SPACE if task/frame/scan bounds were
 * reached; INVALID_STATE for masked/exception context or invalid stack bounds;
 * NO_MEMORY if snapshot storage cannot be allocated; IO on output failure;
 * UNSUPPORTED for a kernel without the AP SMP bridge.
 * Partial snapshots are
 * explicitly marked in the output and cannot establish test qualification.
 */
h2_pal_result_t h2_bk_task_trace_dump(void);

#ifdef __cplusplus
}
#endif
#endif
