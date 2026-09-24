#include "h2_atomic_e2e.h"
#include "h2_atomic.h"
#include "h2/pal/core/h2_pal_errors.h"
#include <string.h>

typedef struct h2_state {
  h2_atomic_uint_t added;
  h2_atomic_uint_t cas;
  bool psram;
} h2_state_t;

static int create(const h2_pal_mem_api_t *mem, bool psram, void **out) {
  h2_state_t *state = h2_pal_mem_alloc(mem, sizeof(*state));
  if (state == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(state, 0, sizeof(*state));
  state->psram = psram;
  h2_atomic_result_t init_rc = h2_atomic_uint_init(&state->added, 0u);
  if (init_rc == H2_ATOMIC_OK)
    init_rc = h2_atomic_uint_init(&state->cas, 0u);
  if (init_rc != H2_ATOMIC_OK) {
    h2_atomic_uint_destroy(&state->added);
    h2_atomic_uint_destroy(&state->cas);
    h2_pal_mem_free(mem, state);
    return init_rc == H2_ATOMIC_UNSUPPORTED ? H2_PAL_ERR_UNSUPPORTED
                                            : H2_PAL_ERR_NO_MEMORY;
  }
  *out = state;
  return H2_PAL_OK;
}
static void destroy(const h2_pal_mem_api_t *mem, bool psram, void *opaque) {
  (void)psram;
  h2_state_t *state = opaque;
  h2_atomic_uint_destroy(&state->added);
  h2_atomic_uint_destroy(&state->cas);
  h2_pal_mem_free(mem, state);
}
static unsigned work(void *opaque, unsigned iterations) {
  h2_state_t *state = opaque;
  unsigned failures = 0u;
  for (unsigned i = 0u; i < iterations; ++i)
    (void)h2_atomic_uint_fetch_add(&state->added, 1u, H2_ATOMIC_SEQ_CST);
  for (unsigned i = 0u; i < iterations; ++i) {
    unsigned expected = h2_atomic_uint_load(&state->cas, H2_ATOMIC_SEQ_CST);
    bool done = false;
    const unsigned max_attempts = state->psram ? 32u : 1000000u;
    for (unsigned attempt = 0u; attempt < max_attempts; ++attempt) {
      if (h2_atomic_uint_compare_exchange(&state->cas, &expected,
                                          expected + 1u, H2_ATOMIC_SEQ_CST,
                                          H2_ATOMIC_SEQ_CST)) {
        done = true;
        break;
      }
    }
    if (!done) ++failures;
  }
  return failures;
}
static unsigned incremented(const void *opaque) {
  const h2_state_t *state = opaque;
  return h2_atomic_uint_load(&state->added, H2_ATOMIC_SEQ_CST);
}
static unsigned compared(const void *opaque) {
  const h2_state_t *state = opaque;
  return h2_atomic_uint_load(&state->cas, H2_ATOMIC_SEQ_CST);
}
static void addresses(const void *opaque, uintptr_t *wrapper,
                      uintptr_t *storage) {
  const h2_state_t *state = opaque;
  *wrapper = (uintptr_t)state;
  *storage = (uintptr_t)state->added.storage;
}
const h2_atomic_e2e_backend_t *h2_atomic_e2e_h2_backend(void) {
  static const h2_atomic_e2e_backend_t backend = {
      "h2_atomic", create, destroy, work, incremented, compared, addresses};
  return &backend;
}
