#ifndef H2_BK_TASK_TRACE_H
#define H2_BK_TASK_TRACE_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stddef.h>

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
 * This legacy entry uses the SDK dump console. When that UART has a managed
 * transport owner, use h2_bk_task_trace_dump_with_writer() instead.
 */
h2_pal_result_t h2_bk_task_trace_dump(void);

/** Synchronously consume a borrowed complete record. Set out_written to the
 * accepted byte count. The record is a byte span, valid only during the call.
 * The writer owns transport serialization and any blocking deadline. */
typedef h2_pal_result_t (*h2_bk_task_trace_write_record_fn)(
    void* user, const char* record, size_t length, size_t* out_written);

/**
 * @brief Emit the same owned snapshot through the caller's output owner.
 *
 * Use this entry when task diagnostics share a UART with H2Loader or another
 * transport owner. Each callback receives a complete CRLF-terminated record,
 * outside the kernel lock and with interrupts enabled. write_record is
 * required; user is optional and borrowed until this synchronous call ends.
 * No output fallback or retry occurs. A writer error is returned unchanged;
 * an OK short write returns IO. Snapshot storage is freed on every exit.
 * Context, snapshot bounds and other results match h2_bk_task_trace_dump().
 */
h2_pal_result_t h2_bk_task_trace_dump_with_writer(
    h2_bk_task_trace_write_record_fn write_record, void* user);

#ifdef __cplusplus
}
#endif
#endif
