/* This comparison suite is opt-in test code. No platform provider uses C11. */
#include "h2_atomic_e2e.h"
#include "h2/pal/core/h2_pal_errors.h"
#include <stdatomic.h>

typedef struct c11_state {
  atomic_uint added;
  atomic_uint cas;
} c11_state_t;
static c11_state_t s_storage;
static atomic_flag s_in_use = ATOMIC_FLAG_INIT;

static int create(const h2_pal_mem_api_t *mem, void **out) {
  (void)mem;
  /* Static storage is internal DRAM on ESP, matching the h2_atomic provider. */
  if (atomic_flag_test_and_set(&s_in_use))
    return H2_PAL_ERR_BUSY;
  c11_state_t *state = &s_storage;
  atomic_init(&state->added, 0u);
  atomic_init(&state->cas, 0u);
  *out = state;
  return H2_PAL_OK;
}
static void destroy(const h2_pal_mem_api_t *mem, void *state) {
  (void)mem;
  (void)state;
  atomic_flag_clear(&s_in_use);
}
static void work(void *opaque, unsigned iterations) {
  c11_state_t *state = opaque;
  for (unsigned i = 0u; i < iterations; ++i)
    (void)atomic_fetch_add_explicit(&state->added, 1u, memory_order_seq_cst);
  for (unsigned i = 0u; i < iterations; ++i) {
    unsigned expected = atomic_load_explicit(&state->cas, memory_order_seq_cst);
    while (!atomic_compare_exchange_weak_explicit(
        &state->cas, &expected, expected + 1u,
        memory_order_seq_cst, memory_order_seq_cst)) {}
  }
}
static unsigned incremented(const void *opaque) {
  const c11_state_t *state = opaque;
  return atomic_load_explicit(&state->added, memory_order_seq_cst);
}
static unsigned compared(const void *opaque) {
  const c11_state_t *state = opaque;
  return atomic_load_explicit(&state->cas, memory_order_seq_cst);
}
const h2_atomic_e2e_backend_t *h2_atomic_e2e_c11_backend(void) {
  static const h2_atomic_e2e_backend_t backend = {
      "c11", create, destroy, work, incremented, compared};
  return &backend;
}
