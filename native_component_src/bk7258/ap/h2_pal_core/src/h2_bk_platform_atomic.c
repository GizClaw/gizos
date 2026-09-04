#include <os/os.h>

#include <stdint.h>

unsigned long long __atomic_load_8(const volatile void *address,
                                   int memory_order) {
  (void)memory_order;
  const uint32_t irq_level = rtos_enter_critical();
  const unsigned long long value =
      *(const volatile unsigned long long *)address;
  rtos_exit_critical(irq_level);
  return value;
}

void __atomic_store_8(volatile void *address, unsigned long long value,
                      int memory_order) {
  (void)memory_order;
  const uint32_t irq_level = rtos_enter_critical();
  *(volatile unsigned long long *)address = value;
  rtos_exit_critical(irq_level);
}

/* The Cortex-M33 toolchain has no libatomic, so every 8-byte read-modify-write
 * the C11 atomics lower to a libcall lands here. The AP core is single-core:
 * masking interrupts is the whole memory model. */
unsigned long long __atomic_exchange_8(volatile void *address,
                                       unsigned long long value,
                                       int memory_order) {
  (void)memory_order;
  const uint32_t irq_level = rtos_enter_critical();
  volatile unsigned long long *target = (volatile unsigned long long *)address;
  const unsigned long long previous = *target;
  *target = value;
  rtos_exit_critical(irq_level);
  return previous;
}

_Bool __atomic_compare_exchange_8(volatile void *address, void *expected,
                                  unsigned long long desired, _Bool weak,
                                  int success_order, int failure_order) {
  (void)weak;
  (void)success_order;
  (void)failure_order;
  const uint32_t irq_level = rtos_enter_critical();
  volatile unsigned long long *target = (volatile unsigned long long *)address;
  unsigned long long *expected_value = (unsigned long long *)expected;
  const _Bool matched = *target == *expected_value;
  if (matched)
    *target = desired;
  else
    *expected_value = *target;
  rtos_exit_critical(irq_level);
  return matched;
}

#define H2_BK_ATOMIC_FETCH_OP_8(name, expression)                              \
  unsigned long long __atomic_fetch_##name##_8(                                \
      volatile void *address, unsigned long long value, int memory_order) {    \
    (void)memory_order;                                                        \
    const uint32_t irq_level = rtos_enter_critical();                          \
    volatile unsigned long long *target =                                      \
        (volatile unsigned long long *)address;                                \
    const unsigned long long previous = *target;                               \
    *target = (expression);                                                    \
    rtos_exit_critical(irq_level);                                             \
    return previous;                                                           \
  }

H2_BK_ATOMIC_FETCH_OP_8(add, previous + value)
H2_BK_ATOMIC_FETCH_OP_8(sub, previous - value)
H2_BK_ATOMIC_FETCH_OP_8(and, previous & value)
H2_BK_ATOMIC_FETCH_OP_8(or, previous | value)
H2_BK_ATOMIC_FETCH_OP_8(xor, previous ^ value)
