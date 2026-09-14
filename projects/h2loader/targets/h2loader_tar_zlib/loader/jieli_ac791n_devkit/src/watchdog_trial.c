#include "asm/cpu.h"
#include "asm/wdt.h"
#include "h2_jieli_ac791n_devkit.h"

#include <stdint.h>
#include <stdio.h>

extern uint8_t wdt_rx_con(void);

/* Test the production watchdog, including its boot mode and feed ownership.
 * Do not rearm it here: that would hide a disabled/incorrect boot watchdog. */
void h2_jieli_wl82_boot_probe(uint32_t stage) {
  if (stage != 105u) return;
  char line[96];
  int length = snprintf(line, sizeof(line),
      "H2_WDT_TRIAL role=loader core=%u control=0x%02x action=hang\r\n",
      (unsigned)current_cpu_id(), (unsigned)wdt_rx_con());
  if (h2_jieli_ac791n_devkit_console_start() == 0 && length > 0 &&
      (unsigned)length < sizeof(line)) {
    (void)h2_jieli_ac791n_devkit_console_write(line, (unsigned)length, 100u);
  }
  __local_irq_disable();
  for (;;) {
    __asm__ volatile("idle");
  }
}
