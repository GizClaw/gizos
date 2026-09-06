#ifndef H2_APP_TEST_PERIPH_H
#define H2_APP_TEST_PERIPH_H
#include "h2/pal/hal/h2_pal_input.h"
#include "h2/pal/hal/h2_pal_pwm_switch.h"
#include "h2_app_test_fault.h"
#ifdef __cplusplus
extern "C" {
#endif
#define H2_APP_TEST_PERIPH_MAX 32u
/** Scenario-owned input and output state for one registered peripheral. Info
 * payload is borrowed; keep it alive. Set ids/types via add, then configure
 * readings/faults only while quiescent. id in returned readings comes from
 * info. */
typedef struct h2_app_test_periph_entry {
  h2_pal_periph_info_t info;
  h2_pal_button_state_t button;
  h2_pal_battery_reading_t battery;
  h2_pal_temperature_reading_t temperature;
  uint16_t duty_x100, last_duty_x100;
  h2_app_test_fault_t read, write;
} h2_app_test_periph_entry_t;
/** Portable registry with raw Button, Battery/Temperature and PWM PAL APIs.
 * It does not map product component ids or enqueue Runtime events. */
typedef struct h2_app_test_periph {
  h2_pal_periph_api_t api;
  h2_pal_button_api_t button;
  h2_pal_input_api_t input;
  h2_pal_pwm_switch_api_t pwm;
  size_t count;
  h2_app_test_periph_entry_t entries[H2_APP_TEST_PERIPH_MAX];
} h2_app_test_periph_t;
/** Initialize caller storage. NULL is a no-op. */
void h2_app_test_periph_init(h2_app_test_periph_t *periph);
/** Copy registry metadata; payload is borrowed. Duplicate id returns
 * INVALID_STATE, full returns NO_SPACE. On success out_entry borrows storage
 * until reinit. */
h2_pal_result_t h2_app_test_periph_add(h2_app_test_periph_t *periph,
                                       const h2_pal_periph_info_t *info,
                                       h2_app_test_periph_entry_t **out_entry);

#ifdef __cplusplus
}
#endif
#endif
