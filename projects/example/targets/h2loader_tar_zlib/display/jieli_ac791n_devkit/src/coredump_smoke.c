#include "asm/cpu.h"
#include "dual_bank_updata_api.h"

#include <stdint.h>

extern void h2_jieli_wl82_assert_reset_hook(void *caller);

/* Test-only probe: persist one record, then hard-reset without user input. */
void h2_jieli_wl82_boot_probe(uint32_t stage) {
  static uint8_t fired;
  if (stage == 105u && fired == 0u) {
    fired = 1u;
    h2_jieli_wl82_assert_reset_hook(
        (void *)(uintptr_t)UINT32_C(0x48324352));
    (void)flash_update_clr_boot_info(CLEAR_APP_RUNNING_BANK);
    system_reset();
    for (;;) {
    }
  }
}
