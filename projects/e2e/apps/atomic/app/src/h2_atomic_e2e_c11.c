/* This comparison suite is opt-in test code. No platform provider uses C11. */
#include "h2_atomic_e2e.h"
#include "h2/pal/core/h2_pal_errors.h"
#include <stdatomic.h>

typedef struct c11_state {
  atomic_uint added;
  atomic_uint cas;
  bool psram;
} c11_state_t;
static c11_state_t s_storage;
static atomic_flag s_in_use = ATOMIC_FLAG_INIT;

static int create(const h2_pal_mem_api_t *mem, bool psram, void **out) {
  /* Static storage is internal DRAM on ESP, matching the h2_atomic provider. */
  if (atomic_flag_test_and_set(&s_in_use))
    return H2_PAL_ERR_BUSY;
  c11_state_t *state = psram ? h2_pal_mem_alloc(mem, sizeof(*state))
                             : &s_storage;
  if (state == NULL) {
    atomic_flag_clear(&s_in_use);
    return H2_PAL_ERR_NO_MEMORY;
  }
  atomic_init(&state->added, 0u);
  atomic_init(&state->cas, 0u);
  state->psram = psram;
  *out = state;
  return H2_PAL_OK;
}
static void destroy(const h2_pal_mem_api_t *mem, bool psram, void *state) {
  if (psram) h2_pal_mem_free(mem, state);
  atomic_flag_clear(&s_in_use);
}
static unsigned work(void *opaque, unsigned iterations) {
  c11_state_t *state = opaque;
  unsigned failures = 0u;
  for (unsigned i = 0u; i < iterations; ++i)
    (void)atomic_fetch_add_explicit(&state->added, 1u, memory_order_seq_cst);
  for (unsigned i = 0u; i < iterations; ++i) {
    unsigned expected = atomic_load_explicit(&state->cas, memory_order_seq_cst);
    bool done = false;
    const unsigned max_attempts = state->psram ? 32u : 1000000u;
    for (unsigned attempt = 0u; attempt < max_attempts; ++attempt) {
      if (atomic_compare_exchange_weak_explicit(
              &state->cas, &expected, expected + 1u,
              memory_order_seq_cst, memory_order_seq_cst)) {
        done = true;
        break;
      }
    }
    if (!done) ++failures;
  }
  return failures;
}
static unsigned incremented(const void *opaque) {
  c11_state_t *state = (c11_state_t *)opaque;
  return atomic_load_explicit(&state->added, memory_order_seq_cst);
}
static unsigned compared(const void *opaque) {
  c11_state_t *state = (c11_state_t *)opaque;
  return atomic_load_explicit(&state->cas, memory_order_seq_cst);
}
static void addresses(const void *opaque, uintptr_t *wrapper,
                      uintptr_t *storage) {
  const c11_state_t *state = opaque;
  *wrapper = (uintptr_t)state;
  *storage = (uintptr_t)&state->added;
}
const h2_atomic_e2e_backend_t *h2_atomic_e2e_c11_backend(void) {
  static const h2_atomic_e2e_backend_t backend = {
      "c11", create, destroy, work, incremented, compared, addresses};
  return &backend;
}
