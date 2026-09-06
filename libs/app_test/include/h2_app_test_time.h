#ifndef H2_APP_TEST_TIME_H
#define H2_APP_TEST_TIME_H
#include "h2/pal/os/h2_pal_time.h"
#include "h2_app_test_fault.h"
#ifdef __cplusplus
extern "C" {
#endif
/** Explicit virtual clock; sleep advances time without executing other tasks.
 * Wall time is unavailable until set, and then advances with monotonic time.
 * Do not use this sleep implementation to drive resident production task loops;
 * use the libco Time PAL for cooperative yielding. All units are milliseconds.
 */
typedef struct h2_app_test_time {
  h2_pal_time_api_t api;
  uint64_t monotonic_ms;
  uint64_t wall_ms;
  h2_pal_time_wall_status_t wall_status;
  h2_app_test_fault_t read, sleep, set_wall;
  uint32_t last_sleep_ms;
} h2_app_test_time_t;
/** Initialize caller storage; NULL is a no-op. No allocation or destroy needed.
 */
void h2_app_test_time_init(h2_app_test_time_t *clock, uint64_t initial_ms);
/** Advance both clocks atomically; overflow returns NO_SPACE without mutation.
 */
h2_pal_result_t h2_app_test_time_advance(h2_app_test_time_t *clock,
                                         uint64_t delta_ms);

#ifdef __cplusplus
}
#endif
#endif
