#ifndef H2_APP_TEST_FAULT_H
#define H2_APP_TEST_FAULT_H
#include "h2/pal/core/h2_pal_errors.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Caller-owned fault and attempt evidence. Zero initialization succeeds.
 * remaining failures are consumed by valid calls; UINT32_MAX means persistent.
 * calls saturates. Configure only while the provider is quiescent. These
 * testing providers are single-threaded: serialize calls and control/evidence
 * access, e.g. on one cooperative executor. They never provide production
 * thread safety. Embedded API objects borrow their enclosing object, which must
 * not move or be reinitialized while in use. All omitted PAL methods remain
 * unsupported. */
typedef struct h2_app_test_fault {
  h2_pal_result_t result;
  uint32_t remaining;
  uint32_t calls;
  /** Successful calls before consuming the armed failures. */
  uint32_t skip;
} h2_app_test_fault_t;

/** Record one attempt and consume an armed failure, otherwise return OK. */
static inline h2_pal_result_t
h2_app_test_fault_take(h2_app_test_fault_t *fault) {
  if (fault->calls != UINT32_MAX)
    ++fault->calls;
  if (fault->skip != 0u) {
    --fault->skip;
    return H2_PAL_OK;
  }
  if (fault->remaining == 0u)
    return H2_PAL_OK;
  if (fault->remaining != UINT32_MAX)
    --fault->remaining;
  return fault->result;
}

#ifdef __cplusplus
}
#endif
#endif
