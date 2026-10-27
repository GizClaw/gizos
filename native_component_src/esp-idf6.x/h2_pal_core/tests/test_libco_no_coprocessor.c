#ifdef NDEBUG
#undef NDEBUG
#endif
#include "h2_esp_libco_stack_guard.h"
#include <assert.h>
/* Stand-ins for libco defaults: this profile must not emit SDK overrides. */
uint32_t h2_libco_riscv32_stack_prepare(uint32_t low, uint32_t high) {
  (void)low;
  return high;
}
uint32_t h2_libco_riscv32_stack_switch_enter(uint32_t low, uint32_t high,
                                             uint32_t* old_low,
                                             uint32_t* old_high) {
  (void)low;
  (void)high;
  *old_low = *old_high = 0u;
  return 0u;
}
void h2_libco_riscv32_stack_switch_exit(uint32_t token) { (void)token; }
int main(void) {
  h2_esp_libco_stack_guard_link();
  assert(h2_libco_riscv32_stack_prepare(0x1000u, 0x3000u) == 0x3000u);
  uint32_t low = 1, high = 1;
  h2_libco_riscv32_stack_switch_enter(0x1000u, 0x3000u, &low, &high);
  assert(low == 0 && high == 0);
  return 0;
}
