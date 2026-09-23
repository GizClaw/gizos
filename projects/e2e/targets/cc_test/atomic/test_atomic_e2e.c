#include "h2_atomic_e2e.h"
#include "h2_desktop_platform.h"
#include "h2/pal/core/h2_pal_errors.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>

int main(void) {
  const h2_atomic_e2e_backend_t *backends[] = {
      h2_atomic_e2e_h2_backend(), h2_atomic_e2e_c11_backend()};
  for (unsigned sample = 0; sample < 3u; ++sample) {
    for (unsigned i = 0; i < 2u; ++i) {
      h2_atomic_e2e_result_t result;
      int rc = h2_atomic_e2e_run(
          h2_desktop_platform_default_allocator(),
          h2_desktop_platform_task_api(), h2_desktop_platform_time_api(),
          backends[(sample + i) % 2u], 100000u, true, false,
          NULL, NULL, NULL, NULL, &result);
      printf("ATOMIC_E2E backend=%s sample=%u concurrent=%u expected=%u "
             "incremented=%u compared=%u elapsed_us=%" PRIu64 " rc=%d\n",
             backends[(sample + i) % 2u]->name, sample,
             (unsigned)result.concurrent, result.expected, result.incremented,
             result.compared, result.elapsed_us, rc);
      assert(rc == H2_PAL_OK);
    }
  }
  return 0;
}
