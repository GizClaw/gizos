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
