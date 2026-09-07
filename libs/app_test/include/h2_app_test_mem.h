#ifndef H2_APP_TEST_MEM_H
#define H2_APP_TEST_MEM_H
#include "h2/pal/os/h2_pal_mem.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Serialized test allocator. NULL delegate uses C malloc/free; otherwise the
 * delegate is borrowed. Returned memory is aligned for max_align_t. Controls
 * and observations may only be accessed when calls are quiescent. Realloc
 * failure preserves the original block. No implicit freeing of leaked
 * allocations. */
typedef struct h2_app_test_mem {
  h2_pal_mem_api_t api;
  const h2_pal_mem_api_t *delegate;
  void *allocations;
  size_t calls, fail_at, live_blocks, live_bytes, peak_bytes;
} h2_app_test_mem_t;
/** Initialize fresh storage; fail_at == 0 disables failure injection, otherwise
 * that numbered allocation/reallocation fails once. Reset counters only after
 * live_blocks is zero. NULL input is ignored. */
void h2_app_test_mem_init(h2_app_test_mem_t *mem,
                          const h2_pal_mem_api_t *delegate);
/** Explicit test-process teardown for intentionally retained allocations.
 * All users must be stopped; invalidates every outstanding allocation. */
void h2_app_test_mem_release_all(h2_app_test_mem_t *mem);
#ifdef __cplusplus
}
#endif
#endif
