/* Host test for the ESP power provider's deep-sleep wake timer: the armed
 * delay reaches the SDK in microseconds, 0 keeps the 1 s default, and only a
 * deep-sleep reset with a timer wake cause reports BOOT_SOURCE_TIMER. */
#include "h2_esp_platform_power_wake.h"

#include <assert.h>
#include <stdint.h>

int main(void) {
  assert(h2_esp_power_deep_sleep_wake_us(0u) == 1000000ULL);
  assert(h2_esp_power_deep_sleep_wake_us(1u) == 1000ULL);
  assert(h2_esp_power_deep_sleep_wake_us(60000u) == 60000000ULL);
  /* 32-bit ms must not overflow in the conversion. */
  assert(h2_esp_power_deep_sleep_wake_us(UINT32_MAX) ==
         (uint64_t)UINT32_MAX * 1000ULL);

  assert(h2_esp_power_wake_boot_source(1, 1) ==
         H2_PAL_POWER_BOOT_SOURCE_TIMER);
  assert(h2_esp_power_wake_boot_source(1, 0) ==
         H2_PAL_POWER_BOOT_SOURCE_UNKNOWN);
  assert(h2_esp_power_wake_boot_source(0, 1) ==
         H2_PAL_POWER_BOOT_SOURCE_UNKNOWN);
  assert(h2_esp_power_wake_boot_source(0, 0) ==
         H2_PAL_POWER_BOOT_SOURCE_UNKNOWN);
  return 0;
}
