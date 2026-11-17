#ifndef H2_JAVASCRIPT_H
#define H2_JAVASCRIPT_H

#include "os/h2_pal_mem.h"
#include "os/h2_pal_time.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Synchronous native string bridge. Arguments borrow engine storage only
 * through the callback. The callback returns an optional result allocated
 * through memory; ownership transfers on every return, including failure.
 * result_length excludes NUL. The engine wipes/frees the allocation before
 * returning. Native callbacks serialize their own dependencies and must not
 * reenter the same evaluation. They may return NULL with zero length.
 */
typedef h2_pal_result_t (*h2_javascript_native_fn)(
    void* user, const char* method, const char* parameters,
    const h2_pal_mem_api_t* memory, char** result, size_t* result_length);

typedef struct h2_javascript_config {
  const h2_pal_mem_api_t* memory;
  const h2_pal_time_api_t* time;
  h2_javascript_native_fn native;
  void* user;
  size_t memory_limit;
  size_t stack_limit;
  uint32_t interrupt_limit;
} h2_javascript_config_t;

/** Evaluate one caller-supplied JavaScript program synchronously in a fresh
 * isolated interpreter. No file, network, modules, tasks or host globals are
 * exposed. __h2_native(method, parameters) is the optional string bridge.
 * Date uses an injected UTC snapshot, local timezone is UTC and Math.random
 * is the engine's time-seeded noncryptographic PRNG. Dependencies and source
 * remain borrowed until return; all owned buffers use Memory PAL.
 * memory_limit defaults to 2 MiB, stack_limit to 128 KiB and interrupt_limit
 * to 10000 interpreter checks. Limits bound interpreter work; native callback
 * work remains the callback owner's responsibility.
 * Returns the final value converted to a string, raw bytes without NUL.
 * NULL/undefined final values return NOT_FOUND. length resets to zero on
 * errors; NO_SPACE reports required size. Output bytes are preserved on
 * failure. NULL output and zero capacity measure a successful value.
 * Syntax/runtime exceptions return FORMAT; allocation/limit exhaustion returns
 * NO_MEMORY, and interrupt exhaustion returns TIMEOUT. Native errors retain
 * their PAL result. Asynchronous jobs are not dispatched.
 */
h2_pal_result_t h2_javascript_evaluate(const h2_javascript_config_t* config,
                                       const char* source, size_t source_length,
                                       uint8_t* output, size_t capacity,
                                       size_t* length);

#ifdef __cplusplus
}
#endif
#endif
