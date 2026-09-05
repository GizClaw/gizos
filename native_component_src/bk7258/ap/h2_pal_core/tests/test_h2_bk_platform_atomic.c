/* Host test for the BK7258 AP 64-bit __atomic_*_8 libcall shims.
 *
 * The shim translation unit is included with its exported names renamed so a
 * host compiler never sees the reserved __atomic_*_8 identifiers, which GCC
 * treats as builtins and would inline instead of calling. The critical-section
 * hooks are stubbed here to prove every helper takes exactly one balanced
 * critical section around its access. */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

#define __atomic_load_8 h2_test_atomic_load_8
#define __atomic_store_8 h2_test_atomic_store_8
#define __atomic_exchange_8 h2_test_atomic_exchange_8
#define __atomic_compare_exchange_8 h2_test_atomic_compare_exchange_8
#define __atomic_fetch_add_8 h2_test_atomic_fetch_add_8
#define __atomic_fetch_sub_8 h2_test_atomic_fetch_sub_8
#define __atomic_fetch_and_8 h2_test_atomic_fetch_and_8
#define __atomic_fetch_or_8 h2_test_atomic_fetch_or_8
#define __atomic_fetch_xor_8 h2_test_atomic_fetch_xor_8
#include "h2_bk_platform_atomic.c"

/* GCC libcall ABI signatures; assigning the helpers to these pointers fails
 * to compile if a signature drifts. */
typedef unsigned long long (*rmw_fn_t)(volatile void *, unsigned long long,
                                       int);
typedef _Bool (*cas_fn_t)(volatile void *, void *, unsigned long long, _Bool,
                          int, int);
typedef unsigned long long (*load_fn_t)(const volatile void *, int);
typedef void (*store_fn_t)(volatile void *, unsigned long long, int);

#define TEST_IRQ_LEVEL 0x5au

static unsigned enter_calls;
static unsigned exit_calls;
static int depth;

uint32_t rtos_enter_critical(void) {
  assert(depth == 0);
  depth++;
  enter_calls++;
  return TEST_IRQ_LEVEL;
}

void rtos_exit_critical(uint32_t irq_level) {
  assert(irq_level == TEST_IRQ_LEVEL);
  assert(depth == 1);
  depth--;
  exit_calls++;
}

static void expect_one_critical_section(void) {
  assert(depth == 0);
  assert(enter_calls == 1u);
  assert(exit_calls == 1u);
  enter_calls = 0u;
  exit_calls = 0u;
}

/* Values with distinct high and low words catch 32-bit truncation. */
#define V_A 0x0123456789abcdefull
#define V_B 0xfedcba9876543210ull

static void test_load_store(void) {
  volatile unsigned long long target = 0u;
  const store_fn_t store = h2_test_atomic_store_8;
  const load_fn_t load = h2_test_atomic_load_8;

  store(&target, V_A, 0);
  expect_one_critical_section();
  assert(target == V_A);
  assert(load(&target, 0) == V_A);
  expect_one_critical_section();
}

static void test_exchange(void) {
  volatile unsigned long long target = V_A;
  const rmw_fn_t exchange = h2_test_atomic_exchange_8;

  assert(exchange(&target, V_B, 0) == V_A);
  expect_one_critical_section();
  assert(target == V_B);
  assert(exchange(&target, 0u, 0) == V_B);
  expect_one_critical_section();
  assert(target == 0u);
}

static void test_compare_exchange_match(void) {
  volatile unsigned long long target = V_A;
  unsigned long long expected = V_A;
  const cas_fn_t cas = h2_test_atomic_compare_exchange_8;

  assert(cas(&target, &expected, V_B, 0, 0, 0));
  expect_one_critical_section();
  assert(target == V_B);
  assert(expected == V_A);
}

static void test_compare_exchange_mismatch(void) {
  volatile unsigned long long target = V_A;
  unsigned long long expected = V_B;
  const cas_fn_t cas = h2_test_atomic_compare_exchange_8;

  assert(!cas(&target, &expected, 7u, 0, 0, 0));
  expect_one_critical_section();
  assert(target == V_A);
  assert(expected == V_A);
}

static void test_compare_exchange_weak_never_spuriously_fails(void) {
  volatile unsigned long long target = 5u;
  unsigned long long expected = 5u;
  const cas_fn_t cas = h2_test_atomic_compare_exchange_8;

  for (int i = 0; i < 64; ++i) {
    assert(cas(&target, &expected, expected + 1u, 1, 0, 0));
    expect_one_critical_section();
    expected++;
  }
  assert(target == 69u);
}

static void test_fetch_op(rmw_fn_t op, unsigned long long initial,
                          unsigned long long operand,
                          unsigned long long result) {
  volatile unsigned long long target = initial;

  assert(op(&target, operand, 0) == initial);
  expect_one_critical_section();
  assert(target == result);
}

static void test_fetch_ops(void) {
  /* add/sub carry across the 32-bit boundary. */
  test_fetch_op(h2_test_atomic_fetch_add_8, 0xffffffffull, 1u,
                0x100000000ull);
  test_fetch_op(h2_test_atomic_fetch_add_8, V_A, V_B, V_A + V_B);
  test_fetch_op(h2_test_atomic_fetch_sub_8, 0x100000000ull, 1u,
                0xffffffffull);
  test_fetch_op(h2_test_atomic_fetch_sub_8, 0u, 1u, ~0ull);
  test_fetch_op(h2_test_atomic_fetch_and_8, V_A, 0xffffffff00000000ull,
                0x0123456700000000ull);
  test_fetch_op(h2_test_atomic_fetch_or_8, 0x0123456700000000ull,
                0x0000000089abcdefull, V_A);
  test_fetch_op(h2_test_atomic_fetch_xor_8, V_A, V_A, 0u);
  test_fetch_op(h2_test_atomic_fetch_xor_8, V_A, ~0ull, ~V_A);
}

int main(void) {
  test_load_store();
  test_exchange();
  test_compare_exchange_match();
  test_compare_exchange_mismatch();
  test_compare_exchange_weak_never_spuriously_fails();
  test_fetch_ops();
  return EXIT_SUCCESS;
}
