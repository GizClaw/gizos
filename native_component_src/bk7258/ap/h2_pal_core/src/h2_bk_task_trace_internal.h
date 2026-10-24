#ifndef H2_BK_TASK_TRACE_INTERNAL_H
#define H2_BK_TASK_TRACE_INTERNAL_H

#include <stdint.h>

enum {
  H2_BK_TASK_TRACE_TASKS = 64,
  H2_BK_TASK_TRACE_FRAMES = 32,
  H2_BK_TASK_TRACE_NAME = 64,
  H2_BK_TASK_TRACE_WORDS = 4096,
};

typedef struct h2_bk_task_trace_row {
  uintptr_t identity, base, top, bottom;
  uint32_t size, count, truncated, invalid;
  char name[H2_BK_TASK_TRACE_NAME];
  uint32_t frames[H2_BK_TASK_TRACE_FRAMES];
} h2_bk_task_trace_row_t;

typedef struct h2_bk_task_trace_snapshot {
  uint32_t count, omitted, truncated, invalid;
  h2_bk_task_trace_row_t rows[H2_BK_TASK_TRACE_TASKS];
} h2_bk_task_trace_snapshot_t;

#ifdef __cplusplus
extern "C" {
#endif
/* Compiled inside the pinned kernel build copy; owns no retained pointers. */
void h2_bk_task_trace_capture(h2_bk_task_trace_snapshot_t* snapshot);
#ifdef __cplusplus
}
#endif
#endif
