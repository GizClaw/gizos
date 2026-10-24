#include "h2_bk_task_trace.h"
#include "h2_bk_task_trace_internal.h"

#ifdef H2_BK_TASK_TRACE_HOST_TEST
#include "h2_bk_task_trace_test_sdk.h"
#else
#include "armstar.h"
#include <common/bk_assert.h>
#include <os/mem.h>
#endif

#if defined(H2_BK_TASK_TRACE_HOST_TEST) || \
    (defined(CONFIG_FREERTOS_SMP) && CONFIG_FREERTOS_SMP)
#define TRACE_OUTPUT(...)               \
  do {                                  \
    if (BK_DUMP_OUT(__VA_ARGS__) < 0) { \
      output_error = 1;                 \
    }                                   \
  } while (0)

h2_pal_result_t h2_bk_task_trace_dump(void) {
  if (__get_IPSR() != 0u || __get_PRIMASK() != 0u || __get_BASEPRI() != 0u)
    return H2_PAL_ERR_INVALID_STATE;
  h2_bk_task_trace_snapshot_t* snapshot = psram_malloc(sizeof(*snapshot));
  if (snapshot == NULL) return H2_PAL_ERR_NO_MEMORY;
  h2_bk_task_trace_capture(snapshot);
  int output_error = 0;
  TRACE_OUTPUT(
      "H2_BK_TASK_TRACE rows=%u omitted=%u truncated=%u invalid=%u\r\n",
      (unsigned)snapshot->count, (unsigned)snapshot->omitted,
      (unsigned)snapshot->truncated, (unsigned)snapshot->invalid);
  TRACE_OUTPUT(
      "task               stack_addr                  top          "
      "size overflow  backtrace\r\n");
  for (uint32_t i = 0u; i < snapshot->count; ++i) {
    const h2_bk_task_trace_row_t* row = &snapshot->rows[i];
    TRACE_OUTPUT("%-16s   [0x%-6lx ~ 0x%-6lx]   0x%-6lx   %-4lu   %-8u   ",
                 row->name, (unsigned long)row->base,
                 (unsigned long)row->bottom, (unsigned long)row->top,
                 (unsigned long)row->size, (unsigned)row->invalid);
    for (uint32_t frame = 0u; frame < row->count; ++frame)
      TRACE_OUTPUT("%lx ", (unsigned long)row->frames[frame]);
    TRACE_OUTPUT("\r\n");
  }
  const h2_pal_result_t rc =
      output_error              ? H2_PAL_ERR_IO
      : snapshot->invalid != 0u ? H2_PAL_ERR_INVALID_STATE
      : snapshot->omitted != 0u || snapshot->truncated != 0u
          ? H2_PAL_ERR_NO_SPACE
          : H2_PAL_OK;
  os_free(snapshot);
  return rc;
}
#else
h2_pal_result_t h2_bk_task_trace_dump(void) { return H2_PAL_ERR_UNSUPPORTED; }
#endif
