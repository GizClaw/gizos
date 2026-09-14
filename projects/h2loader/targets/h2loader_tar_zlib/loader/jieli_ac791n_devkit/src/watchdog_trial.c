#include "asm/cpu.h"
#include "asm/wdt.h"

#include <stdint.h>

/* Fault-injection component only: stop before trial confirmation. The valid
 * P1 Loader remains the recovery owner. Never link into production packages. */
void h2_jieli_wl82_boot_probe(uint32_t stage) {
  if (stage != 105u) return;
  __local_irq_disable();
  wdt_init(WDT_1MS);
  wdt_reset_enable();
  for (;;) {
    __asm__ volatile("idle");
  }
}
