#include "asm/cpu.h"

#include <stdint.h>

extern void h2_jieli_wl82_coredump_mark_loader(void);
extern void h2_jieli_wl82_assert_reset_hook(void *caller);

/* Test-only candidate: reset before app_main and the shared confirmation
 * callback. Never link this component into the production Loader package. */
void h2_jieli_wl82_boot_probe(uint32_t stage) {
  if (stage != 105u) return;
  h2_jieli_wl82_coredump_mark_loader();
  h2_jieli_wl82_assert_reset_hook(
      (void *)(uintptr_t)UINT32_C(0x48324c43));
  system_reset();
  for (;;) {
  }
}
