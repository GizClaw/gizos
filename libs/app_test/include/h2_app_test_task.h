#ifndef H2_APP_TEST_TASK_H
#define H2_APP_TEST_TASK_H
#include "h2/pal/os/h2_pal_task.h"
#include "h2_app_test_fault.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Single-slot deterministic Task PAL for serialized lifecycle tests. This does
 * not simulate concurrency: run explicitly executes the entry on the caller.
 * run_on_start optionally runs it before start returns. Join refuses a pending
 * entry with BUSY and preserves ownership on injected failure. Use libco/real
 * task PALs when validating concurrent production loops. */
typedef struct h2_app_test_task {
  h2_pal_task_api_t api;
  h2_pal_task_entry_t entry;
  void *context;
  h2_pal_task_options_t options;
  h2_app_test_fault_t start, join;
  bool active, running, complete, run_on_start;
} h2_app_test_task_t;
void h2_app_test_task_init(h2_app_test_task_t *task);
h2_pal_result_t h2_app_test_task_run(h2_app_test_task_t *task);
#ifdef __cplusplus
}
#endif
#endif
