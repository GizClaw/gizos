#include "asm/cpu.h"

#include <stdint.h>

extern void h2_jieli_wl82_assert_reset_hook(void *caller);

/* Test-only probe: persist one record, then hard-reset without user input. */
void h2_jieli_wl82_boot_probe(uint32_t stage) {
  static uint8_t fired;
  if (stage == 105u && fired == 0u) {
    fired = 1u;
    h2_jieli_wl82_assert_reset_hook(
        (void *)(uintptr_t)UINT32_C(0x48324352));
    /* The canonical Loader remains ROM-selected. Preserve the failed image
     * and its attempt evidence so recovery is tested without an erase. */
    system_reset();
    for (;;) {
    }
  }
}
