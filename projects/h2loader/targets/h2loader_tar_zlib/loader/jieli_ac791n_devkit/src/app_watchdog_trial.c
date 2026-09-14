#include "asm/cpu.h"
#include "asm/wdt.h"
#include "h2_jieli_ac791n_devkit.h"

#include <stdint.h>
#include <stdio.h>

extern uint8_t wdt_rx_con(void);

/* The shared App launcher has not confirmed this trial. Exercise the normal
 * production watchdog without changing its timeout or reset mode. */
int h2_jieli_target_application_run(void) {
  char line[96];
  int length = snprintf(line, sizeof(line),
      "H2_WDT_TRIAL role=app core=%u control=0x%02x action=hang\r\n",
      (unsigned)current_cpu_id(), (unsigned)wdt_rx_con());
  if (length > 0 && (unsigned)length < sizeof(line)) {
    (void)h2_jieli_ac791n_devkit_console_write(line, (unsigned)length, 100u);
  }
  __local_irq_disable();
  for (;;) {
    __asm__ volatile("idle");
  }
}
