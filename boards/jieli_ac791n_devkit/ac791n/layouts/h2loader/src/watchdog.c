#include "app_config.h"
#include "asm/cpu.h"
#include "asm/p33.h"
#include "h2_jieli_wl82_atomic.h"
#include "h2_jieli_wl82_sdk_port.h"
#include "system/includes.h"

/* SDK idle and yield paths call the weak wdt_clear on both cores. One live
 * core must not conceal a stopped peer by feeding the hardware indefinitely.
 * Each feed consumes evidence of progress from both cores. */
static volatile uint32_t watchdog_progress[2] SEC(.volatile_ram);
static volatile uint8_t watchdog_feed_lock SEC(.volatile_ram);

void wdt_clear(void) SEC(.volatile_ram_code);
void wdt_clear(void) {
  const unsigned core = current_cpu_id();
  if (core >= 2u) return;
  h2_jieli_atomic_store_u32(&watchdog_progress[core], 1u);
  if (!h2_jieli_sdk_try_lock_byte(&watchdog_feed_lock)) return;
  if (h2_jieli_atomic_load_u32(&watchdog_progress[0]) != 0u &&
      h2_jieli_atomic_load_u32(&watchdog_progress[1]) != 0u) {
    h2_jieli_atomic_store_u32(&watchdog_progress[0], 0u);
    h2_jieli_atomic_store_u32(&watchdog_progress[1], 0u);
    /* Pinned wdt.c implements feeding as P33 WDT_CON bit 6. Call the RAM
     * P33 primitive directly; it serializes its bus and invokes no callback. */
    p33_or_1byte(0x80u, 0x40u);
  }
  h2_jieli_sdk_unlock_byte(&watchdog_feed_lock);
}
