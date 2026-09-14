#include "asm/cpu.h"
#include "asm/wdt.h"

/* Fault-injection App only: the shared launcher has not confirmed the trial. */
int h2_jieli_target_application_run(void) {
  __local_irq_disable();
  wdt_init(WDT_1MS);
  wdt_reset_enable();
  for (;;) {
    __asm__ volatile("idle");
  }
}
