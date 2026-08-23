#ifndef H2_LIBCO_SMOKE_H
#define H2_LIBCO_SMOKE_H

#include "h2_runtime.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_LIBCO_SMOKE_DEFAULT_STACK_SIZE (8u * 1024u)
#define H2_LIBCO_SMOKE_MIN_STACK_SIZE (2u * 1024u)
#define H2_LIBCO_SMOKE_MAX_STACK_SIZE (64u * 1024u)
#define H2_LIBCO_SMOKE_DEFAULT_SWITCH_ITERATIONS 10000u
#define H2_LIBCO_SMOKE_MAX_SWITCH_ITERATIONS 100000u

typedef struct h2_libco_smoke_config {
  /** Stack bytes allocated for every stackful coroutine, or zero for 8 KiB. */
  size_t task_stack_size;
  /** Total stress-phase cooperative switches, or zero for 10,000. */
  uint32_t switch_iterations;
} h2_libco_smoke_config_t;

/**
 * Run the blocking libco smoke scenario on the caller's current execution root.
 *
 * The borrowed Runtime and config must remain valid for the complete call.
 * Memory, monotonic Time, sleep, and Log operations are required. Coroutine
 * entries never call Runtime/PAL operations; only the executor root does.
 *
 * @param runtime Borrowed Runtime providing Memory, Time, and Log services.
 * @param config Optional borrowed bounded configuration; zero fields use fixed
 * defaults.
 * @return H2_PAL_OK after PASS, otherwise a nonzero PAL/libco error.
 */
int h2_libco_smoke_run(h2_runtime_t *runtime,
                       const h2_libco_smoke_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
