#ifndef H2_APP_TEST_POWER_H
#define H2_APP_TEST_POWER_H
#include "h2/pal/hal/h2_pal_power.h"
#include "h2_app_test_fault.h"
#ifdef __cplusplus
extern "C" {
#endif
/** Fake power controller. Transitions record intent and update state on
 * success; they never reboot/shut down the host or synthesize a new boot
 * identity. Configure boot_info explicitly for a separate simulated boot. */
typedef struct h2_app_test_power {
  h2_pal_power_api_t api;
  h2_pal_power_capabilities_t capabilities;
  h2_pal_power_boot_info_t boot_info;
  h2_pal_power_state_t state;
  int hold_enabled;
  uint32_t last_reason;
  h2_app_test_fault_t hold, reboot, shutdown, sleep, deep_sleep;
} h2_app_test_power_t;
/** Initialize stopped-test storage with RUNNING state and supported
 * transitions. NULL is a no-op. No allocation or destroy needed. */
void h2_app_test_power_init(h2_app_test_power_t *power);

#ifdef __cplusplus
}
#endif
#endif
