#include "h2_bk_task_trace.h"
#include "h2_bk_task_trace_internal.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

#ifdef H2_BK_TASK_TRACE_HOST_TEST
#include "h2_bk_task_trace_test_sdk.h"
#else
#include "armstar.h"
#include <common/bk_assert.h>
#include <os/mem.h>
#endif

#if defined(H2_BK_TASK_TRACE_HOST_TEST) || \
    (defined(CONFIG_FREERTOS_SMP) && CONFIG_FREERTOS_SMP)
static h2_pal_result_t write_complete(
    h2_bk_task_trace_write_record_fn write_record, void* user,
    const char* record, size_t length) {
  size_t written = 0u;
  const h2_pal_result_t rc = write_record(user, record, length, &written);
  if (rc != H2_PAL_OK) return rc;
  return written == length ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static bool append_record(char* record, size_t capacity, size_t* used,
                          const char* format, ...) {
  if (*used >= capacity) return false;
  va_list args;
  va_start(args, format);
  const int length = vsnprintf(record + *used, capacity - *used, format, args);
  va_end(args);
  if (length < 0 || (size_t)length >= capacity - *used) return false;
  *used += (size_t)length;
  return true;
}

h2_pal_result_t h2_bk_task_trace_dump_with_writer(
    h2_bk_task_trace_write_record_fn write_record, void* user) {
  if (write_record == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (__get_IPSR() != 0u || __get_PRIMASK() != 0u || __get_BASEPRI() != 0u)
    return H2_PAL_ERR_INVALID_STATE;
  h2_bk_task_trace_snapshot_t* snapshot = psram_malloc(sizeof(*snapshot));
  if (snapshot == NULL) return H2_PAL_ERR_NO_MEMORY;
  h2_bk_task_trace_capture(snapshot);
  char record[192u + H2_BK_TASK_TRACE_FRAMES * (2u * sizeof(uintptr_t) + 1u)];
  size_t used = 0u;
  h2_pal_result_t rc = H2_PAL_ERR_IO;
  if (!append_record(
          record, sizeof(record), &used,
          "H2_BK_TASK_TRACE rows=%u omitted=%u truncated=%u invalid=%u\r\n",
          (unsigned)snapshot->count, (unsigned)snapshot->omitted,
          (unsigned)snapshot->truncated, (unsigned)snapshot->invalid))
    goto cleanup;
  rc = write_complete(write_record, user, record, used);
  if (rc != H2_PAL_OK) goto cleanup;
  static const char columns[] =
      "task               stack_addr                  top          "
      "size overflow  backtrace\r\n";
  rc = write_complete(write_record, user, columns, sizeof(columns) - 1u);
  if (rc != H2_PAL_OK) goto cleanup;
  for (uint32_t i = 0u; i < snapshot->count; ++i) {
    const h2_bk_task_trace_row_t* row = &snapshot->rows[i];
    used = 0u;
    rc = H2_PAL_ERR_IO;
    if (!append_record(
            record, sizeof(record), &used,
            "%-16s   [0x%-6lx ~ 0x%-6lx]   0x%-6lx   %-4lu   %-8u   ",
            row->name, (unsigned long)row->base, (unsigned long)row->bottom,
            (unsigned long)row->top, (unsigned long)row->size,
            (unsigned)row->invalid))
      goto cleanup;
    for (uint32_t frame = 0u; frame < row->count; ++frame)
      if (!append_record(record, sizeof(record), &used, "%lx ",
                         (unsigned long)row->frames[frame]))
        goto cleanup;
    if (!append_record(record, sizeof(record), &used, "\r\n")) goto cleanup;
    rc = write_complete(write_record, user, record, used);
    if (rc != H2_PAL_OK) goto cleanup;
  }
  rc = snapshot->invalid != 0u ? H2_PAL_ERR_INVALID_STATE
       : snapshot->omitted != 0u || snapshot->truncated != 0u
           ? H2_PAL_ERR_NO_SPACE
           : H2_PAL_OK;
cleanup:
  os_free(snapshot);
  return rc;
}

static h2_pal_result_t legacy_dump_write(void* user, const char* record,
                                         size_t length, size_t* out_written) {
  (void)user;
  *out_written = 0u;
  /* The pinned SDK's assert formatter has a small fixed buffer and returns
   * a boolean, not a byte count. Preserve its wire output in short pieces.
   * Managed task-context callers should supply their own writer instead. */
  while (*out_written < length) {
    const size_t remaining = length - *out_written;
    const int piece = (int)(remaining < 64u ? remaining : 64u);
    if (BK_DUMP_OUT("%.*s", piece, record + *out_written) < 0)
      return H2_PAL_ERR_IO;
    *out_written += (size_t)piece;
  }
  return H2_PAL_OK;
}

h2_pal_result_t h2_bk_task_trace_dump(void) {
  return h2_bk_task_trace_dump_with_writer(legacy_dump_write, NULL);
}
#else
h2_pal_result_t h2_bk_task_trace_dump(void) { return H2_PAL_ERR_UNSUPPORTED; }
h2_pal_result_t h2_bk_task_trace_dump_with_writer(
    h2_bk_task_trace_write_record_fn write_record, void* user) {
  (void)user;
  return write_record == NULL ? H2_PAL_ERR_INVALID_ARG : H2_PAL_ERR_UNSUPPORTED;
}
#endif
