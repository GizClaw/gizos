#ifndef H2_PAL_CORE_BASELINE_H
#define H2_PAL_CORE_BASELINE_H
#include "h2_runtime.h"
#ifdef __cplusplus
extern "C" {
#endif
/** Baseline cases and retained cleanup owned by the independent PAL Core App.
 */
typedef struct h2_pal_core_cleanup h2_pal_core_cleanup_t;
typedef struct h2_pal_core_baseline_result {
  h2_pal_result_t cleanup_result;
  h2_pal_core_cleanup_t *retained_cleanup;
} h2_pal_core_baseline_result_t;
h2_pal_result_t h2_pal_core_baseline_time(h2_runtime_t *runtime);
h2_pal_result_t
h2_pal_core_baseline_timer(h2_runtime_t *runtime,
                           h2_pal_core_baseline_result_t *result);
h2_pal_result_t
h2_pal_core_baseline_task(h2_runtime_t *runtime,
                          h2_pal_core_baseline_result_t *result);
h2_pal_result_t
h2_pal_core_baseline_queue(h2_runtime_t *runtime,
                           h2_pal_core_baseline_result_t *result);
h2_pal_result_t
h2_pal_core_baseline_mutex(h2_runtime_t *runtime,
                           h2_pal_core_baseline_result_t *result);
h2_pal_result_t
h2_pal_core_baseline_semaphore(h2_runtime_t *runtime,
                               h2_pal_core_baseline_result_t *result);
h2_pal_result_t
h2_pal_core_baseline_condition(h2_runtime_t *runtime,
                               h2_pal_core_baseline_result_t *result);
h2_pal_result_t
h2_pal_core_baseline_concurrency(h2_runtime_t *runtime,
                                 h2_pal_core_baseline_result_t *result);
h2_pal_result_t
h2_pal_core_baseline_cleanup(h2_runtime_t *runtime,
                             h2_pal_core_baseline_result_t *result);
#ifdef __cplusplus
}
#endif
#endif
